# Phase 5.0 spike: on-device stem separation (htdemucs in C++)

## Why
- Phase 5 adds **play-along**: the user matches a tone to a song, then plays along with the
  song with its guitars removed.
- The separated guitar stem also becomes the matcher's reference. The full-mix side channel
  carried cymbals and made matches fizzy (phase 3.4).
- The plugin must not depend on Python. Before 5.1 commits to an engine we need numbers:
  does a C++ Demucs v4 port match Python demucs, how fast is it, how much memory does it
  take, and what are the licences of the code and the weights.

This is a **spike**. No plugin code, no `core/` changes, nothing on the audio thread. The
deliverable is a standalone build plus measured numbers.

## Model
- `htdemucs` (Demucs v4, 4 stems: drums, bass, other, vocals). Guitars land in `other`,
  together with keys/synths.
- `htdemucs_6s` (6 stems: drums, bass, other, vocals, guitar, piano). A dedicated guitar
  stem; opt-in in the product.

## Engine under test
- Candidate: **demucs.cpp** (github.com/sevagh/demucs.cpp, Eigen, CPU), pinned to commit
  `f1206e9adeea103aef4a636b9e62297cf1f8e34e` (`main` on 2026-10-04). Record the licence from
  its `LICENSE` file, not from memory.
- Fallback (ONNX Runtime + exported model) is **not built** in this spike. The report only
  says whether it is needed, based on the candidate's results.

## Deliverables (dsp-engineer)

### D1. Build (`spikes/separator/`)
1. New CMake option `SAWBLADE_BUILD_SEPARATOR_SPIKE` (default **OFF**) in the top-level
   `CMakeLists.txt`, adding `spikes/separator/` only when ON. With the option OFF nothing new
   is fetched or built, and the normal configure/build/ctest is unchanged.
2. Fetch demucs.cpp with FetchContent at the pinned commit; do not init its submodules if you
   can avoid it.
   - Eigen: reuse the project's pinned Eigen 3.4.0 if demucs.cpp builds with it. If it needs
     its own Eigen, fetch that pinned too and say why.
   - Third-party sources build without our `-Werror` set; headers as `SYSTEM`.
   - Use whatever threading the library offers (e.g. OpenMP / its `demucs_mt` path). If you
     enable OpenMP, record that.
3. Our own thin driver `separator_spike` (links `sawblade_warnings`, so zero warnings):
   - `separator_spike --model <ggml.bin> --in <mix.wav> --out-dir <dir> [--threads N]`
   - It reads a 44.1 kHz stereo WAV with the project's dr_wav (resample or reject other rates;
     reject is fine for the spike), runs separation through the **library API** (not by
     shelling out to demucs.cpp's own CLI), and writes one float32 WAV per stem named by
     stem (`drums.wav`, `bass.wav`, `other.wav`, `vocals.wav`, plus `guitar.wav`,
     `piano.wav` for 6s).
   - It prints model load time, separation wall time, audio duration, and the real-time
     factor. Peak RSS is measured externally (`/usr/bin/time -v`) or via `getrusage`.
   - The point of the driver is to prove the API we would embed in 5.1: note in RESULTS.md
     what the API looks like (inputs/outputs, threading knobs, progress callback, whether it
     can be cancelled, memory ownership).
4. A `spikes/separator/README.md` with the exact build, fetch and run commands.

### D2. Weights (never committed)
- `spikes/separator/scripts/fetch_weights.sh`: downloads the official PyTorch checkpoints
  from `dl.fbaipublicfiles.com` (htdemucs and htdemucs_6s), verifies their sha256, and
  converts them to demucs.cpp's ggml format with demucs.cpp's own conversion script (in the
  Python venv, see D3). Output goes to a cache dir **outside the repo**
  (default `~/.cache/sawblade/separator/`, overridable by an env var).
- Hugging Face is blocked on this box; do not rely on prebuilt ggml files from there.
- Record each file's URL, size and sha256 (checkpoint and converted ggml) in RESULTS.md.

### D3. Python reference (venv outside the repo)
- Create a venv outside the repo (e.g. `~/.venvs/sawblade-demucs`), install `demucs==4.0.1`
  with the CPU torch that pip can reach (pypi works here; download.pytorch.org does not).
  Record the exact versions installed.
- Run Python demucs with settings that match demucs.cpp's inference as closely as possible:
  same model, `shifts=0` (no random shift), same segment length and overlap as demucs.cpp
  uses (read demucs.cpp's source and state the values), float32 output. If an exact match is
  impossible, say what differs.
- If Python demucs cannot be installed, report the C++ numbers alone and say why.

### D4. Test material (never committed)
- Find a mixture with ground-truth stems that this box's network allows. Try, in order:
  MUSDB18 7-second sample clips (e.g. the `musdb` package's sample download), other
  freely licensed multitracks reachable over git/pypi/github, then Cambridge-MT.
- Prefer material with **distorted guitar** in it (that is the product's use case). Record
  the source, licence/terms and how to fetch it in RESULTS.md and the README.
- If nothing with ground truth can be fetched, build a mixture from freely licensed stems or
  synthetic material (e.g. a NAM-free synthetic distorted-guitar riff from the project's own
  fixtures/tools plus synthetic drums/bass) and say so plainly; synthetic results are weaker
  evidence and the report must flag that.
- Duration: at least 30 s of mixture if available (more is better for timing); timing is also
  measured on a ≥60 s input (looping the mixture is fine for timing only).
- Scripts in `spikes/separator/scripts/` fetch/build the material into a dir outside the repo.

### D5. Quality evaluation (`spikes/separator/scripts/eval.py`, runs in the venv)
1. Per-stem SDR against ground truth for: C++ 4s, Python 4s, C++ 6s, Python 6s.
   - Use museval's BSSEval v4 SDR (median over 1 s frames) if `museval` installs; else the
     global SDR `10·log10(Σs² / Σ(s−ŝ)²)` over the whole track, both channels. State which.
   - For 6s also report the guitar stem, and for 4s report `other` vs a ground-truth "other"
     built as `mix − drums − bass − vocals` (i.e. everything else) as the dataset defines it.
     Say which ground-truth stems the dataset provides.
2. **Null test** C++ vs Python, per stem and model:
   - residual level `20·log10(rms(cpp − py) / rms(py))` in dB, and max abs difference;
   - align lengths first (trim to the shorter; report any offset found by cross-correlation,
     expected 0).
   - Expected: a near-null (below about −40 dB). If not, investigate the cause (segment /
     overlap / padding / normalisation differences) and report it; don't just record it.
3. Output a markdown table that is pasted into RESULTS.md.

### D6. Speed and memory
- On this box (record CPU model and core count from `lscpu`/`nproc`, RAM):
  wall time per minute of audio and peak RSS for C++ 4s and 6s at 1 thread and at all cores,
  and for Python 4s and 6s at default torch threads. Run each at least twice; report the
  median. Include model load time separately.
- Release build (`-O3`, and note whether `-march=native` is used; a shippable build cannot
  assume `-march=native`, so if you use it, also give one portable-flags number).

### D7. Licences
- Add a "Separator spike (`SAWBLADE_BUILD_SEPARATOR_SPIKE`)" section to `docs/THIRD_PARTY.md`:
  demucs.cpp (code, pin, licence from its LICENSE file), any extra Eigen, Demucs code
  (facebookresearch/demucs), and **weights for htdemucs and htdemucs_6s** separately: where
  they come from, what licence the repo states for them (quote the README/LICENSE wording and
  the file it came from), what they were trained on (MUSDB18-HQ is research/non-commercial;
  check what the Demucs paper/README say about extra training data), and the resulting
  constraint for Sawblade. Also the test material's terms. Do not guess: if a licence is not
  stated, write "not stated" and where you looked.

### D8. `.gitignore`
- Ignore audio, stems and weights under `spikes/separator/` (`*.wav`, `*.bin`, `*.th`,
  `out/`, etc.) so nothing can be committed by accident.

### Output: `spikes/separator/RESULTS.md`
Raw results: environment, versions, file hashes, commands, the SDR / null / speed / memory
tables, API notes, problems hit. The lead writes the final report from it.

## Acceptance
1. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build`
   passes exactly as before, and the configure log shows no demucs.cpp fetch.
2. `-DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON` builds `separator_spike` with zero warnings on our
   target.
3. `separator_spike` runs end to end on the test mixture with both 4s and 6s weights and
   writes all stems.
4. RESULTS.md contains every table in D5/D6 with real measured numbers (or an explicit,
   justified "blocked" for a cell), the API notes, and the hashes.
5. `docs/THIRD_PARTY.md` has the D7 section.
6. `git status` / the diff contain no audio, stems, checkpoints or ggml weights.
7. Commits on `claude/sawblade-p5-separator-spike`, with the lead's trailer.

## Out of scope
- Plugin integration, background threading in the plugin, caching, UI, the ONNX fallback
  build, matcher changes. Propose them in RESULTS.md ("Notes for 5.1"); the lead decides.
