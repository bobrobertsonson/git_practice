# AxeElevator — a pre-amp DI enhancer for guitar and bass

*(Working title. Company: Snotch / Studio Notsch.)*

AxeElevator goes on a **clean DI, before an amp sim**. It's inspired by the
idea behind Dark Cabin Studios' DI Enhancer, but it's our own design: no
code, names or graphics of theirs, and any comparison with their plugin is
done black-box only (render a DI through it, measure with `tools/drywet.py`).

| Path | What it is |
|---|---|
| `dsp/axe.h` | The DSP engine. Header-only C++17, no heap, no dependencies, no allocation in the audio thread. The same file will run on a Daisy Seed |
| `plugin/` | JUCE plugin: AU / VST3 / Standalone (macOS) and LV2 through Darkglass's `juce-anagram-lv2` wrapper (Linux, Anagram, MOD) |
| `host/axe_wav.cpp` | Process a WAV file from the command line |
| `host/synth_di.cpp` | Synthetic guitar and bass DIs (chords, palm mutes, bends, fresh vs dull strings) for testing |
| `tests/engine_test.cpp` | Engine unit tests (`make check`) |
| `tests/lv2_host.cpp` | Minimal headless LV2 host (lilv) with the features mod-host provides; LV2 checks (`make lv2check`) |
| `tools/render_all.sh` | Renders DIs through every mode and measures them (`renders/report.txt`) |
| `tools/drywet.py` | From RASP: what did a chain do to a DI (EQ, level, dynamics, low end) |
| `BUILD.md` | How to build every target, including the Anagram toolchain |
| `samples/` | Your DIs go here. Git-ignored: audio never goes to GitHub |

```sh
make                 # axe_wav, synth_di, engine_test
make check           # engine unit tests
tools/render_all.sh  # synthetic DIs -> every mode -> renders/report.txt
./axe_wav my_di.wav out.wav mode=buzzsaw inst=guitar gain=0.8
./axe_wav            # lists every parameter
```

## The modes

One mode runs at a time. The **Guitar / Bass** switch re-voices every mode
(crossovers, filter corners, EQ centres). Global controls: **Input**, **Gate**
(a downward expander for DI hiss and hum, keyed on the input), **Mix**, **Output**.

| Floor | Mode | Knobs | What it does |
|---|---|---|---|
| 1 | **Tight** | Tight, Punch, Release, Low, Mid, High | 3-band compressor (Linkwitz-Riley split at 150 Hz / 1.8 kHz, bass 90 / 800 Hz). Each band's threshold rides that band's own average level, so it works the same at any DI level. The low band clamps hardest and fastest: a palm mute's lows bloom 10–40 ms after the pick, and stopping that bloom is what "tight" means. Punch lengthens the attack so the pick click still gets through |
| 2 | **Sub** | Poly/Mono, Sub, Dry, Sub Tone, Range, Grit | Octave down blended with the DI. **Poly** holds up on chords (below). **Mono** is RASP's WSOLA shifter: the cleanest on single notes, but it warbles on chords. Range sets the highest note that gets a sub; Grit saturates the sub so it stays audible on small speakers |
| 3 | **New Strings** | Bright, Clarity, Air, Match, Learn Fresh, Learn Now | Gives dull strings back what fresh ones have: upper harmonics (Bright: a level-independent exciter whose new harmonics land at 3–8 kHz, bass 1.8–5 kHz), pick attack (Clarity: a presence bell that opens on transients) and top (Air). **Learn**: play ~12 s on fresh strings (*Learn Fresh*), and later ~12 s on your dull strings (*Learn Now*). It computes the EQ that pulls them toward the fresh sound, in 8 bands from 250 Hz to 10 kHz |
| 4 | **Buzzsaw** | Gain, Lows, Highs, Scoop, Level, Blend | HM-2-style heavy drive, our own circuit: a lift into two 4× oversampled clipping stages (antiderivative anti-aliasing), then big boosts near 100 Hz and 1 kHz (bass ~70 Hz / 800 Hz) with the middle scooped between them. Auto-levelled to the DI's loudness. Blend adds the clean DI underneath (bass: try 30–40%) |

Every continuous control is smoothed (filter coefficients glide per sample).
Mode and instrument changes crossfade over 40 ms between two engines, and
bypass crossfades over 20 ms. The plugin reports **zero latency**: there is
no lookahead and no block processing anywhere.

### Sub on chords — and why not a phase vocoder

A single-voice shifter (like RASP's WSOLA) finds *one* period in the signal
and repeats or skips whole periods. A chord has no single period, so it
lurches between notes: that's the warble.

A **phase vocoder** works in the frequency domain instead. It cuts the audio
into overlapping windows of ~40–90 ms, takes an FFT of each, and treats every
frequency bin as a little oscillator with a magnitude and a phase. To shift
down an octave it halves each bin's frequency (moving its energy to half the
bin index and rotating its phase half as fast), then resynthesises with an
inverse FFT and overlap-add. Because every partial of every note is handled
separately, chords shift cleanly. The catch is the window: a bin can only be
read once the whole window has arrived, so the shifted signal comes out one
window late, plus processing. Good chord separation on low guitar notes needs
windows of 2048–4096 samples, which is **40–90 ms of latency**. That's far
over your 10 ms limit, and it's why studio pitch shifters can afford it and a
pre-amp pedal can't.

**What Poly does instead: a filter-bank divider.** It uses the same idea of
handling each partial separately, but with filters instead of windows:

- 56 narrow band-pass filters, 1/12 octave apart, cover the range (guitar
  50 Hz–1.3 kHz, so drop tunings down to A; bass 28–720 Hz, so a 5-string's low B).
- In each band, a pair of state-variable filters gives the band as a rotating
  phasor. Taking its square root halves the rotation: the band's content an
  octave down, at the band's own level.
- A band only speaks when it is the strongest of its neighbours, so bands
  sitting *between* two chord notes (which beat) stay quiet. Neighbouring
  bands that carry the same partial line up their polarity, so they can't cancel.
- The bank runs decimated to ~6 kHz, so it costs about 2% of one desktop core.

There's no buffering, the DI itself is untouched, and the plugin reports zero
latency. The trade is **bloom**: a narrow filter takes time to ring up, so the
sub of a low E reaches full level ~45–75 ms after the pick (A3: ~35 ms). It
arrives as a fast swell rather than a delayed copy.

Measured (`engine_test` and a sweep of chord shapes, sub level of each note
re that note's level, 0 dB = equal):

| Chord | Poly | Mono (WSOLA) |
|---|---|---|
| E major low triad E2-G#2-B2 | 0.0 / +0.1 / +0.2 dB, stray low energy −17 dB | one note at −15.8 dB, stray energy only −2.6 dB below the subs |
| E5, A5, open E, open G, drop-C C5, drop-A A5 | every note within −2.8 … +0.8 dB | (warbles) |
| single notes 76–200 Hz | ±0.7 dB across the range | clean |

### Learn (New Strings)

The Learner (from RASP) measures the long-term spectrum of what you play in
8 bands, relative to the 250 Hz–1 kHz level, so it doesn't care how hard you
hit or how hot the DI is. It only counts while you play. The match is the
difference between the fresh and the current profile. It is capped at ±3 dB
below 1.2 kHz (that region depends more on *what* you played than on the
strings) and ±12 dB above, and refined against the real filter cascade. On a
test pair (a fresh riff, then a different riff with ~6 dB less top) it took
the worst band error above 1.6 kHz from 5.6 dB to 0.0 dB.

In Logic the learned bands land on the eight **Match** parameters, so they
save with the session and you can edit them by hand. The fresh-strings
profile is saved too, so *Learn Now* works in a later session. On the
Anagram see the notes below.

## Measured on the synthetic DIs (`tools/render_all.sh`)

Loudness change, and the spread of the 100 ms level while playing (lower =
tighter or denser):

| | Palm-muted chugs | Chords | Bass line |
|---|---|---|---|
| Tight (default) | −0.4 LU, spread 9.1 → 6.4 dB | −3.6 LU, 17.8 → 9.0 dB | +0.6 LU, 12.0 → 8.6 dB |
| Tight (hard) | −0.2 LU, 9.1 → 5.0 dB | −4.2 LU, 17.8 → 4.4 dB | +1.0 LU, 12.0 → 6.1 dB |
| Sub (poly, default) | energy below 70 Hz −21.5 → −9.3 dB | −35 → −13.2 dB | |
| New Strings (default) | dull-strings DI: −3…−11 dB vs fresh above 1.6 kHz → −3.4…−0.5 dB | | |
| Buzzsaw (default) | −0.2 LU (auto level), peak < −10 dBFS | −0.7 LU | +0.1 LU |

These numbers show the processing does what it says, not that it sounds
good: real DIs and your ears decide that. Drop your DIs in `samples/` and run
`tools/render_all.sh samples/`.

## Darkglass Anagram / MOD notes

Built with Darkglass's `juce-anagram-lv2` wrapper, following Plugin-Dev-Setup's
REQUIREMENTS.md, CAVEATS.md and LV2-FEATURES.md:

- **IO**: mono in / mono out. Two mono instances are bit-identical after
  `kx:Reset`, so the Anagram can pair them for stereo (tested).
- **Bypass**: `lv2:enabled` control port (default 1), a 20 ms crossfade to
  the untouched input. The plugin never changes it itself.
- **Reset**: `kx:Reset` clears all state and jumps smoothed values to their
  targets. A 0-sample `run()` is harmless (tested).
- **Smoothing**: every parameter. Mode and instrument changes crossfade, so
  scene changes don't drop out or click.
- **Control ports only**, with real ranges and units (dB, Hz, ms, %) and
  choices as enumerations, because the Anagram can't ask a plugin for value
  text. Parameters come in **pages of 6**: Global, Tight, Sub, New Strings,
  Buzzsaw, then Match (6 + 2).
- `dg:abbreviation "AXE"`, 200×200 block images (`plugin/images`, drawn by
  `tools/make_block_images.py`), category `lv2:DistortionPlugin`.
- The plugin never touches CPU flags (no flush-to-zero in the LV2 build), and
  it doesn't link X11 or any display library.
- **ARMv8.2 64-bit** builds: the Darkglass toolchain workflow
  (`.github/workflows/axe-anagram.yml`), and an `-march=armv8.2-a` CI build.

**Known limits:**

- **Learn on the Anagram**: a plugin there shouldn't move its own control
  ports, and the wrapper has no way to ask the host to. So a learned match is
  applied *inside* the plugin until you touch a Match port, and it isn't saved
  with the preset (it's audible right away). The Match ports can always be set
  by hand. The fix is host-side: the wrapper could use the
  `control-input-port-change-request` feature the Anagram already supports.
  That's in the email to Darkglass.
- **Desktop LV2 hosts**: the wrapper *requires* the `nominalBlockLength`
  option. mod-host, Carla and Ardour provide it. Ubuntu 24.04's `jalv` and
  `lv2apply` don't, and refuse to load the plugin. `tests/lv2_host.cpp`
  provides it.
- **MOD Dwarf**: runs glibc 2.27 and GCC 7.5's runtime (an ARMv8.0
  Cortex-A35). The CI LV2 is built against glibc 2.31 and links libstdc++
  statically, so it needs a glibc ≥ 2.31 system: fine for the Anagram (2.34)
  and current desktops, **not** for the Dwarf. A Dwarf build needs MOD's
  `mod-plugin-builder` toolchain. Note that every JUCE plugin also needs
  freetype and fontconfig on the target. Because the engine is a dependency-free
  header, a small plain-LV2 wrapper (no JUCE) would be the cleanest route for
  the Dwarf. That's a follow-up.

## Daisy Seed (later)

`dsp/axe.h` already keeps the pedal rules: no heap, fixed-size buffers (set
`AXE_SR_SCALE 1` for ≤ 96 kHz), no allocation or system calls in `process()`,
and float math. Per-mode cost on one desktop x86 core at 48 kHz: Tight 1.0%,
Sub poly 1.9%, Sub mono 0.8%, New Strings 0.9%, Buzzsaw 3.5%. That leaves
headroom on the Seed's 480 MHz Cortex-M7, to be measured on a board (see
`rasp/daisy` for the firmware pattern).

## Adding a mode

Each mode is one class in `dsp/axe.h` with `init / configure(instrument) /
setTargets / snap / reset / control / process(x, dryAligned)`. Add it to
`Mode`, the switches in `Voice`, its fields in `Params`, a page of ≤ 6
entries in `plugin/Source/PluginProcessor.cpp`'s parameter table, and a row
in `kModeIds` in the editor. The tests loop over `kNumModes`, so a new mode
gets the null, sample-rate, block-size, reset and click tests automatically
once its parameters are added to the smoothing list.
