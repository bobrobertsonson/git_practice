# AxeElevator — a pre-amp sound mangler for guitar and bass

*(Working title. Company: Snotch / Studio Notsch.)*

AxeElevator goes **between your guitar (or bass) and an amp sim**. It has 13
"floors": three tone shapers and ten triggerable, tempo-aware noise makers.
Each floor is voiced for guitar and for bass, and the big **TRIGGER** button
(a footswitch on the Anagram) fires the wild stuff on and off.

| Path | What it is |
|---|---|
| `dsp/axe.h` | The engine: signal chain, Voice, Processor. Includes `axe_base.h` (shared DSP parts), `axe_tone.h` (Tight, Sub, Buzzsaw) and `axe_fx.h` (the ten fx floors plus the clock, trigger, pitch tracker and pick detector). Header-only C++17, no heap, no allocation in the audio thread, so the same code can run on a Daisy Seed |
| `plugin/` | JUCE plugin: AU / VST3 / Standalone (macOS) and LV2 through Darkglass's `juce-anagram-lv2` wrapper (Linux, Anagram, MOD) |
| `host/axe_wav.cpp` | Process a WAV file from the command line, with `trigat=` to stomp the Trigger at set times |
| `host/synth_di.cpp` | Synthetic guitar and bass DIs (chords, palm mutes, bends) for testing |
| `tests/engine_test.cpp` | Engine unit tests (`make check`) |
| `tests/lv2_host.cpp` | Minimal headless LV2 host (lilv) with the features mod-host provides; LV2 checks (`make lv2check`) |
| `tools/render_all.sh` | Renders DIs through every floor (fx floors with Trigger held 2–5 s) and measures them |
| `tools/drywet.py` | From RASP: what did a chain do to a DI (EQ, level, dynamics, low end) |
| `BUILD.md` | How to build every target, including the Anagram toolchain |
| `samples/` | Your DIs go here. Git-ignored: audio never goes to GitHub |

```sh
make                 # axe_wav, synth_di, engine_test
make check           # engine unit tests
tools/render_all.sh  # synthetic DIs -> every floor -> renders/
./axe_wav riff.wav out.wav mode=glitch tempo=140 rate=1/16 trigat=2,4   # Trigger held 2-4 s
./axe_wav            # lists every parameter
```

## Trigger, Engage, tempo

These global controls drive every fx floor:

| Control | What it does |
|---|---|
| **Trigger** | The button or footswitch. In the plugin window: hold to fire; shift-click (or right-click) latches |
| **Engage** | *Always*: the floor runs all the time, and Trigger adds that floor's action (below). *While Triggered*: the floor only sounds while Trigger is held |
| **Sync** | *Host Tempo* follows Logic's tempo and bar position when the host has them; *Tempo Knob* always uses the knob |
| **Tempo** | 40–300 BPM. The Anagram passes plugins no tempo yet, so there you set it here (asked of Darkglass) |
| **Rate** | The grid every fx floor uses: 1/1 … 1/32, triplets, dotted. With no host transport running, pressing Trigger restarts the grid, so the first step lands on your stomp |
| **Swing** | Delays every second step, up to 75% (hard shuffle) |

Plus **Input**, **Gate** (a noise gate keyed on your input), **Mix** and **Output**.

## The floors

| | Floor | Knobs | What it does | Trigger (Engage = Always) |
|---|---|---|---|---|
| 1 | **Tight** | Tight, Punch, Release, Low, Mid, High | 3-band compressor: the lows clamp fast, so palm mutes don't flub | — (While Triggered switches it in) |
| 2 | **Sub** | Poly/Mono, Sub, Dry, Sub Tone, Range, Grit | Octave down. Poly holds chords (below); Mono is RASP's WSOLA shifter, cleanest on single notes | — |
| 3 | **Buzzsaw** | Gain, Lows, Highs, Scoop, Level, Blend | HM-2-style chainsaw drive: big lows and 1 kHz, scooped middle, 4× oversampled | — |
| 4 | **Glitch** | Chaos, Crush, Pitch Jumps, Tape Stops, Reverse, Smooth | Beat repeat: grabs the last step and replays it. Each step Chaos decides between repeat, roll (½ or ¼ repeats), pitch jump, tape stop, reverse and fresh audio. Chaos 0 = a clean stutter | Holds a glitch. In Always it also glitches by itself now and then |
| 5 | **Laser** | Pitch, Track Note, Drop, Sweep, Tone, Level | Every pick fires a falling "pew" (sine to square), as loud as you picked; Track starts it two octaves above your note | Machine gun: one shot per Rate step, even with no input |
| 6 | **Dive Bomb** | Depth, Dive Time, Return, Curve, Wobble, Dry | Pitch drops 1–36 semitones over a time in beats, wobbles at the bottom, climbs back | Holds a dive. In Always every pick also dives |
| 7 | **Chiptune** | Pulse Width, Crush, Arpeggio, Octave, Glide, Dry | Follows your note with a pulse-wave game voice (mono: the strongest note wins) | Runs the arpeggiator (octave, power, major, minor, random) on the Rate |
| 8 | **Stutter Gate** | Pattern, Duty, Depth, Shape, Random, Muffle | Rhythmic chopping on the Rate: all, offbeat, trance, gallop, chug, random | Double-time roll |
| 9 | **Ring Shredder** | Type, Freq, Track Note, Jumps, Glide, Drive | Ring modulator or frequency shifter (every partial moved by the same Hz: metallic). The carrier can follow your note | The carrier jumps (octaves, fifths, fourths) on the Rate, with glide |
| 10 | **Freeze** | Size, Attack, Release, Drift, Shimmer, Level | Holds what you just played as an endless pad (four detuned grains, Shimmer adds octave-up grains). You play on top | Freezes now. In Always every pick re-freezes |
| 11 | **Feedback** | Harmonic, Swell, Release, Level, Bloom, Tone | Amp feedback without the volume: a resonator on a harmonic of your note that starts singing on its own and outlives the string | Swells now. In Always it swells on notes you let ring |
| 12 | **Swarm** | Density, Grain Size, Spray, Pitch Spread, Reverse, Level | Granular cloud: grains from up to 1.5 s back, some shifted or reversed | 4× burst |
| 13 | **Radio** | Station, Width, Static, Warble, Spin, Fading | Your guitar as a shortwave broadcast: band-pass station, fading, wow and flutter, static | Spins the dial to a new station, re-tuning on the Rate while held |

Every continuous control is smoothed. Floor and instrument changes crossfade
over 40 ms between two engines, and bypass crossfades over 20 ms. The plugin
reports **zero latency**: no lookahead, no block processing.

### Measured (`engine_test`)

| Floor | Check | Result |
|---|---|---|
| Glitch | Chaos 0: correlation of the output with itself one step later, while held | 0.999 (a clean repeat) |
| Laser | A pick fires a falling tone | 1.5 kHz → 940 Hz over 400 ms |
| Dive Bomb | Octave down after the dive time, back after the return | 220.0 Hz from 440, then 440.0 Hz |
| Chiptune | 220 Hz in, octave +1 | 440.0 Hz square wave |
| Stutter Gate | Offbeat pattern, full depth | Closed steps 81 dB down |
| Ring Shredder | Frequency shift of 1 kHz by +100 Hz | 1100 Hz; image 49 dB down, carrier 108 dB down |
| Freeze | One second after the string stopped | −4.7 dB re the note; gone after release |
| Feedback | Octave sustain two seconds into a decaying note | 40 dB above the string's own octave |
| Swarm | Grain level | −9 dB re the input |
| Radio | Band-limiting white noise | Station 23 dB above two octaves off |
| Sub | Chords (E5, A5, open E, open G, drop-C, drop-A, low E-G#-B) | Every note's sub within ~2 dB |
| Pitch tracker | Guitar 55 Hz–1 kHz, bass 31–220 Hz | Within 1% |
| All floors | Bypass, reset sync, block size, 44.1–192 kHz, click checks | 1176 checks pass |

Per-floor cost on one desktop x86 core at 48 kHz: 0.3–2.5% (Buzzsaw 4.5%).

### Sub on chords — and why not a phase vocoder

A single-voice shifter (like RASP's WSOLA) finds *one* period in the signal
and repeats or skips whole periods. A chord has no single period, so it
lurches between notes: that's the warble.

A **phase vocoder** works in the frequency domain. It cuts the audio into
overlapping 40–90 ms windows, FFTs each one, halves every frequency bin's
frequency, and resynthesises. Chords shift cleanly, but the shifted signal
comes out a whole window late: **40–90 ms of latency**, over the 10 ms
budget.

**Poly does it with filters instead of windows.** 56 narrow bands, 1/12
octave apart (guitar 50 Hz–1.3 kHz, bass 28–720 Hz), each split into a
rotating phasor whose rotation is halved, so each band's content drops an
octave. A band only speaks when it's the strongest of its neighbours, so
bands sitting between two chord notes stay quiet. The bank runs decimated to
~6 kHz. There's no buffering and the DI is untouched. The trade is
**bloom**: the sub of a low E reaches full level ~45–75 ms after the pick
(A3: ~35 ms).

## Darkglass Anagram / MOD notes

Built with Darkglass's `juce-anagram-lv2` wrapper, following Plugin-Dev-Setup's
REQUIREMENTS.md, CAVEATS.md, LV2-FEATURES.md and TIME.md:

- **IO**: mono in / mono out. Two instances are bit-identical after
  `kx:Reset`, including the "random" fx (seeded generators), so the Anagram
  can pair them for stereo.
- **Bypass**: `lv2:enabled` control port (default 1), a 20 ms crossfade to
  the untouched input.
- **Reset**: `kx:Reset` clears state; a 0-sample `run()` is harmless.
- **Control ports only**, with real ranges and units, and choices as
  enumerations. 108 parameters in **18 pages of exactly 6**: Global, Trigger,
  one page per floor, then Learn, Tone Match and Tone Match 2.
- **Learn on the Anagram**: Learn Playing, Learn Target and Match Now are
  toggle ports; switching one on starts it. The wrapper can't move its own
  ports, so learned values apply inside the plugin and the knobs keep showing
  the old ones until you turn one (turning a knob hands control back to it).
- **Trigger** is a toggle port: bind it to a footswitch. Whether a footswitch
  can drive it momentarily is a question for Darkglass.
- **Tempo**: TIME.md says the Anagram only exposes BPM, through a
  `time:beatsPerMinute` port designation. The JUCE wrapper doesn't implement
  that, so for now the Tempo knob is the clock there. Asked of Darkglass.
- `dg:abbreviation "AXE"`, 200×200 block images, no CPU-flag changes, no
  X11 or display library, ARMv8.2 builds.

Other limits:

- **Desktop LV2 hosts**: the wrapper *requires* the `nominalBlockLength`
  option. mod-host, Carla and Ardour provide it; Ubuntu's `jalv` and
  `lv2apply` don't. `tests/lv2_host.cpp` provides it.
- **MOD Dwarf**: glibc 2.27 and an ARMv8.0 CPU. The CI LV2 needs glibc ≥ 2.31,
  so a Dwarf build needs MOD's `mod-plugin-builder`. Because the engine has
  no dependencies, a small plain-LV2 wrapper without JUCE is the cleanest
  route there (follow-up).

## Learn

Two learn functions, on the **SETUP** page (the Learn and Tone Match pages on
the Anagram). Both only listen: they never record or store audio.

**Learn My Playing** (press, then play normally for 10 s with a few pauses).
It measures your rig and sets five controls, which you can also set by hand:

| Control | Learned from | Used by |
|---|---|---|
| Pick Sense | Half the typical jump in level when you pick | The pick detector: Laser, Dive Bomb, Freeze and Feedback in Always |
| Pick Hold | Half your fastest picking interval | Stops one pick firing twice |
| Noise Floor | The quietest 5% of the pauses | Auto Gate: the gate opens 8 dB above it (+ Gate) |
| Low Note / High Note | The notes you played, ±2 semitones (0 Hz = the instrument's full range) | The note tracker: Laser, Chiptune, Ring, Feedback stop jumping to octaves you never play |

**Tone Match** (an EQ before every floor, 8 bands, 250 Hz–10 kHz, ±12 dB):

1. **Learn Target**: play a reference for 12 s (a DI or guitar whose tone
   you like). Its tone is stored with the session.
2. **Match Now**: play your own guitar for 12 s. The Match bands move your
   tone towards the target; **Match** sets how far (0–100%).

Only playing counts towards the 12 s: pauses don't.

| Check (`engine_test`, `plugin_test`, `lv2_host`) | Result |
|---|---|
| Tone Match, worst band error above 1.6 kHz | 10.7 dB → 1.3 dB |
| Learn My Playing on a synthetic DI (notes 82–220 Hz, a pick every 250 ms, pauses, hiss at −66 dBFS) | Floor −68 dBFS, pick hold 125 ms, range 73–247 Hz |
| Auto Gate | Hiss in the pauses 21 dB down, notes unchanged |
| Tone Match through LV2 ports only | Dull input's high-to-low ratio ×2.35 |

## Adding a floor

Each floor is one class with `init / configure(instrument) / setTargets /
snap / reset / control / process(x, dryAligned)`, reading the shared
`FxContext` for the clock, Trigger and input analysis. Add it to `Mode` in
`axe_base.h`, to the `Voice` in `axe.h`, its fields to `Params`, and a page of
exactly 6 entries to `plugin/Source/PluginProcessor.cpp`. The editor builds
its page from that table. The tests loop over `kNumModes`, so a new floor
gets the bypass, null, silence, sample-rate, block-size, reset and
click checks automatically.
