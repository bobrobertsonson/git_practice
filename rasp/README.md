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
| `daisy/main.cpp` | Floor-pedal firmware for an Electrosmith Daisy Seed: 16 knobs, 4 faders, 3 trims, 4 footswitches, presets in flash, USB MIDI. Builds against libDaisy v9.0.0 |
| `daisy/pod/main.cpp` | Bench firmware for a Daisy Pod: the same DSP on a $70–90 board, for hearing it and measuring latency/CPU first |
| `BUILD.md` | **What to buy and how to build it:** Stage 1 bench test, Stage 2 full pedal parts list |
| `dsp/presets.h` | Factory presets, shared with the playground (`make check` verifies they match) |
| `host/live_test.cpp` | Tests for bypass, mono and feedback (closed-loop room simulation) |
| `playground/index.html` | Playable browser preview, a JS port of `dsp/rasp.h` that matches it to −76 dB. Load a recording, then scrub it on the waveform or loop one phrase |
| `tools/probe.py` | Measurement kit for matching RASP to a reference plugin (section 6) |
| `tools/takes.py` | Renders real takes through every preset, measures them, and compares them with finished reference vocals. See `samples/README.md` |
| `samples/` | Drop zone for raw takes (`raw/`) and finished references (`ref/`) |
| `tools/mixprint.py` | Learns a producer's vocal treatment (EQ, level, pan, rough space) from raw vocal stems plus the final mix |

```sh
make
./rasp_wav my_scream.wav out.wav drive=0.45 chop=0.65 spread=0.75 tailblend=0.45 glue=0.65 out=3
./rasp_wav                      # lists every parameter
```

---

## 1. The chain

```
                ┌──────────────────── dry (control out) ────────────────────┐
mic ─► CONTROL ─► (+ OCTAVE) ─┼─► THICKEN  doubles + stereo combs   (parallel) ─┼─► BUS ─► L/R out
                              └─► FLAVOUR  plate + delay, ducked     (parallel) ─┘
```

| Stage | Controls | What it does, and why it's there |
|---|---|---|
| **Control** | lo cut, hi cut | 24 dB/oct high- and low-pass filters. Removes rumble and handling noise, and fizz above the voice |
| | drive, mix | Tube-Screamer-style overdrive, with a 24 dB/oct high-pass after it at the lo cut frequency: distorting a rough voice creates low rumble that would otherwise add ~12 dB below 120 Hz. Gain is applied only above ~720 Hz, through a soft diode-pair clip added onto the clean signal, with a cap rolloff as drive rises. This gives grit in the mids without fizz. 4× oversampled with anti-aliasing on the clipper (aliasing −72 dB at full drive). Level-matched, so drive and mix change texture, not volume. The clean signal takes the drive's own filter path, so blending wet and dry can't cancel (see *Hollow* below) |
| | tone switch | Sets the drive's tone filter **and** a mix-style voicing EQ: chest-weight shelf, a cut in the boxy 420–500 Hz region, a 2.6–3.5 kHz presence lift, and an air shelf. Dark suits gutturals; bright suits highs and squeals |
| | *de-esser* (trim) | A dynamic bell at 7 kHz, up to 8 dB. Drive plus presence plus air would otherwise make every "s" and "t" spit |
| | chop, release | Four stages on one knob, the way produced screams are usually compressed: a **leveler** that rides the voice toward −18 dBFS (up to ±15 dB, only while you're singing), a FET-style peak compressor (fast, catches spikes), an opto-style compressor (two-stage release, evens out phrases), and a parallel "crush" blended underneath for density. Because of the leveler, it compresses the same whatever your input gain. The LED lights above 3 dB of gain reduction |
| | *air* (menu) | Extra high shelf at 7 kHz, 0–12 dB. +4 in Huge, +7 in Air |
| | body | Broad low-mid boost at 220 Hz, 0–9 dB: chest and weight. +5 in Huge, fitted to fix "still thin" (see below) |
| | *bleed reducer* (trim) | Downward expander (1:4 below threshold, up to −40 dB), keyed on the clean mic signal. Compression lifts stage bleed; this turns it back down between phrases. Its own LED shows when it's working. See section 3 |
| **Harmony** | 4 pairs × (interval, level, width); randomize pitch / time; low cut, high cut | Four stereo pairs of voices made from the compressed voice, laid out like Neural DSP's Mantra: each pair at any interval from −12 to +12 semitones (0 = a double), with a level and a width (both voices centred → hard left/right). Every voice drifts on its own (randomize pitch up to ±20 cents, time up to 20 ms), so a pair reads as two more singers. Shifted voices use grain-aligned shifting (clean on every interval tested, stray tones ≥26 dB down); unison voices are the double-track voices. The pairs have their own stereo bus with a low and high cut, and also feed the reverb and delay. Defaults: pair 1 an octave down, pair 2 an octave up, pair 3 unison, pair 4 a fifth up, all at level 0 |
| **Thicken** | spread | **Synthetic double tracks**: a pair, plus a second pair as spread goes past 0.35 (the "quad-tracked" metal vocal). Each voice's timing wanders 12–36 ms, its pitch drifts up to ±20 cents and its level ±1.5 dB, and each has its own slight tone. That's what makes a real double read as a second performance. Also quieter stereo comb taps (3–26 ms) |
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

### What changed after a Logic listening test ("hollow, needs compression, thickener doesn't do much")

- **Hollow.** The drive's parallel blend mixed the raw voice with the drive
  path, which had extra filters and oversampling delay. At the default blend a
  real lead lost 10 dB at 100 Hz and had a 7.7 dB notch at 4.5 kHz. The clean
  signal now takes the same path. The notch is gone (+2.4 dB) and the low loss
  is halved (−5 dB, the drive's own mid focus).
- **Compression.** The leveler (above) makes Chop independent of input gain:
  the old version lost about 1.5 dB of density with a 12 dB quieter input.
  Huge's Chop also went from 0.65 to 0.75.
- **Thickener.** The doubles now wander like real double tracks, with a
  second pair in Huge, and Huge's thicken level went from −2 to 0 dB.
- **Still thin** (second listen). The mix fits can't see below 400 Hz on most
  songs (the band covers it), but KnockedLoose's can: its producer had 4.1 dB
  more 250 Hz on the lead than Huge. A **Body** control (+5 in Huge) brings it
  within 1 dB. Your blind picks agreed: louder doubles were "fuller" and the
  78 Hz lo cut "bigger" than 130 Hz, so Huge's doubles went up to +3 dB.
- **Harmony** (four stereo pairs, Mantra-style) replaced the two mono pitch
  layers, after the user showed their Mantra setups (an octave-down pair, unison
  doubles, a fifth). The feedback guard knows about octave pairs; no false notches
  on three leads with all four pairs on.
- **Pitch layers** in semitones (−12…+12) replaced the fixed octaves. The
  feedback guard catches howls at every interval tested (−12, −7, −5, −2, −1,
  0, +7, +12), with no false notches on three real leads.
- **Octave layers** were added, and the feedback guard knows about them. A
  howl's octave copy otherwise looked like a voice's harmonic, and the guard
  missed a howl carrying 42% of the output. Now it's caught at every octave
  setting, with no false notches on six real takes.

## Tuned on real stems

The first real session (six raw death-metal vocal stems plus the finished mix
of the same song) changed the defaults:

- **Gain staging:** the raw takes peaked near −7 dBFS, but their loud sections sat at only
  −26 dBFS RMS (crest factor ~22 dB). An **input gain** stage now
  brings loud sections to −18 dBFS RMS, the level the compressors are tuned
  for. `takes.py` and the playground set it automatically; on hardware it's
  the preamp gain.
- **Tone:** `mixprint.py` measured the producer's EQ on these vocals: a cut at
  400–630 Hz, a lift at 2.5–4 kHz, and the top rolled off. With the drive bypassed,
  RASP's voicing already matched that within ~2 dB from 160 Hz to 6 kHz.
  The Tube-Screamer drive was the outlier. It thinned 160–400 Hz by 6–10 dB
  and dulled 2.5 kHz by up to 11 dB. Its corner now drops to 200/350/600 Hz
  across the tone positions and its tone filter rises. The natural air shelf
  is now +1 dB, the hi cut defaults to 12.6 kHz, and the lo cut to 78 Hz.
- **Space:** the producer's reverb measured ~0.9 s, and the default plate now measures
  about the same.
- **Style detection:** autocorrelation mistook the first vowel resonance
  (~500 Hz) for pitch and called growls "shouts". `takes.py classify` now
  uses cepstral peak prominence. It labels five stems as low growls and one as a
  brighter mid scream, which matches their spectrograms (not yet confirmed by ear).
- **Speed:** silences between phrases made reverb and filter tails decay into
  denormal floats, and real takes rendered at only 2× real time. Flushing
  denormals (desktop and firmware) restored 37× real time with identical output.

### Second session: deep slam gutturals (two raw stems, no mix)

- **A different voice:** brightness of ~600 Hz against ~1 kHz in the first session, and
  about half the energy below 300 Hz. There's also a strong sweeping resonance at
  1.2–1.8 kHz. It was recorded quiet (−32 to −35 LUFS), so it needs ~10 dB of input gain.
- **Presets:** "dark" kept this voice muddy (presence −15.9 dB share), while "natural" and "huge"
  gave it bite (−12 to −13 dB). `classify` now sends very bass-heavy voices
  (more than ~16% of energy below 300 Hz) to "natural" instead of "dark".
- **Feedback guard:** zero false notches across both full songs on three presets,
  despite that strong resonance.
- **Playground bug found:** the browser engine could receive audio before its
  first settings arrived, throw, and silently stop. In a headless test this hit
  7 of 14 page loads. It now outputs silence until the settings arrive (0 of 14).

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

### Live features (all in `dsp/rasp.h`, tested with `live_test`)

| Feature | What it does | Tested |
|---|---|---|
| **Level-matched bypass** | The processed voice measured ~18 dB louder than the dry mic, so a plain bypass makes you vanish when you talk between songs. Bypass now plays the dry voice at the effect's loudness (K-weighted, learned while you sing, ~10 s memory), with a 25 ms crossfade. The relay only takes over if power fails | On the real DI, effect segments measured −10.5/−9.7/−10.2 LUFS and bypassed segments −9.6/−11.2/−10.5. Unmatched bypass would drop 6–8 dB. No clicks at the switch points |
| **Mono mode** | For one-channel PAs. Drops the stereo comb taps (they become a fixed comb filter when summed) and keeps the detuned doubles | Comb ripple summed to mono: 6.9 dB → 3.6 dB peak-to-peak (plain noise: 3.0) |
| **Feedback guard (HowlGuard)** | Watches the output spectrum every 21 ms. It looks for a peak that is narrow, without harmonics (a voice always has them), and steady for 250 ms, then drops a narrow −12 dB notch on it (up to 6, deepening if a frequency returns). Notches stay until cleared | In a simulated mic + resonant room + speaker loop that howled at 1296 Hz (up to 96% pure tone), it notched 1296 and 2956 Hz within 0.8–0.9 s and held the tone under 1%. **Zero notches** on all six full-length real vocal stems and on held clean notes with or without vibrato. A pure whistle does trigger it, as it would any feedback suppressor |
| **Presets** | 6 factory presets plus 2 spare slots, saved in the Daisy's flash. Recall by footswitch (long press) or USB MIDI Program Change. Knobs use "pickup", so they don't jump a recalled preset | `make check`: C++ and playground defaults and presets match |

```sh
make && make check
./live_test bypass take.wav out.wav in=10        # toggles bypass every 6 s
./live_test feedback take.wav out.wav 0.07       # closed loop; add howl=0 to hear it howl
./live_test falsepos take.wav in=8               # how many notches a take triggers
```

## 4. Hardware

**See [BUILD.md](BUILD.md) for exactly what to buy.** In brief:
**Stage 1** is a Daisy Pod (about $70–90, no soldering) to hear it and measure it.
**Stage 2** is the full floor pedal (about $280–380 in parts).

- **Platform:** Electrosmith Daisy Seed (STM32H750, 480 MHz Cortex-M7 with a
  double-precision FPU, 64 MB SDRAM). Both firmwares build against libDaisy
  v9.0.0. The pedal binary is 153 KB, more than the 128 KB of internal flash,
  so it uses Electrosmith's bootloader (stored in QSPI, runs from SRAM: 30% of
  it used). The delay lines take about 2.5 MB of SDRAM.
- **CPU:** 19× real time on desktop (14× with both octave layers on). **Not measured on the Daisy yet**; Stage 1 does that. The octave search is spread over time (at most 288 multiply-adds in any one sample) so it never spikes inside the Daisy's 4-sample audio block.
  The heaviest stage is the 4× oversampled drive; it can fall back to 2× if needed.
- **Precision:** smoothers and slow oscillator phases are double precision.
  As 32-bit floats they drifted measurably.
- **Power:** 12 V DC, centre-negative. ±12 V for the analog side comes from an isolated DC-DC.
  No phantom power in v1, so use a dynamic mic.

## 3a. Learn: adapt to any voice and mic

Press **Learn** and sing for about 15 seconds (gaps are skipped). RASP
measures your long-term tone in ten bands, compares it with the median of seven
singers' raw takes that the presets were fitted on, and sets a **Match** EQ at
the input (up to ±9 dB per band) so every preset starts from the input it was
tuned for. **Match** (0–100 %) scales it. The result is kept across presets:

- **Plugin:** saved in the session.
- **Pedal:** saved in flash.

How to trigger it:

| Where | How |
|---|---|
| Plugin | LEARN button |
| Pedal | Hold BYPASS 2 s; the effect LED blinks while it listens |
| Pod | Hold both buttons 2 s |
| MIDI | CC 86 |
| Playground | "Learn this voice" in the menu |

`learn_tool take.wav` shows what it hears and sets. `learn_tool --target`
rebuilds the reference from reference takes; `make check` keeps the C++ and JS
tables equal.

The first user's own DI was 7–9 dB darker at 1.6–2.5 kHz and up to 7 dB heavier
at 100–250 Hz than that median. Through Huge it came out thin and boxy. Learn
set +3.6…+5.1 dB of bite, +3…+3.6 dB of air and trimmed 100–160 Hz, which
brought its processed tone closer to the other singers' (band-by-band mismatch
3.2 → 2.1 dB).

## 3b. Momentary effects: Mega and Throw

Both are on only while held: on the pedal's AUX footswitch, the Pod's buttons,
the plugin's buttons or automation lanes, the playground's buttons or the M/T
keys, or MIDI CC 80/81.

- **Mega** turns the voice into a hard-clipped, mid-focused burst for accents.
  It's modelled on the producer-printed "MEGA DIST VOCAL" layer from the
  KnockedLoose session. Run on the same singer's clean main stem, it lands
  within ~4 dB of that layer's tone from 400 Hz to 18 kHz, with tighter lows.
  - It plays about 1–2 dB over the normal sound on three different singers.
  - It follows the voice's own envelope like a gated fuzz, so mic hiss
    between words stays quiet: engaging it into silence measures −94 dBFS.
  - Thicken and flavour stay on after it, so the burst sits in the same space.
  - While Mega is held, the feedback guard places no new notches. Its tone read
    as feedback once (a false notch at 645 Hz on a Gojira lead); with this
    rule, that take gives zero false notches.
- **Throw** sends the voice to its own quarter-note ping-pong delay. Let go and
  the repeats ring out under the next phrase (about −5 dB per repeat, ~4 s at
  120 bpm), while the main delay stays as set. It also works with flavour off.

`live_test momentary` holds Mega at 2–4 s and 6–8 s and Throw at 10–11 s, then
reports:

- the level change at each Mega edge
- click size against the plain run
- the Throw tail

The playground's JS engine matches the C++ engine to about −80 dB through the
same schedule.

### Stutter and glitch

Two more momentaries for the live rig, on the pedal's AUX 2 footswitch (tip =
Stutter, ring = Glitch), the plugin's STUTTER / GLITCH buttons, the
playground's buttons or S/G keys, or MIDI CC 88/89.

- **Stutter** is a beat-repeat. On the press it records one slice of the voice
  (Slice: 1/4, 1/8, 1/16 or 1/32 note at the tap, MIDI or host tempo) while you
  keep hearing yourself live. Then it loops that slice on the grid until you
  let go.
- **Glitch** loops the same way, but each repeat may roll to a half or quarter
  slice, play backwards, drop an octave (half speed) or cut out halfway. Glitch
  amount sets how often. The choices are seeded, so a render repeats exactly.
- It sits after Mega and before harmony, doubles, reverb and delay, so those
  all follow the loop. Mega + Stutter loops the burst.
- Loops get 2 ms fades and a 4 ms crossfade to and from the live voice. While
  a loop plays, the feedback guard places no new notches, because a loop
  repeats tones on purpose.

`live_test stutter` holds Stutter at 2–4 s and Glitch at 6–9 s. On a real take:

- the loop measures correlation 1.000 one slice apart, at 1/8, 1/16 and 1/32
- the largest sample step while held is below the voice's own, so no clicks
- Glitch repeats vary (correlation 1.00, 0.72, −0.03, 0.37 … at amount 1)

The playground matches C++ to −69 to −79 dB through the same schedule.

## 3c. Blind A/B listening test

`abtest/template.html` plus `python3 tools/build_abtest.py` builds a listening
page that runs the playground's engine in a worker, renders two versions of a
real DI that differ in one setting, and matches both to −20 LUFS (BS.1770).
You flip between them while they play (they stay in sync), pick one, and the
page reveals which was which. Picks go to the published page's database,
where Claude reads them to retune presets. The DI clips are published with the
page only; they're never committed.

## 4b. Logic Pro and other DAWs

The same engine runs as an Audio Unit (Logic Pro), VST3 and standalone app,
built with JUCE. A GitHub Actions job builds and validates the Mac version.
See [plugin/README.md](plugin/README.md) for download, install and behaviour.

## 5. Presets (playground)

| Preset | Use |
|---|---|
| Huge (album) | More drive, chop, doubles, plate and glue; output pushed into the limiter; open top (hi cut 17 kHz, +4 dB air) |
| Scream natural | The default |
| Scream dark | Dark voicing, more drive, darker thicken. A deliberate dark sound for thin or harsh takes, not a starting point for gutturals (see below) |
| Scream bright | Highs and squeals: bright voicing, more de-essing |
| Live stage | Tighter effects, more duck, a higher lo cut, bleed reducer set higher |
| Raw | Filters and light compression only |
| Air | Huge with a bright modern top: open hi cut, +7 dB air, more de-essing. A brighter version was rejected in the blind A/B test as "extremely harsh" |

**Fitted to producers.** `tools/presetfit.py` measures what a producer did to a
lead vocal (raw stem against the finished mix), then scores how close each
preset gets. Lower is better:

| Tone error, dB | Bloodbath | Gojira | Immortal Disfigurement | KnockedLoose |
|---|---|---|---|---|
| old Huge | 1.9 | 4.6 | 6.6 | 3.4 |
| **new Huge** | 1.7 | 0.5 | 5.3 | 3.4 |
| **Air** (+10 dB) | ~2.3 | ~3.5 | ~2.5 | ~3.9 |
| Dark | 3.8 | 8.4 | 9.8 | 4.2 |

- Every producer kept more air than the old hi cut, which is why new Huge
  improved everywhere.
- Dark fit worst on three of the four and was never closest, so
  `takes.py classify` no longer suggests it for growls.
- Air is for Immortal Disfigurement-style mixes that lift 6–10 kHz by about
  13 dB. On the others it overshoots the top, as expected.

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
  - Output stays stable and bounded at 48 and 96 kHz, even at extreme settings.
  - Drive aliasing is −72 dB at full drive.
  - The JS port matches C++ to −71 dB or better, including on a real vocal
    and on the live features.
  - The live features pass the tests above.
- **Both firmwares compile and link** against libDaisy v9.0.0.
- **Not done:**
  - Running on hardware: latency and CPU measurements (BUILD.md, Stage 1).
  - A schematic/PCB for the analog front end.
  - Tuning against more songs and styles.
