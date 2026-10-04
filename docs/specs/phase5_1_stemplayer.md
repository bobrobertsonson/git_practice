# Phase 5.1: core `StemPlayer` (play-along backing)

## Why
The user matches a tone to a song, then plays along with that song, guitars removed, through
the matched rig. Stem separation is a separate spike; this task assumes the stems already exist
as audio files (one file per stem). It adds the core player, its loader, Python bindings and a
`tonerender --backing` offline play-along render. **The plugin is untouched** (integration is
phase 5.2). Never commit audio: all tests use synthetic stems generated in the test.

Owner: dsp-engineer (core, cli, bindings, tests). Reviewer audits. The usual hard rules of
CLAUDE.md apply (RT safety, determinism, pinned deps, `-Werror`).

## 1. Files
- `core/include/sawblade/stem_set.h`, `core/src/stem_set.cpp`: `StemKind`, `StemSet`, loader.
- `core/include/sawblade/stem_player.h`, `core/src/stem_player.cpp`: `StemPlayer`.
- `core/include/sawblade/wav_io.h`: add `readAudioFile()` (WAV + FLAC) and a stereo float32 writer.
- `core/src/dr_flac_impl.cpp`: dr_flac implementation TU, compiled in the existing
  third-party library target (`sawblade_dr_wav`, `-w`), like `dr_wav_impl.cpp`.
- `tests/test_stem_set.cpp`, `tests/test_stem_player.cpp` (+ CLI cases in the existing render/CLI tests or a new file).
- `cli/main.cpp`: `--backing` options.
- `bindings/python/sawblade_core_module.cpp` + a pytest file run by the `python_bindings` ctest entry.
- `docs/THIRD_PARTY.md`: dr_flac row.

## 2. Decoding
- WAV: existing dr_wav path.
- FLAC: `dr_flac.h` from the **already pinned** `dr_libs` commit
  (`dfe8377631000664666519fdb83da193fd8037f4`, public domain / MIT-0). No new dependency
  fetch. Record the use in `docs/THIRD_PARTY.md`.
- `AudioFile readAudioFile(const std::filesystem::path&)` dispatches on the extension
  (`.wav`, `.flac`, case-insensitive) and throws `std::runtime_error` (path in the message)
  for anything else or on decode failure. `readWav` is unchanged.
- `void writeWavFloat32Stereo(path, sampleRate, const std::vector<float>& left, const std::vector<float>& right)`
  (equal lengths required; throws `std::runtime_error`).

## 3. `StemSet` and the loader (load-time; may allocate, throw, do I/O)

```cpp
enum class StemKind : int { Drums = 0, Bass, Vocals, Other, Guitar };
constexpr int kStemKindCount = 5;
const char* stemKindName(StemKind);            // "drums", "bass", "vocals", "other", "guitar"

struct StemInfo { std::string file; double sourceRate; int sourceChannels; std::int64_t sourceFrames; };

struct StemSet {                               // immutable after construction
  double sampleRate = 0.0;
  std::int64_t length = 0;                     // frames; the longest stem; shorter ones are zero-padded
  std::array<bool, kStemKindCount> present{};
  std::array<std::array<std::vector<float>, 2>, kStemKindCount> audio;  // [kind][L,R], each `length` long when present, empty otherwise
  std::array<std::vector<StemInfo>, kStemKindCount> sources;            // files that went into each stem
  std::vector<std::string> warnings;
};
```

- Channels: mono files are duplicated to L and R. Files with more than two channels keep the
  first two, with a warning.
- Every stem is resampled to the requested `sampleRate` with the existing offline Kaiser
  `resample()` (bit-identical pass-through at equal rates), per channel.
- `StemSet loadStemFiles(const std::vector<std::pair<StemKind, std::filesystem::path>>& files, double sampleRate)`:
  several files may map to the same kind; they are summed (after resampling).
- `StemSet loadStemDirectory(const std::filesystem::path& dir, double sampleRate)`:
  scans the directory (non-recursive) for `*.wav` / `*.flac` (case-insensitive). Base names
  map as: `drums`, `bass`, `vocals`, `other`, `guitar` or `guitars` (case-insensitive) to
  their kind. Any other audio file (e.g. Demucs 6-stem `piano.wav`) is summed into `other`
  with a warning naming it, so no music is silently dropped. Non-audio files are ignored.
  Files are processed in sorted-name order (determinism).
- `StemSet makeStemSet(double sampleRate, const std::array<std::optional<std::array<std::vector<float>,2>>, kStemKindCount>& audio)`
  (or an equivalent factory) for tests and bindings: audio already at `sampleRate`, padded to the longest.
- Errors (`std::runtime_error`, path in the message): unreadable/undecodable file, no stems
  found, non-positive rate, a stem with zero frames.

## 4. `StemPlayer`

### Threading
- **Producer thread** (message/loader): `prepare()`, `reset()`, `setStemSet()`, `collectGarbage()`.
- **Audio thread**: `process()` and every transport / mix / host setter below. They are RT-safe
  and must be called on the audio thread, or otherwise serialised with `process()` (a thread-safe
  command queue for the plugin is phase 5.2). Setters take effect at the start of the next
  `process()` call; that block boundary is the sample they act on.
- `setStemSet(std::unique_ptr<StemSet>)` publishes through the existing `SwapSlot<StemSet>`.
  It throws `std::invalid_argument` on the producer thread if the set's rate differs from the
  prepared rate. The audio thread **adopts** a pending set only at the start of a block in
  which the transport is fully stopped (paused, pause fade complete, not counting in). While
  playing, the pending set waits. Adoption resets the playhead to 0 and clears the loop. The
  old set is retired by `SwapSlot` and freed only by `collectGarbage()` on the producer thread.
- With no set adopted, stems output silence (count-in clicks still work).

### prepare
`void prepare(const ProcessSpec& spec, int maxRigLatencySamples)`: allocates everything:
the two latency `DelayLine`s (L/R, existing class) sized to `maxRigLatencySamples`, the click
buffers, the crossfade gain tables, scratch buffers for `maxBlockSize`. A previously adopted set
whose rate differs from `spec.sampleRate` is dropped. `process(n)` accepts any `n >= 0`
(split internally to `maxBlockSize`, as `Chain` does).

### process
`void process(float* outL, float* outR, int n) noexcept` **overwrites** both outputs with the
backing mix + count-in clicks, delayed by the rig latency. No allocation, deallocation, locks,
I/O, exceptions or logging.

Signal per channel:
`out = delay_rigLatency( transportGain * masterGain * Σ_kind stemGain[kind] * read(kind, head)  +  clicks )`
where `read` returns 0 outside `[0, length)` and the crossfade engine below may sum two heads.

### Transport (free-run mode)
- Playhead: `std::int64_t`, in samples of the set's rate. `position()` returns it.
- `play()` / `pause()`: a linear 5 ms (`kTransportFadeMs`) transport-gain ramp. `play()` starts
  the playhead at the current position and ramps 0→1; `pause()` keeps advancing the playhead
  while ramping 1→0, then stops it. `isPlaying()`.
- Reaching the end of the set without a loop: the playhead keeps advancing and outputs zeros
  (no auto-stop; `atEnd()` reports it).
- `seek(std::int64_t pos)` (clamped to `[0, length]`):
  - while stopped: the playhead jumps, no fade;
  - while playing: **sample-accurate** — the first output sample of the next block reads
    `pos` (new head) — with an **equal-power crossfade of 5 ms** (`kSeekFadeMs`): the old head
    continues from where it was and fades out while the new head fades in.
- **Crossfade engine**: at most two read heads. Fade-in gain `sin(π/2·t)`, fade-out
  `cos(π/2·t)`, `t = (k + 0.5) / N`, k = 0..N-1, from tables built in `prepare()` (no trig in
  `process()`). A seek or loop wrap that arrives while a crossfade is running is applied when
  that crossfade ends; a deferred loop wrap lands at `A + (overshoot past B)` so the loop
  period stays exact.
- **Loop A–B**: `bool setLoop(std::int64_t a, std::int64_t b)`, `void clearLoop()`. Rejected (returns
  false, nothing changes) unless `0 <= a < b <= length` and `b - a >= 2·N_loop`. When the
  playhead crosses `b` from below, it wraps: the sample after `b-1` is `a` (new head, fading in)
  while the old head continues past `b` (fading out) with an **equal-power crossfade of 10 ms**
  (`kLoopFadeMs`). A playhead already at or beyond `b` when the loop is set plays on without
  wrapping.
- **Count-in**: `void setCountIn(int bars, double bpm, int beatsPerBar = 4)`
  (bars 0..8, 0 = off; bpm 30..300; beatsPerBar 1..12; out-of-range values are clamped).
  `play()` with count-in enabled (free-run only) holds the playhead for
  `C = round(bars·beatsPerBar·60·fs/bpm)` samples and outputs clicks. Beat k
  (k = 0 .. bars·beatsPerBar−1) starts at `round(k·60·fs/bpm)` samples after the block in
  which `play()` took effect (computed per beat in double, no accumulated drift). Beat
  `k % beatsPerBar == 0` uses the accent click. After C samples the stems start at the
  playhead with the normal 5 ms play fade-in. A click that runs past C is still played out in
  full (summed). `pause()` during the count-in cancels it. `isCountingIn()`.
- **Click buffers** (built in `prepare()`): `s[n] = A·sin(2π f n / fs)·exp(-n / (0.005·fs))`,
  `n < round(0.030·fs)`; f = 1500 Hz (accent), 1000 Hz (normal); `A` from
  `setClickLevelDb()` (default −6 dBFS, range −60..0), the same on L and R. Clicks are not
  affected by stem gains or the master backing level. Expose read-only access to the buffers
  (for tests).

### Mix
- `setStemGainDb(StemKind, double dB)` (−60..+12, clamped), `setStemMute(StemKind, bool)`,
  `setStemSolo(StemKind, bool)`, `setMasterLevelDb(double)` (−60..+12).
- Audible: `!muted && (noStemSoloed || soloed)`.
- `setGuitarMode(GuitarMode)`, `enum class GuitarMode { Muted, Ghost, Full }`, **default
  `Muted`**; multiplies the guitar stem by 0, −12 dB (`kGhostGuideDb`), 1.
- Effective stem gain = `dbToLin(gainDb) · audible · guitarModeFactor`. Every change of an
  effective stem gain or of the master level ramps linearly over **20 ms** (`kMixRampMs`,
  same law as `Gain::rampToLinear` / `Chain::kLiveRampMs`), sample-accurate. Setting the same
  value again does not restart a ramp.

### Latency compensation
`setRigLatencySamples(int)` (RT-safe, clamped to `[0, maxRigLatencySamples]`; changing it
while playing is not click-free, documented). The whole output (stems and clicks) is delayed
by it, so the backing lines up with a Chain that reports that latency (`Chain::latencySamples()`).
`StemPlayer::latencySamples()` stays 0 (the delay is deliberate, not processing latency).

### Host-follow mode
`setTransportMode(TransportMode::FreeRun | HostFollow)` (default FreeRun) and
`setHostPosition(std::int64_t hostSample, bool hostPlaying)`, called before each `process()` in
host-follow mode (`hostSample` = the host position of the block's first sample, in samples at
the set's rate).
- Host starts playing: playhead = hostSample (no crossfade), 5 ms play fade-in.
- Host stops: 5 ms pause fade (as `pause()`).
- While playing, the expected position is the internal playhead. If
  `|hostSample − playhead| > hostJumpThreshold` (`setHostJumpThresholdSamples`, default
  `kDefaultHostJumpThreshold = 64`), a declicked seek to hostSample (5 ms equal-power) is
  started at that block. Smaller differences are ignored (the internal clock runs at the same
  rate). Negative positions (pre-roll) read silence.
- In host-follow mode the internal A–B loop and the count-in are inactive (the host owns
  position); `play()`/`pause()`/`seek()` calls are ignored.

### Determinism
Same set + same command sequence at the same sample times → bit-identical output, for any
block size, in free-run mode (commands act at block boundaries, so the test splits blocks at
command times). Host-follow is evaluated per block by definition; its tests use a fixed block size.

## 5. Bindings (`SAWBLADE_BUILD_PYTHON`)
- `load_stems(dir, sample_rate) -> StemSet` and `stem_set_from_arrays(dict[name -> ndarray (n,) or (2,n) float32], sample_rate) -> StemSet`; `StemSet` exposes `sample_rate`, `length`, `present` (list of names), `warnings`.
- `StemPlayer` with `prepare(sample_rate, max_block, max_rig_latency)`, `set_stem_set`,
  transport/mix/host setters (snake_case), and `process(n) -> ndarray (2, n) float32`.
  Python holds a `StemSet` by value/shared object; `set_stem_set` copies it into a new
  `unique_ptr` (fine off the audio thread). Errors map to Python exceptions.
- Pytest cases (in the `python_bindings` ctest entry): load from arrays, solo/mute effect,
  `process` shape/dtype, guitar muted by default. Skip cleanly if the module is not built.

## 6. `tonerender --backing`
- `--backing <dir>`: render the DI through the preset exactly as now (latency-compensated,
  same rate/length rules), then load the stems with `loadStemDirectory(dir, r.sampleRate)`, run
  a `StemPlayer` (rig latency **0**, because `renderPreset` already advances the guitar by the
  chain latency; free-run, play from 0, no count-in, processing block = `--block`) for the
  output length, and write a **stereo** float32 WAV: `L = guitar + backingL`, `R = guitar + backingR`.
  The backing is truncated / zero-padded to the guitar length.
- `--backing-level dB` (master backing level, default 0, −60..+12), `--guitar-stem mute|ghost|full`
  (default `mute`). Both are usage errors (exit 2) without `--backing`.
- `--normalize-peak` still applies to the guitar render only (before mixing); document it.
- Report (`--report`): add a `backing` object: `dir`, `levelDb`, `guitarStem`, `stems`
  (per present kind: `kind`, `files` with source rate/channels/frames), `warnings`, and
  `outputChannels: 2`. Stem loading errors → exit 4.
- Without `--backing`, output and report are unchanged (mono goldens still bit-identical).
- Usage text updated.

## 7. Acceptance tests (Catch2 unless noted; synthetic stems built in the test)
1. **Loader**: WAV mono→stereo duplication; a 44.1 kHz stem resampled to 48 kHz has the length
   `resampledLength()` and matches `resample()` bit-exactly; equal-rate load is bit-identical
   to the source; FLAC decode: dr_flac has no encoder, so the test contains a minimal FLAC
   writer (STREAMINFO + frames with VERBATIM subframes, 16-bit, CRC-8/CRC-16) and checks that
   `readAudioFile` returns the written samples exactly (mono and stereo); directory scan maps names, `guitars` alias, unknown file summed into `other`
   with warning, no-stems error, sorted-order determinism. Test files go to a temp dir, never
   into the repo.
2. **Gain ramps**: a stem gain change ramps linearly over exactly `round(0.020·fs)` samples
   (check start, midpoint, end values against the analytic ramp within 1e-6); master level too.
3. **Mute/solo**: mute removes a stem after the ramp; solo of one stem silences the others;
   mute wins over solo; ghost mode = −12 dB ±0.01 dB; guitar is muted by default.
4. **Loop crossfade continuity**: a 220 Hz sine stem, loop points chosen so the phase at `b`
   and `a` mismatch by ~π. Max |y[n] − y[n−1]| over the wrap region ≤ 1.5 × the sine's own max
   step + 1e-4; the test also computes the hard-cut (no crossfade) splice and shows it exceeds
   the threshold (so the test is meaningful). Loop period exact: a marker impulse at `a` recurs
   every `b − a` samples over ≥ 3 wraps (after the fade).
5. **Seek declick**: same continuity criterion for a seek while playing; the new head reads `pos`
   at the first sample of the block after `seek()` (verified with an impulse/ramp stem, fade-in gain applied).
6. **Count-in timing**: bars=2, bpm=120 (and a non-integer beat length, e.g. bpm=137 at 44.1 kHz):
   output equals the accent/normal click buffers placed exactly at `round(k·60·fs/bpm)`
   (bit-exact apart from overlaps), zero elsewhere during the count-in; the first stem sample
   appears at `C` with the play fade-in.
7. **Latency alignment**: a Chain built from a preset with the `test.latency` stub block
   (e.g. 37 samples; `tests/latency_stub.h`) and a stem with an impulse at sample i; DI impulse
   at sample i; with `setRigLatencySamples(chain.latencySamples())`, the impulse peaks of the
   chain output and the stem player output land on the same sample.
8. **Host-follow**: steady host advance → no seek and output identical to free-run; a jump >
   threshold triggers a 5 ms crossfade to the host position (continuity criterion holds, new
   position read exactly); a jitter ≤ threshold is ignored; host stop/start fades.
9. **Block-size independence**: a scripted free-run session (play, gain change, mute, seek,
   loop wrap, pause, count-in play) rendered with block sizes 1, 7, 64, 512 and 4096 →
   bit-identical outputs.
10. **Zero allocations** (`AllocGuard`): `process()` across play, ramps, seek, loop wraps,
    count-in, host jumps, rig latency change, and the block in which a pending set is adopted.
    Also: the old set is not destroyed in `process()` (it is still alive until `collectGarbage()`).
11. **Set adoption**: a set published while playing is not adopted until stopped; after
    adoption the playhead is 0 and the loop is cleared; rate mismatch throws in `setStemSet`.
12. **CLI** (runs the real `tonerender`): `--backing` with a synthetic stem dir (written to a temp
    dir by the test) produces a stereo WAV of the DI length where `L − R` equals the backing
    L − R difference and the guitar stem is absent by default (present with `--guitar-stem full`);
    `--backing-level` scales the backing; report has the `backing` object; `--guitar-stem` without
    `--backing` → exit 2; bad dir → exit 4; without `--backing` the existing goldens pass unchanged.
13. Pytest cases of §5 (when the Python build is available).
14. The full existing suite still passes (C++ and, if built, Python).

## 8. Out of scope
Plugin integration, a thread-safe command FIFO, time-stretching / pitch-shift, stem separation,
tempo maps, song-start offset in host timeline, waveform display, compressed in-memory storage.
Propose these in the report instead of building them.
