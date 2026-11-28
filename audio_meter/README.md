# Audio Meter

Test program for the microphones. It shows each mic's loudness as a live bar
in the terminal, and has a benchmark mode that compares sample rate and
reading quality at several SPI clocks.

Example output (the numbers depend on the board and SPI clock):

```
Microphone amplitude meter on ADC channels 0 1. Press Ctrl+C to stop.

mic 0 (ch 0) [################                        ]  40%  amp:  812  raw: 2860
mic 1 (ch 1) [######                                  ]  15%  amp:  301  raw: 2349
12765 Hz per mic | mic 1 lags 39.2 us | blocks 11889 | disturbed 6 | max gap 154 us | ADC errors 0
```

The sampling code is in the shared `audio` library (see `audio/README.md`);
this folder only has the demo and the `level_bar` display.

## Build & run

Built with the rest of the project (from the repo root):

```shell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build --target audio_meter   # also copies to ~/ensc351/public/myApps/audio_meter
```

On the BeagleY-AI (as root, for SPI access; needs the setup in `overlays/README.md`):

```shell
sudo ./audio_meter            # meter at the default 1 MHz SPI clock
sudo ./audio_meter 1250000    # meter at a 1.25 MHz SPI clock (higher sample rate)
sudo ./audio_meter --bench    # compare rate and reading quality at 1, 1.25, 1.5 and 2 MHz
sudo ./audio_meter --spi-diag # measure the real SPI clock and where each conversion's time goes
```

At 3.3 V stay at or below 1.25 MHz; see `audio/README.md`.

## Level bar

`level_bar.h` draws any 0.0 to 1.0 value as a text bar:

- `LevelBar_format()` writes one bar into a string.
- `LevelBar_print()` redraws one bar on the current line.
- `LevelBar_printMulti()` redraws several bars (one per mic) and a status line
  in place, using ANSI cursor movement.

## Tuning the meter (`AudioLevel_Config`)

| Field | Default | Notes |
|---|---|---|
| `windowMs` | 20 | Peak amplitude is measured over this window. |
| `fullScaleCounts` | 2048 | Amplitude (in ADC counts) that shows as 100%. **Lower it (e.g. 600) if the bar barely moves.** |
| `releaseMs` | 500 | How long the bar takes to fall from 100% back to 0%. |

## Troubleshooting

- **`raw` stays near 0 or 4095**: wrong channel or mic not powered.
- **`raw` hovers around 2048 but the bar never moves**: mic signal is small; lower `fullScaleCounts`.
- **`no reserved CPU core`**: boot with `isolcpus=3`; see `overlays/README.md`.
- **`unable to open /dev/spidev0.0`**: SPI overlay not enabled, or no permission to access SPI.
