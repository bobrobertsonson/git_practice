# RASP — a hardware vocal processor for extreme vocals

A floor box for vocalists who scream, growl and squeal. It makes a real
extreme vocal sound like a produced record, live and with no audible
latency. It's modeled on the way screamer-style plugins (such as Ablaze
Screamer) are publicly described.

**It does not turn clean singing into a scream.** You bring the technique;
the box adds grit, control, size and space.

| Path | What it is |
|---|---|
| `dsp/rasp.h` | The DSP engine. Header-only C++17, no heap, no dependencies |
| `host/rasp_wav.cpp` | Desktop tool: process a WAV file |
| `daisy/main.cpp` | Firmware for an Electrosmith Daisy Seed (20 pots, 2 toggles, 4 footswitches) |
| `playground/index.html` | Playable browser preview, a JS port of `dsp/rasp.h` that matches it to −71 dB |
| `tools/probe.py` | Measurement kit for matching RASP to a reference plugin (section 5) |

```sh
make
./rasp_wav my_scream.wav out.wav drive=0.5 bright=2 chop=0.6 spread=0.6 tailblend=0.3
./rasp_wav                      # lists every parameter
```

---

## 1. Signal chain

```
             ┌──────────────── dry (control out) ────────────────┐
mic ─► CONTROL ─┼─► THICKEN  stereo comb + diffusion (parallel) ───┼─► sum ─► L/R out
             └─► FLAVOUR  reverb + delay, ducked   (parallel) ───┘
```

| Section | Controls | What it does |
|---|---|---|
| **Control** (series) | lo cut, hi cut | 24 dB/oct Butterworth high- and low-pass filters |
| | drive, tone switch, mix | Tube-Screamer-style overdrive. Gain is applied only above ~720 Hz, through soft diode-pair clipping that is added back onto the clean signal. A feedback-cap rolloff tames the top as drive rises. Tone switch: dark / natural / bright. 4× oversampled with anti-aliasing on the clipper: aliasing measures −75 dB at full drive. Level-matched, so drive and mix change texture, not volume |
| | chop, release | Soft-knee compressor. One knob sets threshold (−6 to −36 dBFS) and ratio (1.5:1 to 8:1) together, with auto makeup gain. A −1 dBFS peak limiter follows it. Release runs from 25 to 600 ms. The LED lights above 3 dB of gain reduction |
| **Thicken** (parallel) | spread | Four fixed short taps per side, with different spacings on the left and right (3–26 ms). No modulation, so no chorus wobble |
| | diffuse, darken, width, solo | Allpass smearing, lowpass, mid/side width, and a solo for the branch alone |
| **Flavour** (parallel) | tail + blend | 4-line feedback-delay-network reverb, RT60 from 0.4 to 6 s, with 15 ms pre-delay |
| | repeats + blend, time switch | Ping-pong delay at 1/8, dotted 1/8 or 1/4 of the tapped tempo. Each repeat gets darker |
| | tilt, duck | ±6 dB tilt EQ on the effect, and ducking of the effect while the vocal is present |
| Faders | control, thicken, flavour, output | Section levels |

## 2. Latency

- **No buffering and no lookahead in the DSP.** The only delay is filter
  phase delay in the drive path. That's about 4 samples from the
  oversampling filters plus the tone filter: 7 samples (0.15 ms) measured at
  3 kHz. An analog tone control delays the signal in the same way.
- **Daisy Seed with 4-sample blocks:** buffering adds 0.17 ms on top of the
  converter delay. I expect about 1 ms round trip, but this still needs
  measuring on a board.
- **Hardware:** a true-bypass relay (the vocal still reaches the PA if the box
  loses power), plus a headphone/IEM out so you can monitor at the box's own
  latency.

## 3. Feedback live

Chop is a compressor, so it raises quiet sounds, including room noise and
wedge spill, and that makes feedback more likely. Defenses:
keep **lo cut** up, use a dynamic cardioid mic (SM58, Beta 58A, e835), keep
the wedge in the mic's null, use in-ears instead of a wedge, and use
**duck** to keep the effect tails out from under your voice. Cupping the
mic removes its pickup pattern and makes feedback much worse.

Possible additions: a downward expander before the compressor, and an
automatic howl notch.

## 4. Hardware

**Platform: Electrosmith Daisy Seed** (STM32H750, 480 MHz Cortex-M7 with a
double-precision FPU, 64 MB SDRAM, audio codec on board).

- On desktop the engine runs at 42× real time. The anti-aliased drive is
  the heaviest part. **Daisy CPU use hasn't been measured yet.** If it's
  high, the first fixes are faster `exp`/`log1p` approximations in the
  clipper, or 2× oversampling plus ADAA instead of 4×.
- Delay lines use about 1.8 MB, so the processor object lives in SDRAM.
- **Panel:** 16 knobs through two CD4051 multiplexers, 4 level faders,
  2 three-way toggles, a solo button, 4 footswitches and LEDs. The full pin map
  is in `daisy/main.cpp`. The previous version had a pin clash: D15 is also A0.
- **Analog:** mic preamp (THAT1512 or INA217, 40–60 dB, optional +48 V
  phantom), XLR in, XLR L/R out, a headphone amp, a bypass relay, a ground lift
  and 9 V center-negative power.
- Rough parts cost: about $150. A 1590DD-size enclosure or a small desktop case fits it.

## 5. Matching a reference plugin by measurement

If you own the reference plugin, use it as a **black box**: play known test
signals through it and measure what comes out. Don't decompile it.
`tools/probe.py` does this:

```sh
python3 tools/probe.py make probe.wav
# In a DAW at 48 kHz: put the plugin on probe.wav and bounce it to stereo WAV,
# once per setting. Name the file after the settings: ref_drive50.wav
./rasp_wav probe.wav rasp.wav drive=0.5 chop=0 thicken=0 flavour=0
python3 tools/probe.py analyze ref_drive50.wav --ref rasp.wav
```

| Measured | Matches |
|---|---|
| Frequency response at −30 dBFS | lo/hi cut, the drive's mid focus, tone switch |
| Input/output level curve, attack and release | chop and release |
| Harmonic profile at −12 dBFS (H2–H7) | the drive's clipping character |
| Early tap times per side | thicken spread (comb spacings) |
| RT60, strongest echo time | tail, delay time |
| Effect level in the gaps vs. under the vocal | duck |

Measure one section at a time. To measure the drive, use control only with
chop off. To measure chop, set drive and mix to 0. To measure thicken, use
solo. For flavour, leave the dry path on.

Use your ears too: render the same dry scream through both at matched
settings and A/B them.

**Legal side (not legal advice):** plugin license agreements normally forbid
decompiling and reverse engineering, and the code is copyrighted. Matching
how a product *sounds* by measuring it is how most hardware "clones" are
developed. That still doesn't let you use the other company's name,
graphics or presets. If you plan to sell the box, ask the developer about
licensing their algorithm.

## Status

- Verified on desktop:
  - Output is stable and bounded at 48 and 96 kHz.
  - Drive aliasing is −75 dB at full drive.
  - The compressor takes a −30/−6 dBFS input pair to −22/−9 dBFS out.
  - The drive's auto-level holds output within 0.3 dB from drive 0 to 1.
  - The JS port matches the C++ engine to −71 dB or better, even at extreme settings.
- Two bugs found and fixed during the C++/JS cross-check:
  - The float control smoother stalled short of large targets, which left the delay time 3.75 samples off.
  - The delay read lost fractional precision at long delay times.
- **Not done:**
  - Not tuned against real screams or a real Screamer. Use section 5 for that.
  - `daisy/main.cpp` hasn't been compiled or run on hardware yet.
