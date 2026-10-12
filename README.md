# SubForge

An 808 boom and sub drop instrument for Logic Pro, built so the low end fits the
mix: it doesn't blow out the bottom, and it stays audible on small speakers.

![SubForge control panel](docs/subforge_ui.png)

## Why it doesn't blow out the mix

Most sub drops cause trouble for the same reasons: a pure low sine eats all the
headroom, it gets quieter to the ear as it falls (so you turn it up), it vanishes
on phones, and its peaks stack with the kick. SubForge handles each of these:

| Problem | What SubForge does |
| --- | --- |
| Energy below ~25 Hz uses up headroom nobody hears | **Low Cut**: a steep 24 dB/oct high-pass removes it |
| Sub peaks push the master limiter into distortion | **Drive + Ceiling**: the sub is soft-clipped up to a fixed ceiling, so its peak level is predictable |
| Drop disappears on phones and laptops | **Harmonics**: an audible harmonic layer above the **Crossover** lets your ear "hear" the missing fundamental |
| Drop sounds quieter as it falls, so you push it | **Auto Harmonics**: more harmonics are added the lower the pitch goes, instead of more level |
| Kick and sub peaks stack | **Kick Duck**: route the kick into the side chain and the sub dips out of its way |
| Wide sub collapses on mono systems and clubs | Output is always mono |
| Retriggered 808s click | New notes choke the old one with a 5 ms fade and always start at the same point in the wave |

Because SubForge makes the sound itself, it knows the exact pitch at every moment.
The harmonics come straight from that pitch, so they're clean (no aliasing) and
follow a drop perfectly, with no pitch detection guesswork.

## Two modes

- **Boom**: an 808-style hit. It starts **Punch** semitones above the note and
  settles onto it over **Punch Time**, then rings out over **Decay**. With
  **One Shot** on it rings out fully; turn it off to let note length (plus
  **Release**) gate it. **Glide** slides between overlapping notes.
- **Drop**: a sub sweep. It falls **Drop Amount** semitones from the note you
  play, over **Drop Length** (synced to the song tempo), shaped by **Drop
  Curve** (negative = fast start, positive = slow start then plunge). Use 12, 24
  or 36 semitones to land on the same note an octave or more lower, so it ends in key.

## Building and installing in Logic (Mac)

You need Xcode (from the App Store) and CMake (`brew install cmake`).

```sh
git clone https://github.com/bobrobertsonson/git_practice.git
cd git_practice
git checkout claude/peaceful-goldberg-oil95u
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target SubForge_AU
```

The first configure downloads JUCE, so it takes a minute. The build copies
`SubForge.component` into `~/Library/Audio/Plug-Ins/Components`. Check that macOS
accepts it:

```sh
auval -v aumu Sfg1 Sbfg
```

Then open Logic. If it doesn't show up, use **Logic Pro → Settings → Plug-in
Manager → Reset & Rescan Selection**. Create a **Software Instrument** track and
choose **AU Instruments → SubForge → SubForge**.

## Fitting it into a mix

1. **Play in key.** The pitch comes from the MIDI note. For drops, pick a Drop
   Amount of 12/24/36 so they land on the root.
2. **Set the Ceiling first** (the default is -6 dB). It caps SubForge's peak
   level, so the rest of the mix keeps its headroom. The line on the Output meter
   shows it. Use **Output** to trim afterwards.
3. **Drive** adds weight. Watch the **Sub Clip** meter: a few dB of reduction is
   fine, constant 10+ dB is crunchy.
4. **Route the kick into the side chain.** In the SubForge window header, open
   the **Side Chain** menu and pick your kick track or bus. Set **Duck Threshold**
   so the **Duck** meter moves on every kick, then **Duck Depth** to taste (6–10 dB)
   and **Duck Release** so it recovers before the next note. Only the kick's low
   end triggers it, so hats and claps on the same bus won't. If Logic doesn't offer
   a side chain menu for the instrument, use Logic's Compressor on the SubForge
   track with the kick as its side chain instead.
5. **Make room under drops.** Put a Compressor on your bass track with SubForge
   as its side chain, or simply cut or thin the bass while a drop plays.
6. **Turn on Phone Check** to hear roughly what a phone speaker plays (everything
   below 150 Hz removed). If the drop disappears, raise **Harmonics** or **Auto
   Harmonics**, not the level. **Turn it off before you bounce.**

## Controls

| Section | Control | Range | Default |
| --- | --- | --- | --- |
| Sound | Mode | Boom / Drop | Boom |
| | Tune | ±24 st | 0 |
| | Punch / Punch Time | 0–24 st / 5–150 ms | 7 st / 40 ms |
| | Decay | 50–4000 ms (time to fall 60 dB) | 800 ms |
| | One Shot / Release | on/off / 5–2000 ms | on / 60 ms |
| | Glide | 0–500 ms | 0 |
| | Click | 0–100 % | 20 % |
| | Velocity | 0–100 % sensitivity | 50 % |
| Drop | Drop Amount | 1–48 st | 24 |
| | Drop Length | 1/4 to 16 beats | 4 beats |
| | Drop Curve | -1 to +1 | 0.3 |
| Low End | Low Cut | 15–60 Hz | 25 Hz |
| | Crossover | 60–200 Hz | 100 Hz |
| | Harmonics / Auto Harmonics | 0–100 % | 35 % / 60 % |
| | Drive | 0–18 dB | 3 dB |
| | Ceiling | -18 to 0 dBFS | -6 dBFS |
| Kick Duck + Output | Duck Depth / Threshold / Release | 0–24 dB / -48–0 dB / 20–500 ms | 8 dB / -30 dB / 120 ms |
| | Output | -24 to +6 dB (after the ceiling) | 0 dB |
| | Phone Check | on/off, monitoring only | off |

## Signal flow

```
            ┌─ sub ──────── low cut ─ drive ─ soft clip (ceiling) ─┐
MIDI ─ voice┼─ harmonics ── high-pass at crossover ────────────────┼─ kick duck ─ safety clip (ceiling) ─ output ─ [phone check]
            └─ click ──────────────────────────────────────────────┘
```

## Project layout

- `Source/dsp/`: the sound engine (plain C++, no JUCE): `SubVoice.h` makes
  the boom/drop, `SubEngine.h` handles notes and the low-end chain, `Filters.h`
  has the filters and the soft clipper.
- `Source/plugin/`: the JUCE wrapper: parameters, MIDI, side chain and the control panel.
- `tests/dsp_tests.cpp`: offline tests for ceiling, pitch accuracy, tempo sync,
  harmonics, click-free retriggering, low cut, ducking, gating and glide.

## Developing on any OS

The DSP tests and the VST3/Standalone builds work on macOS, Linux and Windows:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                         # run the DSP tests
./build/subforge_tests /some/folder            # also write demo WAV renders
```

Use `-DSUBFORGE_BUILD_PLUGIN=OFF` to build only the tests (no JUCE download).
