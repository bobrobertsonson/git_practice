# Phase 5.1 report: core `StemPlayer` (play-along backing)

Spec: `docs/specs/phase5_1_stemplayer.md` (with lead amendments §3, §4 and §9).
Status: **accepted by the lead after reviewer ACCEPT (round 2).** The plugin is untouched, and
no audio is committed.

## What was built
- **Decoding** (`wav_io.h`):
  - `readAudioFile()` reads WAV, and FLAC through `dr_flac.h` from the already pinned dr_libs
    commit (no new dependency; `docs/THIRD_PARTY.md` has the row).
  - `writeWavFloat32Stereo()` writes the stereo output.
- **`StemSet` + loader** (`stem_set.h/.cpp`):
  - Five named stems (drums, bass, vocals, other, guitar). Mono files are duplicated to both
    channels, and only the first two channels of a wider file are kept.
  - Every stem is resampled with the offline Kaiser `resample()` (equal rates pass through
    bit-identical). Shorter stems are zero-padded to the longest.
  - `loadStemDirectory()` maps file names to stems and accepts `guitars` as an alias. Unknown
    audio files (e.g. `piano.wav`) are summed into `other` with a warning. Duplicate stems are
    summed with a warning. Files are read in sorted-name order.
  - `loadStemFiles()` and `makeStemSet()` build a set from explicit files or from audio already
    in memory.
- **`StemPlayer`** (`stem_player.h/.cpp`):
  - **Set handoff:** sets go to the audio thread through `SwapSlot`. A replacement set is
    adopted only while the transport is stopped. The first set is adopted immediately, and the
    playhead is kept. Old sets are freed only in `collectGarbage()`.
  - **Engine:** one sample at a time, two read heads, and equal-power crossfade tables built in
    `prepare()`.
  - **Transport:**
    - play and pause fade over 5 ms;
    - seek is sample-accurate, with a 5 ms equal-power crossfade;
    - loop A–B crossfades over 10 ms. A wrap that falls during another crossfade is deferred
      and lands at `a + overshoot`, so the loop period stays exact;
    - the count-in clicks at `round(k·60·fs/bpm)` per beat, with an accent click on each
      downbeat.
  - **Mix:**
    - per-stem gain, mute and solo; mute wins over solo;
    - guitar mode Muted (the default) / Ghost (−12 dB) / Full;
    - master level;
    - all gain changes ramp linearly over 20 ms, counted in double precision, so they don't
      depend on block size.
  - **Latency:** the whole output, clicks included, is delayed by the rig latency through
    preallocated `DelayLine`s.
  - **Host-follow:**
    - host play and stop map to play and pause fades;
    - a position jump of more than 64 samples (the default) starts a declicked seek;
    - jitter up to that limit is ignored;
    - a jump during a running crossfade is deferred and corrected for the samples that passed.
- **Python bindings:** `load_stems`, `stem_set_from_arrays`, `StemSet` and `StemPlayer`.
  `process(n)` returns a float32 array of shape `(2, n)`.
- **CLI:** `tonerender --backing <dir> [--backing-level dB] [--guitar-stem mute|ghost|full]`
  writes a stereo float32 WAV: L/R = latency-compensated guitar + backing L/R. The guitar stem
  is muted by default. The report gains a `backing` object. Without `--backing`, output and
  report are unchanged.

## Tests
- **Release build:** `ctest` 157/157 passed, zero warnings under `-Werror`. This includes 37 new
  Catch2 cases (`[stems]`, `[stemplayer]`, `[backing]`).
- **Python build** (`-DSAWBLADE_BUILD_PYTHON=ON`): 158/158 passed. The `python_bindings` entry
  now also runs `match/tests/test_stem_player_bindings.py` (10 tests).
- **ASan/UBSan Debug:** the new tests are clean. The full existing suite was not run under the
  sanitizers.
- **Coverage of spec §7:** every item is covered.
  - **Loader:** includes FLAC, using a minimal FLAC writer in the test.
  - **Gain ramps and mix:** analytic gain ramps; mute, solo and ghost mode.
  - **Loop crossfade:** continuity, with a hard-cut control that fails the same threshold, and
    an exact loop period.
  - **Seek:** declick and sample accuracy.
  - **Count-in:** bit-exact click placement at 120 bpm / 48 kHz, 137 bpm / 44.1 kHz, and in 3/4.
  - **Latency alignment:** against a real `Chain` with a 37-sample `test.latency` block.
  - **Host-follow:** steady advance (identical to free-run), the jump threshold edges at 64 and
    65 samples, a jump during a crossfade, and stop and start.
  - **Determinism:** block sizes 1, 7, 64, 512 and 4096 give bit-identical output.
  - **Real-time safety:** zero allocations and zero frees in `process()`, including the
    adoption block.
  - **Set adoption** and the **CLI** behaviour.

## Reviewer verdicts
1. **Round 1, REVISE.** The two lead amendments (adopting the first set in any transport state,
   and the duplicate-stem warning) were missing, along with their tests. There were four
   non-blocking items. All were fixed in `a017392`.
2. **Round 2, ACCEPT.** No must-fix or should-fix items. The reviewer traced the crossfade,
   wrap, count-in, host-follow and latency logic by hand and audited real-time safety.

## Accepted deviations (spec §9)
- The ramps use their own double-precision, counter-based ramp, with the same law as
  `Gain::rampToLinear`.
- `AllocGuard::frees()` was added.
- No internal block splitting.
- `isPlaying()` reports the requested state.
- A seek to exactly `b` does not wrap.

## Open questions / proposals for the main lead
1. **Memory.** Five stereo stems held as float32 cost about 350–580 MB for a 5-minute song.
   Proposal: int16 or half-float storage, or streaming from disk, in a later phase.
2. **Load time.** The offline Kaiser resampler is O(N·taps) per channel. A full song that is not
   at 48 kHz can take tens of seconds to load. Options: resample the stems in parallel, or cache
   resampled stems.
3. **Phase 5.2 (plugin):**
   - a thread-safe command FIFO from the message thread to the audio thread (today the setters
     must be called on the audio thread);
   - a song-start offset in the host timeline;
   - the plugin calling `setRigLatencySamples(chain.latencySamples())` when a preset changes.
     A latency change while playing is not click-free.
4. **Live latency semantics.** Delaying the backing by the rig latency lines up the *output*:
   recorded or monitored guitar against the song. A live player still hears their guitar L
   samples after they pick. That is unavoidable, and fine for typical NAM rig latencies.
5. **CLI.** `--normalize-peak` applies to the guitar render only, before mixing. The mix can
   exceed 0 dBFS in float. Decide whether to add a mix-level normalize.
6. **Settle block.** The CLI runs one block while the transport is stopped, so the
   `--backing-level` and `--guitar-stem` settings apply from the first output sample instead of
   ramping in over 20 ms. A "start at target, no ramp" option on the player would be cleaner.
