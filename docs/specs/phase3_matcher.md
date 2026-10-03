# Phase 3 spec — matcher v1

## 3.1 Python bindings (dsp-engineer)
`sawblade_core` pybind11 module (pin pybind11; build via CMake option `SAWBLADE_BUILD_PYTHON`):
- `render(preset_json: str|dict, audio: np.ndarray[float32], sample_rate: float,
  render_rate="auto", out_rate="input", block=256) -> (np.ndarray, report: dict)` using the
  same `renderPreset` path as tonerender (bit-identical output — tested against the CLI).
- `CaptureCache`: load each .nam / IR once and reuse across renders (keyed by path+sha256);
  renders that differ only in continuous parameters must not reload models.
- Release the GIL during rendering; thread-safe for parallel renders of different chains.
- Tests (pytest + CTest): bit-identity vs tonerender on the golden presets; cache hit avoids
  reload (timing or counter); errors map to Python exceptions with the JSON path.

## 3.2 Matcher (match-engineer)
`sawblade-match --di DI.wav --ref REF.wav [--ref-section a:b ...] --pool pool_manifest.json
 --out DIR [--budget N] [--seed S]`
- **Search space**: discrete — HM-2 model, saw amp model, boost model (or none), body amp
  model, cab IR (all from the pool, any model of each tone); continuous — blend, per-path
  levels, per-path EQ (3 peaking bands + HP/LP within bounded ranges), NAM input gains
  (±12 dB), post EQ (3 bands), gate threshold from the DI noise floor (fixed, not searched).
  Live-compatible (shared cab) by default.
- **Loss** (documented weights): A-weighted 1/3-oct LTAS error vs the reference guitars
  (isolated stem or calibrated sections) after removing an overall level offset; plus buzz
  and lowDecay differences; plus a time-aligned multi-resolution STFT term **only** when the
  reference is a matched pair (cover mix: alignment offsets from docs/TEST_MATERIAL.md,
  refined to sample accuracy by the tool).
- **Optimizer**: stage 1 screens discrete combos on short excerpts (≤ 20 s, guitar-dominant)
  with default continuous params; stage 2 CMA-ES (seeded) on the top-K combos; stage 3 full-
  length verification with tonecheck. Deterministic given `--seed`.
- **Preferences**: smaller model size when within 0.3 dB loss; no non-trainable blocks;
  reject solutions that clip.
- **Output**: best preset (TONE3000 `source` ids + modelIds, so it is resolvable and
  attributable), top-5 alternatives, loss breakdown, tonecheck report, rendered audio
  (git-ignored).
- **Validation (acceptance)**: (a) known-answer: render the cover DIs through a hidden preset
  built from pool captures, then recover it — the found preset's loss must be within 0.5 dB
  A-weighted of the hidden one; (b) cover DI → cover mix: report A-weighted error before vs
  after; (c) cover DI → original: same.
