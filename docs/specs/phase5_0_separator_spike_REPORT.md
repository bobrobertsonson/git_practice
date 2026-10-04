# Phase 5.0 separator spike: report (lead)

Spec: `docs/specs/phase5_0_separator_spike.md`. Raw data, commands and hashes:
`spikes/separator/RESULTS.md`. Build: `-DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON` (default OFF).
Branch `claude/sawblade-p5-separator-spike`, commits 9307c54 (spec), 607aab6, d8a2024, 0745e2b, ea88df8.

**Reviewer verdict: ACCEPT** (two rounds; no must-fix items). The reviewer:
- re-ran a C++ separation, which was bit-identical to the stored stems;
- re-ran `eval.py`, and all 112 table rows matched;
- checked the licence claims against the actual LICENSE and README files;
- confirmed the shift/padding explanation in demucs.cpp's source;
- checked that `--ftz` leaves the output unchanged (bit-identical, or −99 to −130 dB across builds and thread counts).

I fixed the duplicated-phrase nit in RESULTS.md myself.

## Bottom line

- **Quality:** demucs.cpp gives the same separation quality as Python Demucs. Per-stem SDR is within 0.3 dB. The null test reaches −62 to −72 dB (4s model) once Python uses the same settings.
- **Speed:** it is roughly **10x slower than PyTorch on CPU**, and it barely scales across cores through its library API. As it stands it is not good enough for 5.1.
- **Next step:** the 5.1 engine decision needs one short bake-off (below) before any plugin work.

## Environment

- **Machine:** Intel Xeon @ 2.10 GHz (AVX-512), 4 cores, 15 GB RAM, Linux, g++ 13.3.
- **Separation code:** demucs.cpp `f1206e9a`, built with the project's Eigen 3.4.0 and OpenMP.
- **Python reference:** demucs 4.0.1 with torch 2.5.1 (CPU), museval 0.4.1.

## Test material (all outside the repo)

- **Real:** MUSDB18-7 sample "Music Delta - 80s Rock", **6.8 s**, from the `sigsep-mus-eval` git repo. Zenodo and Hugging Face are blocked from this box. Ground truth here is drums, bass, other and vocals; guitar has no separate stem (it is part of `other`).
- **Synthetic:** a 36 s synthetic mix with exact stems. Its SDR numbers are weak evidence: the model didn't recognise the synthetic bass or guitar as bass or guitar.
- **Timing:** the real clip looped to 70 s.
- **Gap:** guitar-stem quality on real distorted guitar is **unmeasured**.

## Results

### Quality: SDR in dB on the real clip (museval BSSEval v4, median over frames)

| Stem | C++ 4s | Python 4s (shifts=0) | C++ 6s | Python 6s |
|---|---|---|---|---|
| drums | 15.07 | 15.03 | 14.72 | within 0.3 |
| bass | 13.09 | 13.01 | 11.44 | within 0.3 |
| other (6s: other+guitar+piano) | 9.35 | 9.32 | 9.14 | within 0.3 |
| vocals | 14.31 | 14.39 | 14.65 | within 0.3 |

Per-cell Python 6s values are in RESULTS.md.

### Null test: C++ vs Python, residual relative to the Python output

| Python reference | Residual |
|---|---|
| Spec settings (`shifts=0`) | −16 to −28 dB |
| Same random shift as C++ (4033 samples) | −30 to −39 dB |
| Same shift, and the last short chunk zero-padded as C++ does | 4s: −62 to −72 dB. 6s: −48 to −71 dB, except synthetic 6s `other` at −38 dB |

The cross-correlation lag between the outputs is 0 in every case. Two demucs.cpp behaviours explain the gap with the spec settings:

1. **Random time shift.** It always applies one time shift, from an unseeded `rand() % 22050`. That violates the determinism rule in CLAUDE.md if it is ever seeded differently.
2. **Padding of the last chunk.** It zero-pads the final chunk when that chunk is shorter than a 7.8 s segment. Python pads with the real neighbouring audio instead.

Float precision is not the cause: Python fp32 vs fp64 is −127 dB. The −38 dB synthetic `other` stem is likely the model being unsure on synthetic material (its SDR is 3.7 dB), which amplifies small differences. That is not proven.

### Speed: seconds of compute per minute of audio, 70 s input, median of 2–3 runs

| Engine / build | 4s, 1 thread | 4s, 4 threads | 4s, split into 4 workers | 6s, 1 thread | 6s, 4 threads | 6s, split into 4 workers |
|---|---|---|---|---|---|---|
| Python / torch CPU | 64 | **20** | n/a | 52 | **16** | n/a |
| C++, `-march=native` (AVX-512, not shippable) | 261 | 241 | 85 | 246 | 231 | 82 |
| C++, `-march=x86-64-v3` (AVX2/FMA, shippable) | 574* | 325* | 186* | 263 | 230 | 88 |
| C++, no `-march` | 978* | n/a | 305* | 359 | n/a | 114 |

\* These 4s cells are inflated by **denormals** (tiny floating-point values the CPU handles very slowly) in the 4s model's transformer layers.
- Turning on flush-to-zero / denormals-are-zero (`--ftz`) brings 4s close to 6s speed:
  - 6.8 s clip, v3 build, 1 thread: 102 s → 49 s.
  - Same clip, no-`-march` build: 165 s → 69 s.
- The output does not change.
- 6s is unaffected.
- `--ftz` was not re-timed on the 70 s loop.

"Split" is demucs.cpp's song-splitting mode, which cuts the song into 4 parts and runs them in parallel. It is a different algorithm: SDR is 0–1.6 dB lower and it needs about 6 GB of RAM.

### Memory and model load

| Item | Value |
|---|---|
| Peak RSS, Python | 1.7–2.2 GB |
| Peak RSS, C++ single mode | 2.1 GB at 1 thread, 2.9 GB at 4 threads |
| Peak RSS, C++ split mode | 5.4–6.4 GB |
| Model load | 0.2–0.4 s |
| Weights | 84 MB (4s), 55 MB (6s), stored as float16 |

### A 4-minute song on this box

| Engine | Time |
|---|---|
| Python, 4 threads | about 1.1–1.4 min |
| C++ AVX2 with FTZ, single mode, 4 threads | about 15 min (6s), 4s similar (estimated) |
| C++ AVX2, split mode | about 6 min (6s) |

### Apple Silicon estimate (not measured; no Mac available)

- **Per-core speed:** an M-series performance core running Eigen with NEON is roughly as fast per core as this 2.1 GHz Xeon running AVX2. NEON has 4×128-bit FMA pipes. Apple cores clock at about 3.2–4 GHz, but their vectors are narrower than AVX-512.
- **Denormals:** ARM cores handle denormals without Intel's microcode penalty, so the 4s denormal problem should be much smaller. FTZ should still be set (the FPCR.FZ bit).
- **C++ single mode:** about 150–250 s per minute of audio, i.e. **10–15 min per 4-minute song**. It does not scale with cores, for the same reason as here: most of the time is in scalar layers.
- **C++ split mode:** split over 8 performance cores, about 40–70 s per minute of audio, at 6+ GB of RAM.
- **PyTorch for comparison:** CPU about 10–20 s per minute of audio. With MPS or CoreML (Apple's GPU and ML runtimes), faster again.
- **Conclusion:** demucs.cpp as built here would not get a song separated "while the user tunes the tone" on a Mac either.

## Licences (recorded in docs/THIRD_PARTY.md)

| Item | Licence / terms | Source checked |
|---|---|---|
| demucs.cpp code `f1206e9a` | MIT, © 2023 Sevag H | its `LICENSE` |
| Demucs code (facebookresearch) | MIT, © Meta; README says the repo is no longer maintained | `LICENSE`, README |
| htdemucs weights (`955717e8-8726e21a.th`) | **No separate weights licence stated.** README: "Demucs is released under the MIT license". Applying MIT to the weights is an inference | README, LICENSE, docs/training.md, remote/*.yaml |
| htdemucs_6s weights (`5c90dfd2-34c22ccb.th`) | Same as above. The source of the guitar/piano training stems is not stated | same |
| Training data | MUSDB18-HQ (academic use only) + "an extra training dataset of 800 songs" | README, SigSep page |
| Test clip (MUSDB18-7 / MedleyDB) | CC BY-NC-SA, academic use only | SigSep `tracklist.csv` and page |

**Effect for Sawblade:** fine for this personal, non-commercial project.
- Weights and test audio are never committed or bundled.
- Users download the weights from Meta's official URL.
- Anything commercial or redistributed would need the weights re-cleared.

## Recommendation for 5.1

### 1. Engine: decide with a short bake-off, scoped as task 5.1a

Neither shipping the weights nor plugin integration should start before it.

**Candidates:**
- **(a) demucs.cpp, patched:**
  - FTZ/DAZ set on every inference thread;
  - a fixed shift of 0, for determinism;
  - Python-compatible context padding;
  - a cancel flag;
  - no stdout output;
  - the BLAS backend (OpenBLAS on Linux, Accelerate on macOS through `EIGEN_USE_BLAS`).
- **(b) ONNX Runtime:**
  - export the htdemucs core network, with STFT/iSTFT and segment overlap-add done in our own C++;
  - CPU execution provider, plus the CoreML execution provider on macOS.

**Bars, on the user's Mac:**
- **Speed:** ≤ 60 s of compute per minute of audio for the 6s model, i.e. a 4-minute song in ≤ 4 min.
- **Memory:** peak RSS ≤ 3 GB.
- **Accuracy:** null test ≤ −40 dB against Python with matched settings.
- **Deciding:** pick (a) if it meets the bars, because it has no runtime dependency and the code is small. Otherwise pick (b). Python/torch stays the reference.
- **What I expect:** (b) wins on speed. demucs.cpp spends its time in scalar layers that BLAS won't fix.

### 2. Model: htdemucs_6s as the working default, 4s as the fallback

Of everything in this recommendation, this is the decision most likely to change.

**Why 6s:**
- Its `guitar` stem is exactly what both features need:
  - play-along: the mix minus guitar, so keys survive;
  - matcher reference: the guitar stem without cymbals.
- In the C++ build it is 35% smaller (55 MB vs 84 MB) and has no denormal penalty.

**What would keep it opt-in instead:**
- 6s guitar quality on real distorted guitar is unmeasured.
- Demucs itself calls 6s experimental: its piano stem is weak, and it bleeds.

**Gate before choosing:** 5.1a must measure guitar-stem SDR on real distorted-guitar multitracks. The user supplies these, kept off-repo, per docs/TEST_SOURCES.md. 6s stays opt-in as the spec says until that is measured. **User decision needed** once the numbers exist.

### 3. Model files: shipping and download

- **Nothing in the installer.**
- **Download on first use of play-along:**
  - from `dl.fbaipublicfiles.com`;
  - pinned by sha256 (in RESULTS.md);
  - into the app's model cache, e.g. `~/Library/Application Support/Sawblade/models/`;
  - convert once into the engine's format (ggml or ONNX), and store the hash of the converted file.
- **No Python in the plugin:**
  - Write the converter in C++. The `.th` file is a zip of pickled tensors, so a minimal reader is needed.
  - Or, for this personal project, run a one-time `sawblade-models fetch` from the existing `match/` venv.
  - Either way the plugin only loads converted files.
- **Licence display:** the plugin's about box records the weights' licence status exactly as THIRD_PARTY.md states it.

### 4. Threading

- Separation never runs on the audio thread. It runs as a background job on a dedicated worker:
  - one job at a time, below normal priority;
  - the intra-op thread count is the number of performance cores minus one;
  - FTZ/DAZ is set on every thread that runs inference.
- Progress and cancellation:
  - progress is reported through an atomic;
  - cancellation is checked between segments.
- Stems reach the playback engine through the existing lock-free handoff used for model/IR swaps, and are freed off the audio thread. Play-along playback reads preloaded buffers only.
- No split mode: it costs SDR and uses 6 GB of RAM.

### 5. Cache

- **Key:** sha256 of the decoded 44.1 kHz stereo float audio + model id + weights hash + engine version.
- **Storage:** stems as FLAC (24-bit) in the app's cache dir, roughly 60–100 MB per 4-minute song with 6 stems. LRU cap, default 5 GB, user-clearable.
- **What is cached:** the song's separated stems, never committed or exported.
- **Presets:** reference the cache key and a song label only. No audio goes into presets.

### 6. Matcher and play-along signals

| Use | Signal |
|---|---|
| Matcher reference | the 6s `guitar` stem, or 4s `other` as the fallback |
| Play-along backing | `mix − guitar` (exact reconstruction of everything else), with `mix − other` as the 4s fallback |

## Follow-ups proposed (not done; out of spike scope)

- 5.1a bake-off, as above.
- Guitar-stem ground truth from the user's own multitracks.
- Re-time v3 with `--ftz` on the 70 s loop to get a clean headline number.
