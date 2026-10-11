# RASP — a hardware extreme-vocal processor

A floor box that turns a clean (or half-committed) vocal into fry screams,
gutturals and high squeals in real time, with doubling, delay and reverb
built in. It's in the same spirit as software "screamer" vocal plugins,
built for a stage pedalboard.

| Path | What it is |
|---|---|
| `dsp/rasp.h` | The whole DSP engine. Header-only C++17, no heap, no dependencies |
| `host/rasp_wav.cpp` | Desktop tool: process a WAV file so you can tweak by ear |
| `daisy/main.cpp` | Firmware for an Electrosmith Daisy Seed (12 pots, 2 toggles, 3 footswitches) |
| `playground/index.html` | Playable browser preview, a JS port of `dsp/rasp.h` |
| `Makefile` | Builds the desktop tool |

```sh
make
./rasp_wav dry_vocal.wav screamed.wav type=1 drive=0.8 fry=0.7 spread=0.6 tailblend=0.3
./rasp_wav                     # lists every parameter name
```

**Hear it:** open `playground/index.html` in Chrome on your own computer.
Load a dry vocal or press *Live mic* while wearing headphones. The live mic
only works when the file is opened locally. The hosted preview can't
access a microphone, so load a recording there instead.

---

## 1. Signal chain

The panel follows the three-section layout of screamer-style plugins:

```
mic ─► CONTROL ────────────────► THICKEN ──────────────► FLAVOUR ─────────────► L/R out
       lo cut / hi cut           spread  (2 detuned       tail + blend   (reverb)
       drive + type switch               doubles)         repeats + blend (ping-pong delay,
       mix (dry/wet)             diffuse (smear)                          slap / 1/8 / 1/4 + tap)
       chop (gate) + release     darken, width            tilt  (±6 dB around 900 Hz)
       gain                      solo, gain               duck  (verb/delay dip while you sing)
```

**Drive is the scream engine.** It models the throat in four parts:

| What the voice does | What you hear | DSP |
|---|---|---|
| False folds vibrate at half the pitch | Octave-down "false chord" weight | Flip-flop sub-octave generator |
| Chaotic fold vibration | Rattle, gravel, growl | Jittered amplitude modulation (20–90 Hz) |
| Constricted vocal tract | Guttural "oo" to shrieking "ee" | Two resonant formant filters + chest bump |
| Overdriven airflow | Grit and tearing | Asymmetric clip → sine wavefolder, 2× oversampled |
| Turbulent air | Breath and hiss | Band-passed noise that follows your level |

The drive follows your input level: the harder you push, the more it breaks
up. A glue compressor holds the output level steady. The **Type** switch
(Fry / Growl / Squeal) moves the formants and the rattle character.

## 2. Latency: the target is near zero

- **The DSP adds no buffering of its own.** Every stage is causal with no
  lookahead. An impulse comes out on the same sample it goes in; this was
  checked with `rasp_wav`. The oversampler uses IIR filters, so it adds no
  block delay.
- **On a Daisy Seed with 4-sample blocks**, buffering adds about 0.17 ms at 48 kHz.
  The rest is the codec's ADC/DAC converter delay. My expectation is a
  total round trip of **about 1 ms**, but measure it: send a click through
  and record input and output on an interface. For scale, 1 ms is the delay
  you hear standing about 34 cm from a speaker, so you won't hear it as
  delay. Above roughly 5–7 ms, singers start to feel it.
- **Analog dry path:** the dry voice never needs to pass through the
  converters. In "assist" use, a hardware design can mix the dry signal in
  analog after the DAC.
- **True bypass:** a relay that connects input straight to output when bypassed. It also
  keeps the vocal path alive if the box loses power. Strongly recommended
  for a vocal box.
- **Headphone/IEM out on the box:** this lets you hear the effect through
  in-ears with the box's 1 ms latency. Going through a digital desk's
  monitor mix can add another 1–3 ms.
- **The browser playground has 10–40 ms** of latency (it shows the measured figure).
  Use it to audition the sound, not to judge how it feels to sing through.

## 3. Feedback: yes, it's a real risk live

Feedback happens when the mic picks up the speakers and the loop gain goes
above 1. A screamer makes that more likely for two reasons:

1. **The drive adds up to ~30 dB of gain,** and the compressor lifts quiet
   sounds. Room noise and wedge spill get loud too. In testing, a −60 dBFS
   noise floor came out at **−34 dB** between phrases with the gate off.
2. **Saturation fills in the spectrum,** so more frequencies are close to
   howling at once.

What RASP does about it:

| Defense | Where |
|---|---|
| **Chop:** a gate that listens to the clean signal before the drive, with hysteresis, 40 ms hold and adjustable release. Between phrases, output goes to silence (that same −34 dB dropped to digital silence). The gate LED lights only while the gate is open | `Chop` in `dsp/rasp.h`, LED on D15 |
| **Lo cut / hi cut:** cuts low-end rumble and fizzy highs, the regions where wedges howl most | CONTROL section |
| **Duck:** reverb and delay dip while you sing, so the tails don't add gain under the voice | FLAVOUR section |
| **Output safety clipper** | End of chain |

What you should do on stage:
- Use **in-ear monitors instead of wedges**. This is the biggest single improvement.
- Use a **dynamic cardioid or supercardioid mic** (SM58, Beta 58A, e835). Keep the
  wedge in the mic's null: behind a cardioid, at 45° for a supercardioid.
- **Cup the mic only if that's your sound.** Covering the grille removes the
  pickup pattern and makes feedback much more likely.
- **Set chop at soundcheck:** stand at your spot with the band playing,
  not singing, and turn chop up until the gate LED goes off.

Planned for later: an automatic howl detector. It would watch for a narrow
peak that keeps growing, then place a notch filter on it. Most commercial
feedback suppressors work this way.

## 4. What could run it

| Platform | Why choose it | Drawbacks | Approx. cost |
|---|---|---|---|
| **Electrosmith Daisy Seed** ⭐ | 480 MHz Cortex-M7, 64 MB SDRAM for delay lines, audio codec on board, 12 ADC inputs. Supports blocks of a few samples | Needs your own preamp, PCB and enclosure | ~$30 for the module |
| **Daisy + PedalPCB Terrarium/Hothouse** | Ready-made pedal PCB, the fastest way to a real box | Guitar-level input, so you still need a mic preamp; fewer knobs | ~$60–150 |
| **Teensy 4.1 + Audio Shield** | 600 MHz M7, large community | Its default 128-sample blocks give ~3 ms of latency; can be reduced | ~$45 |
| **ESP32-S3 + I2S codec** | Cheap, Wi-Fi/Bluetooth for a phone editor app | Weaker FPU, so the full chain is tight | ~$15 |
| **Bela / Raspberry Pi + audio HAT** | Enough power for neural "scream transfer" models | More latency than bare metal, unless you use Bela | ~$80–200 |
| **ADI SHARC / SigmaDSP** | What commercial vocal boxes use | Proprietary tools | $$ |

On desktop the full chain runs at about 86× real time. I estimate that's
roughly **15–25% CPU on a Daisy Seed** but haven't measured it on the board yet.
That leaves room for 4× oversampling or a howl detector. The delay lines
take about 1.6 MB, so the firmware places the processor in SDRAM.

### Hardware beyond the Daisy

- **Mic preamp:** XLR in with 40–60 dB of gain (THAT1512 or INA217). Add +48 V
  phantom power through a small 9 V → 48 V boost if you want condenser mics.
- **Outputs:** XLR L/R out at mic level, ¼" line outs and a **headphone out** for IEMs.
- **True-bypass relay**, a **ground-lift** switch and 9 V center-negative power.
- **Panel:** 12 pots, 2 three-way toggles, 3 footswitches (bypass, flavour, tap),
  LEDs and an encoder + OLED screen for the menu parameters. The pin map is in
  `daisy/main.cpp`. A larger case such as a 1590DD fits it all.
- **Rough parts cost:** about $130 (Daisy $30, preamp + phantom $25, jacks $20,
  relay $5, enclosure $20, pots/switches/OLED $25, PCB $10).

## 5. Roadmap

1. **Tune the sound by ear** with real dry vocals, using the playground or `rasp_wav`.
2. **Bring up a Daisy Seed** on a breadboard. Measure CPU (`CpuLoadMeter`) and round-trip latency.
3. **Breadboard the preamp** and check noise and gain staging with an SM58.
4. **Design a KiCad PCB and build the enclosure.**
5. **Add stage features:** howl detector, presets in QSPI flash, MIDI program
   change, and a momentary "assist" footswitch mode.

## Status

- The C++ engine builds and has been checked on desktop. Output is stable and
  bounded at 48 and 96 kHz, an impulse passes with zero samples of delay, and
  the gate silences the space between phrases.
- The playground's JS engine matches the C++ engine to within −46 dB
  (float vs. double rounding).
- `daisy/main.cpp` targets the libDaisy API but **has not been compiled or run
  on hardware yet**.
- The defaults are a starting point. They still need tuning with real
  vocals.
