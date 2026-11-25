# Audio library (Linux reserved-core version)

Background, measurements and what comes next: [CONTEXT.md](CONTEXT.md).

Samples the node's two microphones through the MCP3208 ADC as fast as the
hardware allows, and hands the raw samples to any code that needs them
(localization, level meter, recording).

| Module | Header | Purpose |
|---|---|---|
| Audio capture | `audio/audio_capture.h` | Samples ADC channels 0 and 1 into timestamped, evenly spaced blocks. **Use this for signal analysis.** |
| Audio level | `audio/audio_level.h` | Example consumer: loudness (0.0 to 1.0) of each mic, for meters and LEDs. |

Link your target with `audio` (it brings in `hal`):

```cmake
target_link_libraries(node LINK_PRIVATE audio)
```

## Getting samples for analysis

```c
#include "audio/audio_capture.h"

AudioCapture_init(NULL);   // defaults: ADC ch 0 = mic 0, ch 1 = mic 1, 128-frame blocks

AudioCapture_Reader *reader = AudioCapture_openReader();
int channels = AudioCapture_getNumChannels();
uint16_t *samples = malloc(AudioCapture_getBlockFrames() * channels * sizeof(uint16_t));

AudioCapture_BlockInfo info;
while (AudioCapture_readBlock(reader, samples, &info, -1) == AUDIO_CAPTURE_OK) {
    if (info.blocksSkipped > 0) {
        // This reader fell behind and lost blocks: reset anything that
        // assumes a continuous signal
    }
    if (info.timingDisturbed) {
        // Samples in this block may not be evenly spaced: skip it for
        // timing-sensitive work such as direction finding
    }
    double mic1LagNs = info.framePeriodNs / channels;   // mic 1 is sampled this long after mic 0
    for (int f = 0; f < info.numFrames; f++) {
        uint16_t mic0 = samples[f * channels + 0];
        uint16_t mic1 = samples[f * channels + 1];
        double timeNs = info.tFirstNs + f * info.framePeriodNs;   // CLOCK_REALTIME of mic 0's sample
        ...
    }
}

AudioCapture_closeReader(reader);
free(samples);
AudioCapture_cleanup();
```

Each reader gets every block independently, so the analysis, the level meter
and a recorder can all run at once. The sampling thread never waits for
readers. A reader that falls more than about 1.5 s behind skips ahead and
sees `blocksSkipped > 0`.

**Use `info.framePeriodNs` from each block, not a fixed sample rate.** It is
measured for each block.

## How sampling works

Each block (a frame is one sample from each mic) is read with **one** SPI
message, with all the conversions back to back:

- **Samples inside a block are evenly spaced**, unless an interrupt paused
  the transfer partway through. The code measures every block and sets
  `timingDisturbed` when its timing was off by more than 5%.
- **Mic 1 is sampled half a frame after mic 0** (conversions are evenly
  spaced, so channel k lags by k * framePeriodNs / numChannels). This offset is
  the same in every frame: subtract it from the measured time difference
  between the mics. A sound source at equal distance from both mics should give
  exactly this offset, which is a good check.
- **There is a short gap between blocks**, the time between SPI calls
  (normally a few microseconds). Each block reports `gapBeforeNs`.

### Timestamps

`tFirstNs` is the `CLOCK_REALTIME` time of the block's first sample, estimated
from the time around the SPI call. With the boards' clocks synced, timestamps
from different nodes can be compared.

### Data format

Raw 12-bit counts, 0 to 4095. A silent mic sits at its bias point (about 1350
on the current hardware, not mid-scale); subtract the real DC offset, for
example the block mean, before an FFT or correlation.

## Sample rate on the BeagleY-AI

Measured with two channels, hardware SPI (MCU_SPI0) and a reserved core:

| SPI clock | Samples/s per mic | Mic 1 lag |
|---|---|---|
| 1 MHz (default) | 12,792 | 39.1 us |
| 1.25 MHz | 14,205 | 35.2 us |

At 1 MHz each conversion takes about 39 us: 19 clocks plus about 18 us of
fixed cost in Linux, mostly its 10 us chip-select delay. Stay at or below
1.25 MHz on a 3.3 V supply.

Capture runs on a CPU core reserved with `isolcpus=`, because the SPI driver
polls and keeps that core 100% busy. `AudioCapture_init()` fails if no core is
reserved. Set up the board with `overlays/README.md`.

### Checking the setup

```
sudo ./audio_meter --spi-diag   # real SPI clock and where the time goes
sudo ./audio_meter --bench      # rate, gaps, disturbed blocks, noise at each SPI clock
readlink /sys/class/spi_master/spi*/device/driver   # want omap2_mcspi
```

Run as root, for access to SPI.

## Raising the spidev buffer size (recommended)

spidev copies each SPI message through a buffer of `spidev.bufsiz` bytes,
4096 by default. It rounds every transfer up to 128 bytes (the arm64 DMA
alignment), so even though one conversion is only 3 or 4 bytes, by default
**only 32 conversions (16 frames for two mics) fit in one message**. The code
detects this and shrinks the blocks to 16 frames. That works, but it means a
gap between blocks after every 16 samples.

Raising the buffer to 64 KiB allows 128-frame blocks (the default; up to 255).
Check the current size:

```
cat /sys/module/spidev/parameters/bufsiz
```

It can only be set when spidev loads, so it needs a reboot. Do one of these:

- **If spidev is a loadable module** (`/sys/module/spidev/initstate` exists):
  ```
  echo "options spidev bufsiz=65536" | sudo tee /etc/modprobe.d/spidev.conf
  sudo reboot
  ```
- **If spidev is built into the kernel** (no `initstate` file): add
  `spidev.bufsiz=65536` to the kernel command line (the `append` line in
  `/boot/firmware/extlinux/extlinux.conf`), then reboot.

## Other notes

- While a block is being read, other users of the ADC, such as a joystick on
  another channel, wait for it to finish.
- Blocks hold at most 255 frames for two mics (Linux allows 511 transfers per
  SPI message, given a big enough spidev buffer).
