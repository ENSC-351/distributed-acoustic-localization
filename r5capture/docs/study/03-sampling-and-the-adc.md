# 3. Sampling, the ADC, and mic spacing

## Why sample rate matters (timing, not sound quality)

We measure a delay of at most d/c (0.29 ms at 10 cm). Samples are 1/fs apart, so the
whole range of possible delays is only **fs·d/c samples** wide. The angle change for a
1-sample change in delay is largest near broadside: `Δθ ≈ asin(c / (fs·d))`.

| Rate per mic | Time per sample | Max delay at 10 cm | One sample near broadside |
|---|---|---|---|
| 17,200/s (Linux sampler, measured) | 58 µs | 5.0 samples | ~11.5° |
| 25,000/s (1 MHz SPI, *estimate*) | 40 µs | 7.3 samples | ~7.9° |
| 31,000/s (1.25 MHz SPI, *estimate*) | 32 µs | 9.0 samples | ~6.4° |

Three reasons higher fs helps:
1. **Finer ruler.** Sub-sample interpolation beats whole samples, but with noise its
   error scales with sample spacing.
2. **Sharper pulse.** The impact onset is what correlation locks onto; more samples on
   that edge = narrower correlation peak = easier to locate, harder to confuse with reflections.
3. **Less aliasing** (below).

Not fixed by fs: mic placement error, reflections, noise, skew-correction error.

## Aliasing

A sampled signal can only represent frequencies below **fs/2** (Nyquist): 8.6 kHz today.
Energy above that doesn't disappear; it **folds back** as fake lower frequencies,
distorting the waveform, and differently on each mic. The usual fix is an analog
low-pass (anti-aliasing) filter before the ADC. **Our preamp has none yet.**

## Mic spacing: wider helps, up to a limit

Wider spacing = bigger delay = every timing error costs fewer degrees. At 17,200/s:

| Spacing | Max delay | One sample near broadside |
|---|---|---|
| 10 cm | 5.0 samples | ~11.5° |
| 20 cm | 10.0 samples | ~5.7° |
| 30 cm | 15.0 samples | ~3.8° |

Doubling spacing ≈ halves angle error: a bigger lever than the R5's ~1.5× rate.

Limits:
1. **Wrong-cycle confusion (spatial aliasing).** A ringing impact at frequency f has a
   correlation peak every cycle (1/f). Above spacing ≈ **c/(2f)**, more than one peak
   fits in the allowed range and the estimator can pick the wrong one: a large error,
   not a small one. 1 kHz → 17 cm, 1.5 kHz → 11 cm, 2 kHz → 8.6 cm, 3 kHz → 5.7 cm.
   Seen in tests: at 30 cm, a 1.5 kHz tone had a second peak at 58 % of the main one.
   Broadband clicks are much more forgiving.
2. **Near-field.** Close impacts (within ~1 m) bias a 30 cm pair.
3. **Coherence.** Farther apart, the mics hear more different reflections.

**Decision rule:** record a real marble impact, look at its spectrum. Broadband → go
wider (20–30 cm); ringing → stay under c/(2f) or gate on the onset.

## The MCP3208

12-bit (0–4095), 8 channels, SPI. One conversion = **19 clocks** minimum (Joey sends a
single 19-bit SPI word). Chip select must go high between conversions.

**Command format** (3-byte version, used in fw/mcp3208.c):
```
send: 00000 START SGL D2 | D1 D0 xxxxxx | xxxxxxxx      (START=1, SGL=1 single-ended)
recv: ???????? | ??? 0 B11 B10 B9 B8 | B7 … B0           (0 = null bit)
```
The 5 leading zeros make the 12-bit result end exactly on the last byte. Decoding masks
`rx[1] & 0x0F`, because the other bits are undefined on the real chip (a test feeds junk
there on purpose).

**Speed ceiling** (2 conversions per frame):

| SPI clock | Per conversion | Rate per mic | Status |
|---|---|---|---|
| ~0.68 MHz (Linux bit-banging, today) | ~28 µs | 17,200/s | measured |
| 1.0 MHz | ~19–20 µs | ~25,000/s | in spec (rated 1 MHz at 2.7 V) |
| 1.25 MHz | ~15–16 µs | ~31,000/s | edge of spec at 3.3 V (interpolated); check accuracy |
| 2.0 MHz | ~10 µs | ~50,000/s | out of spec at 3.3 V (rated at 5 V) |

Realistic ceiling ≈ **1.5–1.8× today**. To go faster, the clock must go faster, which
needs a hardware SPI controller (MCU_SPI0 is probably on these pins; to confirm).
Ways: (A) bigger spidev buffer (fewer gaps, same clock), (B) hardware SPI on Linux,
(C) R5 + hardware SPI, (D) R5 bit-banging GPIO.

## Multiplexer skew

The MCP3208 has **one** converter behind an 8-to-1 multiplexer, so mic 1 is always
sampled one conversion after mic 0: **27.6 µs** on our board. Sound travels ~1 cm in
that time, which at 10 cm spacing is several degrees. It's constant, so the estimator
corrects it (`channel_skew_s`). A simultaneous-sampling ADC (two converters on one
clock) wouldn't need this; a stereo audio codec is that kind of device, but our board
has no audio input without the course's Zen Hat.

## Bias point

The schematic re-biases the ADC input to mid-supply (R4 = R7 = 100 kΩ), which should
read ~2048; Joey's README says a silent mic reads ~1350. Open question for Joey. It
doesn't affect direction finding (the mean is subtracted) but reduces headroom.
