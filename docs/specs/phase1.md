# Phase 1 spec — core + `tonerender`

Goal: a C++20 core library and a `tonerender` CLI that renders a mono DI WAV through a
two-path preset (`docs/PRESET_SCHEMA.md`) using local `.nam` files and `.wav` IRs, with unit
and golden-file tests. No JUCE in phase 1. Tasks are delivered in order T1 → T4; each is
reviewed before the next starts.

## Global decisions (lead)

- **Deps (FetchContent, pinned):**
  - NeuralAmpModelerCore @ `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` (MIT). Build its `NAM/`
    sources into a static lib ourselves with `NAM_SAMPLE_FLOAT` defined; do not build its
    tools. Its Eigen submodule may be used, or Eigen 3.4.0 tag `3147391d946bb4b6c68edd901f2add6ac1f31f8c`
    from gitlab (MPL2) — pick one, document it.
  - nlohmann/json v3.11.3 (MIT). Catch2 v3.7.1 (BSL-1.0). PFFFT (marton78/pffft, pin a
    commit; BSD-style). dr_wav from mackron/dr_libs (pin a commit; public domain / MIT-0).
  - Mark dependency include dirs `SYSTEM` so `-Werror` applies only to our code.
- **Top-level CMake option** `SAWBLADE_BUILD_TESTS` (ON), targets: `sawblade_core` (static),
  `tonerender` (exe, T4), `sawblade_tests` (Catch2, registered with CTest via `catch_discover_tests`).
- **Processing interface** (all DSP blocks, mono, in place):
  ```cpp
  namespace sawblade {
  struct ProcessSpec { double sampleRate; int maxBlockSize; };
  class Processor {
  public:
    virtual ~Processor() = default;
    virtual void prepare(const ProcessSpec&) = 0;   // may allocate
    virtual void reset() = 0;                       // clears state; NOT RT-safe (may allocate,
                                                    // e.g. NAM prewarm) — call off the audio thread
    virtual void process(float* io, int numSamples) noexcept = 0; // RT-safe, n <= maxBlockSize
    virtual int latencySamples() const noexcept { return 0; }
  };
  }
  ```
- Samples `float`; coefficient/filter design math in `double`; biquad state may be `double`.
- **Allocation guard (test harness):** test executable replaces global `operator new/delete`
  (all variants incl. aligned/nothrow) with counting versions gated by a thread-local
  "armed" flag. `AllocGuard` RAII arms it; `count()` returns allocations while armed.
  Every block and the full chain get a test: after `prepare()`, process ≥ 10 blocks of
  varying sizes ≤ maxBlockSize while armed → `count() == 0`.

---

## T1 — skeleton, I/O and basic DSP

Deliver:
1. CMake project + deps above; `docs/THIRD_PARTY.md` (name, version/commit, license, use).
2. `core/include/sawblade/*.h`, `core/src/*.cpp`.
3. **WAV I/O** (`wav_io.h`): `readWav(path) -> AudioFile{ double sampleRate; int channels;
   std::vector<float> interleaved; }` supporting 16/24/32-bit PCM and 32-bit float;
   `writeWavFloat32(path, sampleRate, mono samples)`. Throws `std::runtime_error` with path
   in message on failure. Not RT (load-time only).
4. **Biquad + ParametricEq** (`eq.h`): RBJ cookbook peak, lowShelf, highShelf (shelf uses `q`
   as RBJ Q), highPass, lowPass; transposed direct form II; `ParametricEq` = cascade of up to
   16 bands configured before `prepare()`; disabled bands skipped. Expose
   `double magnitudeDb(double freqHz) const` (analytic, from coefficients) for tests and UI.
5. **Gate** (`gate.h`): per schema — peak envelope (0.1 ms attack / 10 ms release one-pole),
   open at `thresholdDb`, close at `thresholdDb - hysteresisDb` after `holdMs`; gain is
   smoothed by a one-pole toward its target using the attack/release time constants;
   closed gain = `rangeDb`. Gate has `processKeyed(const float* key, float* io, n)`
   (key = DI) and `process()` = self-keyed.
6. **Gain** and **DelayLine** (`delay.h`): integer delay, max set in `prepare`, settable delay
   ≤ max without allocation.
7. Tests (`tests/`), Catch2:
   - EQ: for each type at fs = 48 kHz and 44.1 kHz, measured magnitude response (sine at
     ≥ 6 frequencies incl. the center/corner, steady-state RMS) matches `magnitudeDb` within
     ±0.1 dB, and matches textbook expectations: peak gain at center = `gainDb` ±0.05 dB;
     HP/LP at corner with q=0.7071 = −3.01 ±0.05 dB; shelf far-band gain = `gainDb` ±0.2 dB.
   - EQ: invalid params (freq ≤ 0, ≥ 0.49·fs, q ≤ 0) rejected at configure time.
   - Gate: (a) signal at −20 dBFS passes with gain within 0.1 dB of unity after attack;
     (b) after the key drops to −80 dBFS, output reaches `rangeDb` ±1 dB within
     hold + 5·release; (c) hysteresis: key oscillating between threshold−3 dB and
     threshold+1 dB once opened does not close; (d) hold respected (still open at
     hold − 1 ms after key drops).
   - DelayLine: impulse delayed exactly n samples for n ∈ {0,1,7,max}.
   - WAV: round-trip float32; read 16/24-bit fixtures generated in-test via dr_wav.
   - AllocGuard self-test (a deliberate `new` is counted) + zero-alloc tests for EQ, gate, delay.

## T2 — NAM block, IR convolution, lock-free swap

1. **NamBlock** (`nam_block.h`): `static std::unique_ptr<NamBlock> load(path, NamBlockConfig)`
   (load-time; uses `nam::get_dsp`), exposes `expectedSampleRate()`, `loudnessDb()` (optional),
   metadata (name, gear_type, modeled_by). `prepare()` calls the model's `Reset(sr, maxBlock)`
   with prewarm. `process()` applies inputGain → model → outputGain (+ loudness
   normalization if configured: `(-18 − loudness)` dB) using a preallocated scratch buffer;
   bypass = passthrough. Throws at `prepare()` if `sampleRate != expectedSampleRate()` and the
   model does not support arbitrary rates. Latency 0 (NAM models are causal and
   latency-calibrated at training; document this).
   If NAM core allocates inside `process()` for any shipped architecture, report it with the
   call site — do not paper over it.
2. **IR loading** (`ir.h`): `loadIr(path, targetSampleRate, normalize=true)` → mono float
   vector: stereo → left channel (+ warning string returned), offline windowed-sinc
   (Kaiser, ≥ 64 taps/side) resampling when rates differ, truncate to 2.0 s, L2-normalize.
3. **Convolver** (`convolver.h`): **zero-latency** partitioned convolution: first P = 128 taps
   direct-form FIR in the time domain, remaining taps uniformly partitioned (partition 128,
   FFT 256, frequency-domain delay line) via PFFFT, with input buffered per sample so the
   tail partitions are computed at block boundaries of 128 regardless of host block size.
   `latencySamples() == 0`. All FFT setup/buffers allocated in `prepare()`/`setIr()`.
4. **SwapSlot<T>** (`swap_slot.h`): single-producer (message thread) / single-consumer
   (audio thread) handoff. Producer: `publish(std::unique_ptr<T>)` (object already prepared).
   Consumer: `T* current() noexcept` at block start — picks up the newest published object
   without locks/allocation, moving the replaced one to a retire queue. Producer:
   `collectGarbage()` deletes retired objects. Use `std::atomic` only; no mutex.
5. Tests:
   - NamBlock with a hand-written **Linear** `.nam` fixture (impulse response `[1,0,0,…]`,
     bias 0) → output == input × gains within 1e-6; a Linear fixture `[0.5, 0.25]` checks
     the model is actually applied (and loudness normalization arithmetic with a metadata
     loudness of −24 dB → +6 dB).
   - NamBlock on vendored example models (`wavenet.nam`, `lstm.nam` from NAM core
     `example_models/`, MIT — copy into `tests/fixtures/nam/` with a LICENSE note): finite
     output, deterministic across two runs (bit-identical), and block-size invariance:
     rendering 2 s of noise with block sizes {64, 128, 1000} differs ≤ 1e-5 max-abs.
   - Sample-rate mismatch throws.
   - Convolver vs. naive direct convolution: IR lengths {1, 100, 128, 129, 257, 4800,
     96000}, block sizes {1, 32, 128, 300, 512}: max abs error ≤ 1e-5 × (L1 norm of IR).
     Latency 0 proven by impulse: output[0] == ir[0].
   - IR resample: 44.1 k IR of a 1 kHz windowed sine resampled to 48 k has peak spectrum at
     1 kHz ±5 Hz and RMS within 0.1 dB; stereo → left + warning; truncation at 2 s; L2 = 1.
   - SwapSlot: stress test — producer publishes 10 000 objects while consumer thread calls
     `current()` in a loop and checks object integrity (magic value); all objects deleted
     (instance counter returns to 0 after final `collectGarbage()`).
   - Zero-alloc tests for NamBlock (both example models + Linear), Convolver, SwapSlot::current.

## T3 — preset model and the two-path chain

1. **Preset** (`preset.h`): C++ structs mirroring `docs/PRESET_SCHEMA.md` v1;
   `Preset parsePreset(const nlohmann::json&, const std::filesystem::path& baseDir)`,
   `nlohmann::json toJson(const Preset&)`, `Preset loadPresetFile(path)`.
   Strict: unknown keys, wrong types, missing required fields, out-of-range values, and
   `version` > 1 all throw `PresetError` whose message contains the JSON path
   (e.g. `paths.a.blocks[1].model.file`). Defaults per schema. Paths resolved relative to
   the preset file.
2. **Chain** (`chain.h`):
   - **Block registry** (`block_registry.h`): maps a block `type` string to a factory
     producing a `Processor` plus static traits `{ bool namTrainable; }`. Register `nam` and
     `eq` in phase 1. Each path's chain is built by iterating its `blocks` through the
     registry — no `if (type == "nam")` branching in `Chain`. This is the extension point
     for future modeled pedals; keep it small (no plugin loading, no params system yet).
   - `ChainResources loadResources(const Preset&, double sampleRate)` (load-time/background):
     builds every block via the registry (loading NAM models), loads IRs, verifies optional `sha256` (implement SHA-256 or vendor a
     small public-domain one), collects warnings.
   - `Chain(const Preset&, ChainResources&&)`; `prepare(spec)`; `process(const float* in,
     float* out, int n) noexcept` implementing the signal graph in `CLAUDE.md`.
   - Latency: per-path latency = sum of block latencies (+ per-path cab when `perPath`);
     shorter path delayed to match; `latencySamples()` = total chain latency.
   - Alignment: `AlignResult resolveAlignment()` (not RT; uses the deterministic probe per
     schema, measured at the blend point — includes per-path IRs in `perPath` mode; resets
     all state afterwards). `auto` mode is resolved during `prepare()`;
     positive `delaySamplesB` delays B, negative delays A; `invertB` flips B; max lag
     buffer sized from `maxLagMs`.
   - Blend linear per schema; disabled path contributes silence; cab `shared` vs `perPath`;
     `cab.enabled=false` bypasses; post EQ; bus comp (feed-forward peak, soft knee, per
     schema); output gain.
   - `ChainInfo info()`: latency per path/total, resolved alignment, `liveCompatible`,
     `exportExactness`, warnings (incl. busComp release > 150 ms "not NAM-trainable").
3. Tests:
   - Registry: unknown block `type` → `PresetError` naming the path; duplicate block `id` → error;
     an `eq` block mid-chain behaves identically to the same bands in path `eq`.
   - Preset: example from the schema doc parses; round-trip `parse(toJson(p)) == p`; one test
     per error class asserting the JSON path appears in the message.
   - Alignment: path A uses a Linear identity model; path B uses a Linear model whose IR is
     a single −1 at index 23 (B lags A by 23 samples and is inverted). Auto-align must
     resolve `delaySamplesB == -23` (A is delayed by 23 to meet B) and `invertB == true`.
     With blend 0.5 the chain output equals path A's output delayed by 23 samples within
     1e-5 (paths sum constructively). Also: identical paths → (0, false); an offset beyond
     ±`maxLagMs` is not found (result stays within the window).
   - Latency compensation: a test-only Processor stub reporting latency N in one path proves
     paths are re-aligned and `latencySamples()` is reported correctly.
   - Blend 0 = only A, 1 = only B (bit-exact vs a single-path render).
   - `shared` vs `perPath` with the same IR on both paths produce the same output (≤ 1e-5)
     and `liveCompatible` is true/false respectively.
   - Bus comp: static curve check (steady sine above threshold settles to the analytic gain
     ±0.2 dB) and makeup.
   - Whole-chain zero-alloc test and block-size invariance ({1, 64, 256, 1000}, ≤ 1e-5).

## T4 — `tonerender` CLI, fixtures, golden tests

1. `cli/tonerender`:
   `tonerender --preset P.json --in DI.wav --out OUT.wav [--block N=256] [--report R.json]
   [--normalize-peak dBFS]` (no normalization by default). Input must be mono (stereo →
   left with warning). Output: float32 mono WAV at the input rate, same length as the input,
   latency-compensated (output advanced by `latencySamples()`; tail flushed with zeros).
   Exit codes: 0 ok, 2 usage, 3 preset error, 4 I/O/model error. Errors to stderr.
   Report JSON: preset name, sample rate, block size, `ChainInfo` fields, input/output peak
   and RMS dBFS, render time, real-time factor, warnings, capture attributions
   (title/creator/license for every capture that has `source`).
2. Fixtures (`tests/fixtures/`), all deterministic and generated by a committed generator
   (`tests/tools/make_fixtures.cpp`, built as a target, plus `scripts/regen_fixtures.sh`):
   - `di_riff.wav`: 4 s @ 48 kHz, 24-bit — Karplus–Strong low-E/A power-chord-ish plucks with
     palm-muted (short decay) and open hits, gaps of near-silence with −70 dBFS noise
     (to exercise the gate).
   - `ir_a.wav` (48 k, ~200 ms decaying filtered noise), `ir_b.wav` (44.1 k, different
     spectrum — exercises resampling).
   - Linear `.nam` fixtures from T2; example NAM models from T2.
   - Presets: `golden_shared.json` (gate on, A = [wavenet, lstm] two nam blocks,
     B = [linear boost, wavenet], auto align, shared cab ir_a, post EQ, bus comp on),
     `golden_perpath.json` (per-path IRs ir_a/ir_b, align manual).
3. Golden tests: render each golden preset via the library entry point used by the CLI;
   compare with `tests/golden/<name>.wav`: max abs diff ≤ 1e-4 and length equal. Setting env
   `SAWBLADE_UPDATE_GOLDEN=1` rewrites goldens (and the test reports it did). Commit goldens.
4. CLI tests (CTest): runs the binary on the golden preset → exit 0, output WAV exists and
   matches golden ≤ 1e-4, report JSON parses and contains `liveCompatible`; bad preset → exit 3;
   missing input → exit 4.
5. `README.md`: what Sawblade is, build, test, run `tonerender`, regenerate goldens.
