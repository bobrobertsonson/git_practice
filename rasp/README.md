# RASP — a hardware vocal processor for extreme vocals

A floor box for vocalists who scream, growl and squeal. It's a full vocal
chain: the processing a mix engineer would put on a big metal vocal, live and
with no audible latency. The panel follows the way screamer-style plugins
(such as Ablaze Screamer) are publicly described.

**It does not turn clean singing into a scream.** You bring the technique;
the box adds grit, control, size and space.

| Path | What it is |
|---|---|
| `dsp/rasp.h` | The DSP engine. Header-only C++17, no heap, no dependencies |
| `host/rasp_wav.cpp` | Desktop tool: process a WAV file |
| `daisy/main.cpp` | Firmware for an Electrosmith Daisy Seed (16 knobs, 4 faders, 3 trims, 2 toggles, 4 footswitches) |
| `playground/index.html` | Playable browser preview, a JS port of `dsp/rasp.h` that matches it to −76 dB |
| `tools/probe.py` | Measurement kit for matching RASP to a reference plugin (section 6) |

```sh
make
./rasp_wav my_scream.wav out.wav drive=0.45 chop=0.65 spread=0.75 tailblend=0.45 glue=0.65 out=3
./rasp_wav                      # lists every parameter
```

---

## 1. The chain

```
                ┌──────────────────── dry (control out) ────────────────────┐
mic ─► CONTROL ─┼─► THICKEN  doubles + stereo combs        (parallel) ──────┼─► BUS ─► L/R out
                └─► FLAVOUR  plate + delay, ducked          (parallel) ──────┘
```

| Stage | Controls | What it does, and why it's there |
|---|---|---|
| **Control** | lo cut, hi cut | 24 dB/oct high- and low-pass filters. Removes rumble and handling noise, and fizz above the voice |
| | drive, mix | Tube-Screamer-style overdrive. Gain is applied only above ~720 Hz, through a soft diode-pair clip added onto the clean signal, with a cap rolloff as drive rises. This gives grit in the mids without fizz. 4× oversampled with anti-aliasing on the clipper (aliasing −72 dB at full drive). Level-matched, so drive and mix change texture, not volume |
| | tone switch | Sets the drive's tone filter **and** a mix-style voicing EQ: chest-weight shelf, a cut in the boxy 420–500 Hz region, a 2.6–3.5 kHz presence lift, and an air shelf. Dark suits gutturals; bright suits highs and squeals |
| | *de-esser* (trim) | A dynamic bell at 7 kHz, up to 8 dB. Drive plus presence plus air would otherwise make every "s" and "t" spit |
| | chop, release | Three compressors on one knob, the way produced screams are usually compressed: a FET-style peak compressor (fast, catches spikes), an opto-style leveler (slow, two-stage release, evens out phrases), and a parallel "crush" blended underneath for density. The LED lights above 3 dB of gain reduction |
| | *bleed reducer* (trim) | Downward expander (1:4 below threshold, up to −40 dB), keyed on the clean mic signal. Compression lifts stage bleed; this turns it back down between phrases. Its own LED shows when it's working. See section 3 |
| **Thicken** | spread | **Two micro-pitched doubles** (±3–18 cents, 14 and 21 ms, one per side): the classic double-tracked sound and the main source of size. Also stereo comb taps (3–26 ms) |
| | diffuse, darken, width, solo | Allpass smearing of the combs, lowpass on the branch, mid/side width, and a solo for the branch alone |
| **Flavour** | tail + blend | 8-line feedback-delay-network plate, RT60 0.5–6 s. 25 ms pre-delay, slight modulation so long tails don't ring metallic, and a 300 Hz–8 kHz return EQ |
| | repeats + blend, time switch | Ping-pong delay at 1/8, dotted 1/8 or 1/4 of the tapped tempo. Each repeat gets darker |
| | tilt, duck | ±6 dB tilt on the effect. Duck pulls the effect down while you sing, so tails bloom in the gaps instead of washing out the words |
| **Bus** | *glue* (trim), output fader | Stereo-linked 2:1 glue compressor into a −1 dBFS peak limiter. Pushing the output fader drives the limiter, which makes the vocal louder and denser without clipping. The LED lights when it's limiting |

### Measured on a synthetic scream (raw → default → "Huge" preset)

| | Raw | Default | Huge |
|---|---|---|---|
| Loudness (RMS) | −18.1 dB | −12.8 dB | −11.6 dB |
| Crest factor (lower = denser) | 13.7 dB | 11.6 dB | 10.6 dB |
| Stereo width (side vs. mid) | mono | −7.9 dB | −6.0 dB |
| Peak | −4.4 dBFS | −1.0 dBFS | −1.0 dBFS |

These numbers don't show whether it sounds good. Your ears and real takes
decide that. **Send raw tracks and I'll tune against them.**

## 2. Latency

- **No buffering and no lookahead in the DSP.** The dry path has only
  filter phase delay. An impulse peaks 6 samples (0.13 ms) late because of the
  EQ, the same way an analog tone control delays the signal.
- The doubles, combs, plate and delay sit on parallel branches, so their
  delays shape the sound without making the voice late.
- **Daisy Seed with 4-sample blocks:** buffering adds 0.17 ms on top of the
  converter delay. I expect about 1 ms round trip; still to be measured on a board.
- **Hardware:** a true-bypass relay (the vocal still reaches the PA if the box
  loses power), plus a headphone/IEM out so you can monitor at the box's own latency.

## 3. Live use: bleed and feedback

A produced vocal chain is heavily compressed. Live, that means drums, guitar
amps and wedges in your mic get louder relative to your voice. That makes
the mix muddier and feedback more likely.

Test with a synthetic scream plus drum-like bleed 28 dB below it:

| | Voice-to-bleed separation |
|---|---|
| Input | 28.4 dB |
| Full chain, bleed reducer off | 17.9 dB |
| Full chain, bleed reducer at −39 dBFS | **24.8 dB** |

Setting it at soundcheck: with the band playing and you silent, raise the
**bleed trim** until the bleed LED stays lit. Then sing your quietest part
and check the LED goes out. Also on stage:
- Use the **Live** preset. It has a shorter plate, less delay, more duck,
  a higher lo cut and less glue.
- Use a dynamic cardioid mic (SM58, Beta 58A, e835) and keep the wedge in
  its null. In-ears beat wedges.
- Cupping the grille removes the mic's pickup pattern and invites feedback.

Planned: an automatic howl detector (it finds a narrow, growing peak and
notches it).

## 4. Hardware

**Platform: Electrosmith Daisy Seed** (STM32H750, 480 MHz Cortex-M7 with a
double-precision FPU, 64 MB SDRAM, audio codec on board).

- **CPU:** the engine runs at 26× real time on desktop. The anti-aliased drive is
  about a third of that cost. Gain math uses fast log/exp approximations
  (error < 0.01 dB). Daisy CPU use is **not measured yet**. My estimate is
  20–40% at 48 kHz. If it's high, the drive can fall back to 2× oversampling.
- **Memory:** 1.4 MB of delay lines, placed in SDRAM.
- **Precision:** smoothers and slow oscillator phases are double precision.
  As 32-bit floats they drifted measurably: a delay time stalled 3.75
  samples short, and the doubles' pitch drifted.
- **Panel:** 16 knobs through two CD4051 multiplexers, 4 level faders,
  3 trims (bleed, de-ess, glue), 2 three-way toggles, a solo button,
  4 footswitches, and 7 LEDs through a 74HC595 shift register. The pin map is
  in `daisy/main.cpp`.
- **Analog:** mic preamp (THAT1512 or INA217, 40–60 dB, optional +48 V
  phantom), XLR in, XLR L/R out, a headphone amp, a bypass relay, a ground lift
  and 9 V center-negative power.
- Rough parts cost: about $160. A 1590DD-size enclosure or a small desktop case fits it.

## 5. Presets (playground)

| Preset | Use |
|---|---|
| Huge (album) | More drive, chop, doubles, plate and glue; output pushed into the limiter |
| Scream natural | The default |
| Scream dark | Gutturals and lows: dark voicing, more drive, darker thicken |
| Scream bright | Highs and squeals: bright voicing, more de-essing |
| Live stage | Tighter effects, more duck, a higher lo cut, bleed reducer set higher |
| Raw | Filters and light compression only |

## 6. Matching a reference plugin by measurement

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

It measures frequency response, the input/output level curve, attack and
release, harmonic profile, early tap times per side, RT60, echo time and
ducking depth. Measure one section at a time.

**Legal side (not legal advice):** plugin license agreements normally forbid
decompiling and reverse engineering, and the code is copyrighted. Matching
how a product *sounds* by measuring it is how most hardware "clones" are
developed. That still doesn't let you use the other company's name,
graphics or presets. If you plan to sell the box, ask the developer about
licensing.

## Status

- **Verified on desktop:**
  - Output is stable and bounded at 48 and 96 kHz, even at extreme settings.
  - Drive aliasing is −72 dB at full drive.
  - The de-esser takes 5.2 dB off pure sibilance.
  - The doubles measure ±12 cents at the default spread.
  - The JS port matches the C++ engine to −76 dB or better.
- **Not done:**
  - Not tuned against real screams or a real Screamer. Raw tracks and `tools/probe.py` are the next step.
  - `daisy/main.cpp` hasn't been compiled or run on hardware yet.
