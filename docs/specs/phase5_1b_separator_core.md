# Phase 5.1b: on-device stem separation in core + plugin ("load any mp3")

The engine is decided by the 5.1a bake-off: **ONNX Runtime, CPU EP**
(docs/specs/phase5_1a_separator_engine_REPORT.md, spikes/separator/). The model is
**htdemucs_6s** by default, so its `guitar` stem feeds both the play-along mute and the matcher
reference. **htdemucs (4s)** is the fallback, with other→guitar role mapping (5.2).

## Core (dsp-engineer)
1. **`Separator` class** (namespace sawblade), off the audio thread only:
   - It takes decoded stereo float audio at 44.1 kHz (resample with the existing offline
     resampler) and produces a `StemSet`.
   - It carries over the spike's segment/overlap-add and STFT/iSTFT, with no whole-song
     buffers beyond input + output, and peak RSS ≤ 3 GB on a 4-minute song.
   - Thread count is configurable. It reports progress (fraction, ETA) through a callback and
     supports cooperative cancel (latency ≤ one segment).
2. **Audio decoding:** mp3/flac/wav/m4a.
   - mp3 through dr_mp3 from the already pinned dr_libs (MIT-0/public domain).
   - m4a: use the platform decoder in the plugin (JUCE AudioFormatManager on macOS), or say
     it's unsupported in core.
3. **Model files:**
   - Never committed.
   - `ModelStore` resolves the cache dir (~/Library/Application Support/Sawblade/models/,
     ~/.local/share/sawblade/models/ on Linux), checks sha256, and reports missing models
     with the exact fetch command.
   - Add `match/` tooling `sawblade-models fetch` (Python, using the existing venv) to download
     the official checkpoints from dl.fbaipublicfiles.com (pinned sha256, as in RESULTS.md),
     export the ONNX (port the spike's exporter), and write the ONNX sha256.
4. **Stem cache:**
   - `StemCache` is keyed by sha256(audio file) + model id. Stems are stored as FLAC
     (dr_flac can read; for writing, use 32-bit float WAV if a pinned FLAC encoder isn't
     already available, and say so). Reuse on a second load.
5. **ONNX Runtime dependency:**
   - Pinned release download (sha256), as in the spike, for Linux x64 and macOS arm64
     (universal if available).
   - CMake option `SAWBLADE_WITH_SEPARATOR` defaults ON for the plugin build and OFF for core
     tests unless the model is present.
   - Record the licence (MIT) in THIRD_PARTY.md. Record the weights' licence status as the
     spike found it.
6. **CLI:** `tonerender --separate <song> --stems-out <dir>` and `sawblade-stems <song> <dir>`
   (a small C++ tool).

## Plugin
1. In the play-along panel, LOAD SONG accepts an audio file as well as a stems folder.
   - A file is separated on a background job: a progress bar, cancel, and a cache hit is
     instant.
   - When done, load the StemSet exactly as 5.2 does.
2. A settings toggle "Separation model: 6-stem (guitar) / 4-stem (fallback)".
3. Missing model: a clear message with the fetch command, and no crash.

## Acceptance
- **Tests, with a tiny synthetic ONNX model** (a generated identity/linear graph with the same
  I/O shapes) so CI needs no weights:
  - segmenting/overlap-add reconstruction (a null test against an unsegmented reference);
  - cancel;
  - progress;
  - the cache;
  - decoding of mp3/wav/flac fixtures (generated in tests).
- **Real-model test:** gated by an env var. When the model is fetched it separates a 10 s
  clip, and the null vs the spike's output is ≤ −40 dB.
- Plugin tests pass, plus pluginval at level 10, plus the clang -Werror build.
- **Report:** timing on this box for a 4-minute song (6s and 4s, 4 threads) and RSS.
