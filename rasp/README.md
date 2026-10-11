# RASP — a hardware extreme-vocal processor

A stompbox / desktop box that turns a clean (or half-committed) vocal into
fry screams, gutturals and high shrieks in real time. It's in the same spirit
as software "screamer" vocal plugins, built for a stage pedalboard.

This folder contains:

| Path | What it is |
|---|---|
| `dsp/rasp.h` | The whole DSP engine. Header-only C++17, no heap, no dependencies |
| `host/rasp_wav.cpp` | Desktop test tool: process a WAV file so you can tweak by ear |
| `daisy/main.cpp` | Firmware for an Electrosmith Daisy Seed (knobs, footswitch, mode toggle) |
| `Makefile` | Builds the desktop tool |

```sh
make
./rasp_wav dry_vocal.wav screamed.wav 1 0.8 0.7 0.6 0.2 0.3
#          in           out          mode drive fry throat vowel air
# mode: 0 = Fry, 1 = Growl (guttural), 2 = Squeal (high)
```

---

## 1. What makes a vocal sound "extreme"

Real screaming isn't just distortion. Several things happen in the throat
at the same time, and each one can be faked with a DSP block:

| What the voice does | What you hear | DSP block in RASP |
|---|---|---|
| False vocal folds / arytenoids vibrate at a sub-multiple of the pitch | Octave-down "false chord" weight | **Fry layer:** flip-flop subharmonic generator |
| Chaotic, irregular fold vibration | Rattle, growl, "gravel" | **Fry layer:** jittered amplitude modulation (20–90 Hz) |
| Narrow, constricted vocal tract | Nasal/"pinched" or cavernous vowel | **Throat:** two resonant formant band-passes + low-mid push |
| Overdriven, clipped airflow | Grit, harmonics, "tear" | **Grit:** asymmetric tube clip → sine wavefolder, 2× oversampled |
| Turbulent air through the folds | Breath, hiss, "air" | **Air:** band-passed noise that follows the input envelope |
| Constant intensity | Loud, upfront, consistent level | Envelope-driven drive + glue compressor + noise gate |

The key trick is **envelope following**: the harder the singer pushes, the
more drive goes into the grit stage. It *responds* like a real scream rather
than acting as a static fuzz.

### Signal chain

```
in ─► HPF 90Hz ─► gate ─┬─► envelope ("push") ─────────────┐ (modulates drive)
                        │                                   ▼
                        └─► FRY ──► THROAT ──► GRIT (2x OS) ──► + AIR ──► tilt EQ ──► comp ──► mix ──► clip ──► out
                            sub-octave  formants   tanh→fold      noise
                            + rattle AM
```

### Controls (front panel)

| Knob | Range | Does |
|---|---|---|
| **Drive** | 0–1 | Saturation amount |
| **Fry** | 0–1 | Sub-octave and rattle depth |
| **Throat** | 0–1 | Formant resonance (Q and level) |
| **Vowel** | 0–1 | "oo" (dark, guttural) to "ee" (bright, shriek) |
| **Air** | 0–1 | Breath/hiss layer |
| **Mix** | 0–1 | Dry/wet. Blend under a real scream for "assist" mode |
| Mode toggle | 3-way | Fry / Growl / Squeal (biases formants and rattle) |
| Footswitch | | Bypass |

---

## 2. What could run it

All three blocks are cheap except the oversampled saturator. The whole chain
is roughly 200–400 floating-point operations per sample, so any modern
Cortex-M7 handles it easily. On a desktop CPU it runs at about **120× real time**.

| Platform | CPU | Why choose it | Drawbacks | Approx. cost |
|---|---|---|---|---|
| **Electrosmith Daisy Seed** ⭐ | STM32H750, 480 MHz M7, 64 MB SDRAM, 24-bit codec on board | Made for audio pedals. ~1 ms latency, C++, firmware already in `daisy/`. Room for 4× oversampling or a small neural model | Needs your own enclosure, preamp and PCB | ~$30 for the module |
| **Daisy Petal / PedalPCB "Terrarium"** | Same Daisy Seed | Ready-made pedal PCB with 6 knobs, 4 toggles and 2 footswitches. Fastest way to get a real pedal | Guitar-level input, so you still need a mic preamp | ~$60–150 |
| **Teensy 4.1 + Audio Shield** | iMXRT1062, 600 MHz M7 | Huge community, Teensy Audio library, easy USB-audio | 16-bit codec by default, ~3 ms latency at stock block size | ~$45 |
| **ESP32-S3 + I2S codec (e.g. ES8388)** | 240 MHz dual-core Xtensa | Very cheap, Wi-Fi/BT for an app-controlled preset editor | Weaker FPU, so you may need to drop oversampling | ~$15 |
| **Bela / Raspberry Pi 4/5 + HAT** | ARM Cortex-A, Linux | Enough power for **neural "scream transfer"** models (RTNeural/ONNX), plus a screen and presets | Boot time, higher power, more latency than bare metal unless you use Bela | ~$80–200 |
| **Analog Devices SHARC / ADAU1467 (SigmaDSP)** | Dedicated DSP | What commercial vocal processors use. Lowest latency | Proprietary tools, steep learning curve | $$ |

**Recommendation:** prototype on a **Daisy Seed** (or a Terrarium-style
pedal PCB) and add a dedicated mic preamp in front of it.

### Hardware you need beyond the brain

A vocal pedal is different from a guitar pedal because the input is a **microphone**:

- **Mic preamp:** XLR in with ~40–60 dB gain and **+48 V phantom** for condensers.
  A THAT1512 or INA217 instrument-amp preamp is a solid choice. Many vocalists
  use SM58-type dynamics, so you can skip phantom power on v1.
- **XLR out** (balanced, mic level) so it sits between the mic and the
  stage snake/mixer like a TC-Helicon VoiceTone. Add a ¼" line out too.
- **Ground-lift switch:** stage audio always has ground loops.
- **Power:** 9 V DC center-negative (pedalboard standard). Phantom power
  needs a small 9 V → 48 V boost converter.
- **Enclosure:** 1590BB or 125B-size, with 6 knobs, 1 toggle, 1–2 footswitches and an LED.

Rough v1 bill of materials: Daisy Seed $30, preamp parts $15, XLR jacks
$10, enclosure $15, knobs/switches $15, PCB $10. **About $95.**

---

## 3. Features worth adding later

- **"Assist" mode:** footswitch momentary. Blend the effect only while it's held, for the scream parts of a song.
- **Pitch-tracked subharmonic:** replace the flip-flop with a YIN/autocorrelation
  pitch tracker. The sub then stays locked on messy input.
- **Double-tap layering:** a second detuned and delayed copy for a "gang vocal" scream stack.
- **Neural mode (Pi/Bela or Daisy):** train a small GRU/LSTM (see
  [RTNeural](https://github.com/jatinchowdhury18/RTNeural)) on clean-vs-screamed
  pairs from the same singer. Daisy can run a tiny model (hidden size ~8–16).
  Bigger ones need a Pi-class CPU.
- **Presets + MIDI:** store presets in QSPI flash and switch them over MIDI program change from a
  backing-track rig.
- **Feedback suppression:** extreme gain on a live mic will howl. Add a
  notch-based feedback eliminator or at least a tighter gate.

---

## 4. Development roadmap

1. **Tune by ear on desktop.** Record a dry vocal and iterate on `dsp/rasp.h`
   with `rasp_wav`. All the sound design happens here.
2. **Bring up on a Daisy Seed.** Drop `daisy/main.cpp` into a
   [DaisyExamples](https://github.com/electro-smith/DaisyExamples) project.
   Pot wiring is in the file header. Measure CPU with `CpuLoadMeter`.
3. **Breadboard the mic preamp.** Use a dynamic mic, check noise and gain staging.
4. **Build the enclosure and PCB.** Use a Terrarium-style board or a custom KiCad board.
5. **Add stage features.** Assist footswitch, presets, MIDI.

## Status / caveats

- The DSP core compiles and is verified on desktop. Processing a synthetic
  voice gives stable, bounded output with the expected harmonic brightening and
  sub-octave content.
- `daisy/main.cpp` is written against the libDaisy API but **has not been
  compiled or run on hardware yet.**
- Parameter changes are applied straight from the control loop. A production
  version should smooth them to avoid zipper noise.
- The sound needs real tuning with real vocals. Treat the defaults as a starting point.
