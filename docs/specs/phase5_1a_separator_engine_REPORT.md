# Phase 5.1a separator engine bake-off: report (lead)

Spec: `docs/specs/phase5_1a_separator_engine.md`. Raw data, commands, hashes and the Apple derivation:
`spikes/separator/RESULTS.md`, section "Phase 5.1a". Branch `claude/sawblade-p5-1a-separator-engine`.
Commits: 8cf2153 (spec), 72f7be0 (code), 117b3ba (results), plus the lead's doc-nit fix and this report.

**Reviewer verdict: ACCEPT** (first round, no must-fix items). The reviewer did the following themselves:
- Ran the default build + ctest: 181/181 pass, and nothing new is fetched.
- Built the spike with BLAS and ONNX ON: 0 warnings, with a fresh sha256-pinned ORT download.
- Re-ran both engines on real7 6s: the stems are byte-identical to the stored ones.
- Re-ran stock Python: the null residuals match the RESULTS table exactly.
- Re-timed ORT 6s at 4 threads: 35.6 vs 34.0 s/min.
- Checked cancel: 1.6 s.
- Checked the patch and the STFT/iSTFT line by line against demucs 4.0.1.

I fixed three wording and number nits in RESULTS.md myself: the OpenBLAS ±% sign, the arena figure (23% not 25%), and how the 3 GB bar is defined.

## Environment

- **Machine:** Xeon Cascade Lake, 4 vCPU, AVX-512, 15 GB.
- **demucs.cpp build:** `-march=x86-64-v3`.
- **ONNX Runtime:** ORT 1.30.0 (CPU EP).
- **Python reference:** demucs 4.0.1 / torch 2.5.1, stock `apply_model(shifts=0)` as the reference.

This is a slower host than in 5.0: Python 6s at 4 threads takes 36.7 s/min here vs 16 there. Absolute numbers are not comparable across the two reports; the ratios are.

## Results (70 s loop, medians; s of compute per minute of audio, peak RSS)

| engine | 4s, 1 thr | 4s, 4 thr | 6s, 1 thr | 6s, 4 thr | RSS (70 s / 240 s song) | null vs Python (worst stem) |
|---|---|---|---|---|---|---|
| (a) demucs.cpp patched, Eigen GEMM, FTZ | 534 | 448 | 484 | 437 | 2.8 GB / **3.3 GB** | −42.8 dB |
| (a) demucs.cpp patched, OpenBLAS, FTZ | 479 | 430 | 462 | 455 | 2.1 GB / 2.7 GB | −42.8 dB |
| (b) ONNX Runtime CPU EP (arena off) | 97 | **34.5** | 78 | **34.0** | 2.6 GB / 3.09 GB | **−56.0 dB** |
| Python torch (reference) | 106 | 41.7 | 90 | 36.7 | 1.8 GB / 2.9 GB | — |

- **SDR on real7:** every engine equals Python to 0.01 dB on every stem.
- **Determinism:** a repeated run with the same settings is bit-identical for all engines. Across 1 vs 4 threads, ORT and OpenBLAS are also bit-identical; Eigen differs by ≤ −124 dB on most stems (−78 dB on the quiet 6s `other`).
- **Cancel latency:** within one segment for every engine. That is about 2–6 s for ORT and about 35–47 s for demucs.cpp.
- **demucs.cpp does not scale and BLAS does not save it:**
  - 1→4 threads gives only 1.0–1.2x.
  - GEMM is only 13–25% of profile samples; the rest is scalar transformer, conv2d and group-norm code.
  - OpenBLAS changes the time by +10% to −4%.
- **ORT matches torch on CPU:** it scales 2.3x over 1→4 threads.

### Apple Silicon estimate, 6s model (reasoning, not a measurement; derivation in RESULTS.md)

| engine | estimate | bar ≤ 60 s/min |
|---|---|---|
| (a) demucs.cpp + Accelerate + NEON | ~160–320 s/min (11–21 min per 4-min song) | **fails** (≥ 2.7x over even optimistically) |
| (b) ORT CPU EP | ~25–45 s/min | **passes** (1.3–2.4x margin, ±1.5x uncertainty) |
| (b) ORT CoreML EP | ~5–20 s/min if the graph is almost fully accepted | upside, unverified |

The reasoning behind the estimate:
- For (a), Accelerate/AMX can only remove the 13–25% GEMM share. The scalar layers scale with per-core speed alone.
- For (b), ORT's CPU EP is already at torch parity here, and M-series P-cores run this kind of workload at least as fast per core as this Xeon.

**CoreML caveats:**
- 98% of the nodes have a CoreML op builder. The exceptions are Gather ×26 and Pad ×3, which gives up to about 30 partitions.
- An fp16 proxy (torch autocast) keeps the loud stems at −54 to −64 dB, but the quiet 6s `other` and `piano` stems fall to −5 and −12 dB. So the ANE/fp16 path would fail the per-stem −40 dB null, and CoreML must be run as fp32 (MLProgram with fp32 compute on GPU/CPU) or justified separately.

### Against the bars

| Bar | (a) demucs.cpp | (b) ONNX Runtime |
|---|---|---|
| 6s ≤ 60 s/min on the Mac | No (est. 160–320) | Yes (est. 25–45 CPU EP) |
| Peak RSS ≤ 3 GB | OpenBLAS yes; Eigen no on a 4-min song | Yes at 70 s; 3086 MB on a 4-min song, 3% over 3000 MB / 0.5% over 3 GiB. The excess is the spike driver's whole-song buffers (~0.5 GB), which 5.1b removes. |
| Null ≤ −40 dB | Yes (−42.8 worst) | Yes (−56.0 worst) |

## Recommendation for 5.1b: **ONNX Runtime (candidate b)**

The 5.0 decision rule was to pick (a) if it meets the bars and (b) otherwise. (a) fails the speed bar by a wide margin, and the profile shows why: BLAS cannot fix scalar layers. Making it competitive would mean rewriting demucs.cpp's transformer and conv paths, which is effectively writing our own inference engine. (b) passes all three bars, with the RSS caveat that 5.1b design removes. It also nulls 13+ dB tighter than (a), is bit-identical across thread counts, and cancels in seconds instead of ~40 s.

What 5.1b should carry over:
- **Engine settings:**
  - ORT 1.30.0, pinned by URL + sha256.
  - CPU EP as the baseline on both platforms.
  - CoreML EP on macOS is opt-in, enabled only after it passes the null and speed checks on the user's Mac, in fp32.
- **Session options:**
  - Arena and memory-pattern off: ORT defaults peak at 5 GB.
  - `intra_op` = P-cores − 1, `inter_op` = 1.
  - Denormals-as-zero on.
- **Our own C++:**
  - Normalisation, Python-padded segmentation (shift 0), linear-ramp overlap-add and PFFFT STFT/iSTFT, all already proven to −57..−96 dB.
  - Stream segment outputs into the cache instead of holding whole-song buffers, which keeps RSS under 3 GB.
  - Cancel between segments, and consider `RunOptions::SetTerminate()` for sub-second cancel.
- **Model files:**
  - Export `.onnx` from the official checkpoints once, sha256-pinned.
  - fp32 is 174 MB (4s) / 115 MB (6s). These are derivative weights: never committed or bundled, and downloaded and converted on first use per the 5.0 report §3.
  - The export needs torch with the MHA fastpath disabled. Decide in 5.1b between a one-time `match/` venv step and a prebuilt-file route; the latter is not allowed under the licensing rules, so plan the venv step.
- **demucs.cpp** stays in `spikes/` as a reference only. If it is ever revived, fix the reviewer's FtzScope note first: unfilled `saved[]` entries can restore MXCSR=0 on a larger team.

### Still open (unchanged from 5.0, now gating 5.1b defaults)

- **Guitar-stem quality on real distorted guitar:** needs the user's own multitracks (docs/TEST_SOURCES.md). The 6s model stays opt-in until this is measured. **User decision needed** after that.
- **Mac verification:** CPU EP speed, CoreML EP coverage, fp32 null and the first-run compile time, all on the user's Apple Silicon Mac. That is the first task of 5.1b.

## Reviewer should-fix items not done (non-blocking, spike-only)

- **FtzScope `saved[]` sizing in the demucs.cpp patch:** not needed, since (a) is not chosen. Noted above.
- **V3 path of the patch:** only the shift is fixed there. The htdemucs path is the one measured.
- **Script nits:** the `eval.py` docstring name and the `run_cancel.sh` `exit=$?` string. The cancel byte counts were verified directly.
