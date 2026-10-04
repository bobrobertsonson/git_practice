# Phase 5: Play-along (stem separation in the plugin)

User request (2026-10-04): match a tone to a song, then play along with that song with the
guitars removed, through the matched tone.

## Concept
1. The user loads a song (their own file: mp3, wav, flac or m4a). Sawblade separates it
   **on the user's machine**, in the background, and caches the stems next to the match
   cache. Nothing is uploaded.
2. The same stems feed two features. The matcher uses the guitar stem as its reference
   (the spec 3.4 fizz fix already depends on this). Play-along uses the rest of the
   stems as the backing track.
3. Play-along transport:
   - play/pause, loop A–B (by bar if a tempo is detected, else by time) and a count-in;
   - the guitar stem can be muted, ducked (-12 dB "ghost guide") or soloed;
   - level controls for the backing vs the live rig;
   - later: slow-down without a pitch change (time-stretch) and transpose.
4. Output:
   - **Standalone app:** the backing is mixed into the output; this is the main use.
   - **Plugin in a DAW:** the backing player is off by default because DAW users have
     their own tracks. When enabled, it follows the host transport position.
5. "Match from this song": the matched tone is loaded, and the user plays over the
   backing straight away.

## Engine
- **Separation model:** htdemucs (Demucs v4).
  - The 4-stem model puts guitars in `other`, together with keys and synths. Removing
    `other` is fine for most metal and is the default.
  - The 6-stem model (`htdemucs_6s`) has a dedicated guitar stem. Use it when the user
    wants to keep keys and synths. Its guitar quality is lower, so it's an opt-in.
- **Inference in C++.** Candidate: a C++ port of Demucs v4 (e.g. demucs.cpp, Eigen, CPU).
  Fallback: ONNX Runtime with an exported model. The plugin must not depend on Python.
  The spike has to verify the licence (code and weights), speed and quality.
- **Threading:**
  - Separation runs on a background worker thread at low priority, with progress and
    cancel.
  - The audio thread only reads pre-decoded, pre-resampled stem buffers. They are swapped
    in lock-free (same SwapSlot pattern), with no I/O or allocation in `process()`.
  - Long songs are streamed from a memory-mapped cache file decoded off-thread into a
    ring buffer.
- **Latency:** the backing is delayed by the rig's reported latency, so the live guitar
  and the backing stay aligned at the output.
- **Cache:** keyed by the audio file's SHA-256 + model id. Stems are stored as FLAC
  (lossless, about half the size).

## Phases
- **5.0 spike (dsp-engineer):**
  - Build the C++ Demucs candidate standalone.
  - Separate the reference songs.
  - Compare with Python demucs: SDR on the guitar stem, or a null test between the two
    implementations' outputs.
  - Measure time on this box (and estimate it for an Apple Silicon Mac).
  - Report the licences.
  - No plugin code.
- **5.1:** a core `StemPlayer` (no allocation in `process()`, latency-compensated, loop,
  transport follow) and a separation job API in core. Tests come first.
- **5.2:** plugin integration: a play-along panel, the cache, and the matcher using the
  same stems.
- **5.3:** time-stretch and transpose (a separate spec; library choice and licence first).

## Rules
- The user's songs and their stems are never committed or uploaded.
- Personal use: the user brings their own files; Sawblade does not download songs.

## Lead decisions (2026-10-04, from the first offline play-along render)
- With the 4-stem model, the `other` stem is treated as **guitar** by default, so "mute guitar"
  removes it. The user can switch to "keep other" when a song has keys or synths. The StemSet
  loader needs a role-mapping option for this (5.2).
- When the user's DI is a recording of the same song, the backing needs a **start offset**
  to line up with it. The matcher already measures this offset, so expose it as a
  `StemPlayer` start offset and a `tonerender --backing-offset-ms` option (5.2). For live
  play-along there is no DI, and the offset is 0.
