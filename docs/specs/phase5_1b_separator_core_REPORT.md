# Phase 5.1b report: on-device stem separation in core + plugin (lead)

Spec: `docs/specs/phase5_1b_separator_core.md`. Branch `claude/sawblade-p5-1b-separator`.

**Status: accepted by the lead after reviewer ACCEPT.** The reviewer accepted the round-1 C++ change, the lead sent one revision round on the memory bar, and the reviewer accepted that too. The Python tool was accepted separately on its first round.

Nothing in the repo is audio, weights or ONNX. Tests generate their own audio and their own synthetic ONNX model. Root `.gitignore` now ignores `*.onnx`, `*.ort` and `*.tgz`.

## Commits

| Commit | What |
|---|---|
| `e1ead6b` | Core + CLI + tests: decoding, ModelStore, Separator, StemCache, the ORT CMake setup, `tonerender --separate`, `sawblade-stems` |
| `810f53c` | `match/`: `sawblade-models fetch/status` |
| `900460c` | `match/`: reviewer should-fix items |
| `82e66d7` | Plugin: LOAD SONG separates audio files on a background thread |
| `d164e2f` | Docs: THIRD_PARTY, PLUGIN, PRESET_SCHEMA |
| `4b3d438` | Revision 1: streaming output (memory), plugin host-friendliness, reviewer should-fix items |

## Lead decisions (refining what the spec left open)

- **Model store contract.** This is shared by the C++ `ModelStore` and the Python `sawblade-models` tool.
  - **Directory:** `SAWBLADE_MODELS_DIR` if set. Otherwise `~/Library/Application Support/Sawblade/models/` on macOS, and `$XDG_DATA_HOME/sawblade/models/` or `~/.local/share/sawblade/models/` on Linux.
  - **Files:** `<id>-core-opset17.onnx` plus a `.sha256` sidecar, for the ids `htdemucs_6s` (the default) and `htdemucs`.
  - **Acceptance:** a model is accepted when its sha256 equals the pinned value or its sidecar. The tool writes the sidecar only after the ORT-vs-torch check passes, because another platform may export different bytes.
  - **Missing model:** the error prints `match/.venv/bin/sawblade-models fetch --model <id>`.
- **Stem mapping.**
  - **6-stem:** `piano` is summed into `other`.
  - **4-stem:** uses 5.2's `OtherRole` mapping, so `other` becomes the guitar by default.
- **Stem cache.**
  - **Format:** 32-bit float WAV. dr_libs has no FLAC encoder and none is pinned, which the spec allowed.
  - **Key:** sha256(file) + model id. Entries live in `~/.local/share/sawblade/stems/` (or the macOS equivalent), and `SAWBLADE_STEMS_DIR` overrides the location.
  - **Writes:** the stems are written into a staging dir and atomically renamed. A cancelled or failed job leaves no entry.
  - **Loading:** a hit is loaded with the existing `loadStemDirectory`.
- **Formats.**
  - **m4a:** unsupported in core, with a clear error. The plugin decodes it through JUCE's `AudioFormatManager`, which uses CoreAudio on macOS.
  - **mp3:** dr_mp3 from the already pinned dr_libs.
- **`SAWBLADE_WITH_SEPARATOR`.**
  - **Default:** ON when the plugin is ON, OFF otherwise. A default build downloads nothing; the reviewer verified this.
  - **Tests:** the separator unit tests use a synthetic model, so they run whenever the option is ON. The real-model test is gated by an env var.
- **Memory bar (revision 1).** 3 GB means 3000 MB in decimal, as 5.1a measured it.
  - **Round 1:** the peak was 2986 MiB, which is 3131 MB, and it grew by about 160 MB per minute of song.
  - **Fix:** I asked for streaming output. A sliding window of one segment per stem is flushed straight into the cache's WAV writers.
- **Plugin.**
  - **Threads:** separation uses max(1, cores/2) threads, on a thread at lowered priority. The CLI uses cores−1.
  - **State restore:** a restore with a cache miss does not start a multi-minute separation on its own. It shows "Song not separated yet - LOAD SONG to separate it". A user LOAD SONG, or the model toggle, separates immediately.
  - **Editor close:** closing the editor does not cancel the job, because the job belongs to the processor. Destroying the processor cancels the job and joins its thread.

## What was built

**Core** (namespace `sawblade`)
- `sawblade_core` has no ORT dependency. `ModelStore`, `StemCache` and decoding (`readAudioFile` now also reads `.mp3`) live there, so their tests run in every build.
- `Separator` and `separateSong` are in `sawblade_separator`, which is built only with `SAWBLADE_WITH_SEPARATOR`. ORT stays behind a pimpl.
- The `Separator` carries over the spike's normalisation, Python-padded segmentation (shift 0), linear-ramp overlap-add and PFFFT STFT/iSTFT.
- Input is resampled to 44.1 kHz with the existing offline resampler. Mono input becomes stereo.
- Output goes through a `StemSink`. Each region is flushed once no later segment can change it. `CollectingSink` keeps the whole-result API for tests.
- Decode and resample intermediates are freed before inference.
- ORT 1.30.0 CPU EP is set up as in the 5.1a report: arena and mem-pattern off, inter_op 1, denormals as zero. `intra_op` is configurable.
- **Progress:** `(fraction, eta)`, reported at 0 and then once per segment.
- **Cancel:** a `CancelToken` is checked between segments, and `RunOptions::SetTerminate()` stops a running segment.

**ORT dependency**
- Pinned release tarballs, by URL + sha256:
  - Linux x64: the spike's pin.
  - macOS arm64: `6ebb5062…7012`.
- No universal2 tarball exists for 1.30.0, so a configure for an x86_64 or universal Mac target stops with a FATAL_ERROR telling you to configure with `-DSAWBLADE_WITH_SEPARATOR=OFF`.
- The library, its `LICENSE` and its `ThirdPartyNotices.txt` are copied into the plugin bundle (`$ORIGIN` / `@loader_path/../Frameworks`).

**CLI**
- `tonerender --separate <song> --stems-out <dir> [--model] [--threads]` and `sawblade-stems <song> <dir> [same flags]`.
- Both use the cache and print progress.
- When the model is missing they exit non-zero and print the fetch command.

**Python** (`match/sawblade_match/models/`)
- `sawblade-models fetch [--model htdemucs_6s|htdemucs|all]` and `sawblade-models status`.
- Checkpoints come from dl.fbaipublicfiles.com with the pinned sha256. They are downloaded to a `.partial` file and renamed into place.
- The spike exporter is ported verbatim. The tool runs the ONNX checker, then gates the export on ORT vs torch on a seeded synthetic segment at ≤ −60 dB.
- The sidecar is written only after that check passes. A failed `--force` re-export keeps the previously verified file pair.
- Packaging: a new `models` extra pins onnx 1.23.1 and onnxruntime 1.30.0.

**Plugin**
- **LOAD SONG** takes a file or a folder, from the picker or by drag-and-drop.
- **Separation runs** on its own thread, never the audio or message thread. The panel shows a progress bar with an ETA and a CANCEL button. A cache hit is instant.
- **When separation finishes,** the stems go to the 5.2 loader unchanged.
- **Model toggle:** 6-STEM / 4-STEM. It is saved in `playAlong.separationModel` only when it isn't the default, and `songFile` is saved only when set, so untouched sessions keep byte-identical state.
- **Missing model:** the panel shows the fetch command and nothing crashes.
- **Audio thread:** nothing new runs on it. The allocation and lock tests still pass.

**Docs:** THIRD_PARTY.md now records:
- ORT (MIT, with the caveat that ThirdPartyNotices must ship with any redistributed bundle);
- dr_mp3;
- the weights' licence status as the spike found it (none stated; trained partly on MUSDB18, which is academic-use only);
- the new checkpoint and ONNX locations.

PLUGIN.md and PRESET_SCHEMA.md are also updated.

## Timing and memory (this box: Xeon Cascade Lake, 4 vCPU, 15 GB; `sawblade-stems --threads 4`, fresh process, empty cache, synthetic 44.1 kHz stereo)

These are the reviewer's independent measurements after revision 1. The implementer's numbers agree within a few percent.

| Model | Song | Wall | s per min of audio | Peak RSS |
|---|---|---|---|---|
| htdemucs_6s | 4 min | 104 s | 26.0 | **2611 MB** (2490 MiB) |
| htdemucs (4s) | 4 min | 111 s | 27.8 | **2708 MB** (2583 MiB) |
| htdemucs_6s | 8 min | 196 s | 24.5 | **2707 MB** (2581 MiB) |

- Before revision 1, 6s on a 4-minute song peaked at 3131 MB. The spike driver peaked at 3086 MB.
- From 4 to 8 minutes the peak rises by only about 100 MB, which is the larger input buffer, so memory no longer grows with song length otherwise.
- Wall time varies by about 25% between runs on this shared box (the implementer measured 6s at 4 min as 134–140 s). Either way it is well under the 60 s/min bar.
- Speed is on par with the 5.1a spike measurement of 34 s/min.

## Tests

| Build | Result |
|---|---|
| Release, `-DSAWBLADE_BUILD_PLUGIN=ON`, gcc | ctest **259/259 pass**, 1 skipped (the real-model test, gated by an env var), 0 warnings |
| clang/clang++, plugin ON, `-Werror` | 0 warnings, 259/259 pass, the same 1 skip |
| Default (plugin OFF, separator OFF) | 186/186 pass, no ORT downloaded |
| ASan + UBSan Debug, separator ON | 16 separator/separation tests pass, no reports. `detect_leaks=0` is needed because of a 622-byte leak inside libonnxruntime.so with no Sawblade frames |
| pluginval v1.0.4, strictness 10, VST3, xvfb-run | **SUCCESS** |
| `match/` (`tests/test_models.py`) | 22 pass |

- **Synthetic ONNX model.** The tests build it in C++ with the real graph's I/O, for both 4 and 6 sources. CI needs no Python and no weights.
- **Null against an unsegmented reference.**
  - The reference is closed-form; the null is below −90 dB.
  - Cases: shorter than one segment, exactly one segment, and 3 segments plus a ragged tail, all for 4s and 6s.
  - An STFT-path case gives about −100 dB or better away from the segment edges.
  - All cases now run through the streaming path.
- **Cancel.** Covered before start, from the progress callback, and from another thread mid-run (returns in under 5 s, and the Separator stays usable).
- **Progress.** Monotonic, once per segment, with an ETA.
- **Cache.** Miss, hit without the model, a separate entry per model, and a cancel that leaves no staging directory.
- **Other core tests:**
  - **Decoding:** wav, flac and mp3. The mp3 fixture is generated with `ffmpeg` at test time, and that test is SKIPped where ffmpeg is absent.
  - **Determinism:** 1 vs 3 threads.
  - **ModelStore:** sidecar, mismatch and missing cases.
  - **CLI:** covered.
- **Plugin tests:**
  - Separate, cache and load, plus the 4s other→guitar mapping.
  - A missing model gives the fetch command.
  - Cancel keeps the previous song, and a folder load replaces a running job.
  - Destroying the processor mid-job joins cleanly.
  - State keys are written only when non-default.
  - A restore with a cache miss gives NotSeparated and creates nothing; a following user load separates.

**Real-model test** (`SAWBLADE_SEPARATOR_REAL_TEST=1`; the clip and spike reference commands are in the test's header). Each null is against the spike's `separator_onnx` output:

| Clip / model | drums | bass | vocals | other | guitar |
|---|---|---|---|---|---|
| 10 s, 6s | −326 dB | −335 | −297 | −331 (incl. piano) | −286 |
| 10 s, 4s | −314 | −335 | −287 | −336 | — |
| 30 s (4 segments + ragged tail), 6s | −327 | −340 | −306 | −335 | −301 |

The reviewer also compared the stems the CLI writes through the incremental WAV path, on the 30 s clip: max |diff| = 0.0 against the spike, so the output is bit-identical. The bar is −40 dB. This proves parity with the spike driver; parity with demucs/torch rests on the 5.1a report (−56 dB worst stem).

**Model fetch on this box.** Both exported ONNX files match the pinned sha256 (the export is reproducible). The ORT-vs-torch check gave −75 dB or better. A full `fetch --model all` took 58 s. The venv took about 10 minutes and is 5.6 GB, because torch comes with CUDA wheels from PyPI and download.pytorch.org is blocked here.

## Reviewer verdicts

- **`match/` tool (`810f53c`):** **ACCEPT**, no must-fix. The should-fix items were done in `900460c`.
- **Full change, round 1 (`e1ead6b`..`d164e2f`):** **ACCEPT**, no must-fix. The lead still sent a revision round for the memory bar and the should-fix items.
- **Revision 1 (`4b3d438`):** **ACCEPT**, no must-fix.
  - The reviewer read the streaming flush logic in full (ragged middle segment, last segment, exact multiple, normalisation per region): correct.
  - The new atomic job id and the cache-only restore path: no races, and the lock order is consistent.

## Open items and questions

1. **Mac verification (needs your Mac).** These code paths were reviewed by reading only:
   - the arm64 ORT pin;
   - copying the library into `Contents/Frameworks` and the `@rpath`/`@loader_path` setup;
   - the FPCR flush-to-zero path;
   - lowering the separation thread's priority (`QOS_CLASS_UTILITY`);
   - m4a through CoreAudio.

   Speed on Apple Silicon is still an estimate (25–45 s/min, from 5.1a). The bundles must be re-signed after the post-build copy of ORT. CoreML EP stays opt-in and untried.
2. **Guitar-stem quality on real distorted guitar** is still unmeasured; it needs your multitracks (docs/TEST_SOURCES.md). 6-stem is the default as the spec says. Whether to keep it the default is your call once that is measured.
3. **Intel Macs** are not supported by the separator, because ORT 1.30.0 has no universal2 tarball. Such builds must use `-DSAWBLADE_WITH_SEPARATOR=OFF`.
4. **ORT inside a host that already loaded another ORT** with the same soname could reuse that copy. This is a known plugin-hosting risk and was not tested.
5. **Non-blocking cosmetic issue:** after a restore with a cache miss, the panel briefly shows "Loading stems..." before "Song not separated yet". Left as is.
6. **mp3 decoder delay and padding** are not trimmed (documented in `wav_io.h`). This is a small constant offset that the 5.2 offset control absorbs.
7. **Redistribution:** a redistributed plugin must ship ORT's ThirdPartyNotices, and it is already copied into the bundle. The weights' licence (MUSDB18, academic) still restricts any commercial use. That fits the personal, non-commercial scope.
8. **Proposed extras, not done:**
   - a "copy fetch command" click in the panel;
   - a macOS/clang CI job;
   - a smaller, CPU-only torch install for `sawblade-models`, once download.pytorch.org is reachable.
