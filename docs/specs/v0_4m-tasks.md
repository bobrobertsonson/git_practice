# v0.4M — task specs (lead decisions for `v0_4m-matcher_feel.md`)

Phase spec: `docs/specs/v0_4m-matcher_feel.md`. This file pins the definitions, files and tolerances the phase spec leaves
open. Owner: match-engineer. Reviewer on every task. Where this file and the phase spec disagree, the phase spec wins and the
implementer raises it under "Decisions / questions for lead".

## Boundaries (parallel work)

- v0.4A edits `match/sawblade_match/calibrate/` (pedal-fit, metrics). **Do not edit `calibrate/` or `tonecheck/`** in this
  phase. Reuse their functions (`tonecheck.analysis.detect_onsets`, `gap_regions`, ...) by import only. If a tonecheck
  change seems needed, copy the logic into a matcher module and note it.
- Never touch `plugin/`. C++ (`core/`, `bindings/`) only through dsp-engineer and only if a renderer hook is unavoidable
  (none is expected: the gate, `pedal.ts`, `eq` blocks and post-EQ already exist in the preset schema).
- The local container cannot install Python packages (pypi.org blocked). **CI's `python` job is the test of record.** Write
  the tests so that they pass first time; the lead pushes and reads CI.

## Task A: feel features and loss terms

New module `match/sawblade_match/matcher/feel.py` (pure numpy/scipy, 48 kHz, no tonecheck edits). `loss.py` gains a
`feel` term built from it. All features are computed on the same excerpt and active mask as the existing loss.

### A.1 Low-end tightness (60-250 Hz)
- Band: 4th-order Butterworth band-pass 60-250 Hz (sos, zero-phase not needed). Envelope: RMS over 5 ms frames, hop
  2.5 ms, in dB.
- Notes: DI onsets (`detect_onsets`, the excerpt timeline). Note window = onset to the next onset, capped at 400 ms. A note
  counts if its window is >= 80 ms and its low-band peak (first 30 ms) is within 30 dB of the loudest note's peak.
- Per note: `t12` = ms from the peak until the envelope first falls 12 dB below the peak (censored at the window end);
  `sustainDb` = mean power in [peak + 40 ms, peak + 120 ms] (clipped to the window) re peak power, in dB.
- **Chugs** = notes whose *DI* low-band `t12` <= 150 ms (palm-muted). Use chugs if >= 3, else all counting notes; record
  which (`noteSet: "chugs"|"all"`) and the counts. Fewer than 3 notes: term dropped and recorded (as `decay` does today).
- Matched pair: per-note differences on the same onsets. `tight = median(asym(t12_out - t12_ref)) / 20 ms +
  median(asym(sustain_out - sustain_ref)) / 3 dB`, where `asym(d) = d` for d > 0 (floppier than the reference) and
  `0.5 * |d|` for d <= 0.

### A.2 Fizz (per frame, compared as distributions)
- Frames: 2048-pt Hann, hop 1024, frames >= 80 % active.
- Per frame: `hfRatioDb` = 10 log10(E[5-12 kHz] / E[1-4 kHz]); `hfFlat` = spectral flatness 5-10 kHz; `hfMod` = coefficient
  of variation of the 5-12 kHz Hilbert envelope (after a 1 kHz low-pass on the envelope) within the frame.
- Distance per feature = 1-D Wasserstein W1 estimated as the mean |quantile difference| at q = 0.05, 0.10, ..., 0.95.
- `fizz = W1(hfRatioDb) / 1.5 dB + W1(hfFlat) / 0.03 + W1(hfMod) / 0.1`.
- Off when the reference HF is not usable: the full-mix fallback basis (`hf_limit_hz` set) or a matched channel limited by
  `stft_fmax < 12 kHz`. **Check and report** which basis the user's NTM run takes (`--matched mono` on a clean amp track, no
  stems): the fizz term must be ON there. If the current code treats that file as a mix, add the smallest fix (e.g. a
  `--ref-clean` flag or automatic detection) and document it.

### A.3 Polish
- `flux`: per-frame mean |delta dB| between consecutive 1024-pt log-magnitude frames (hop 512), bins 300 Hz-8 kHz, active
  frames; W1 vs the reference / 0.5 dB.
- `crest`: per 400 ms active window (hop 200 ms), peak/RMS in dB; W1 / 1.5 dB.
- `floor`: inter-note level = output power in the DI gap regions (`gap_regions` on the DI) re the output's active power (dB),
  same for the reference. One-sided: `max(0, d) / 6 dB + 0.25 * max(0, -d) / 6 dB`, d = out - ref. Dropped (recorded) if the
  excerpt has < 100 ms of gaps.
- `polish = flux + crest + floor`.

### A.4 Loss integration
- `feel = W_TIGHT * tight + W_FIZZ * fizz + W_POLISH * polish`, initial weights 0.5 / 0.5 / 0.25 (tuned in D.1; documented in
  the `loss.py` docstring and README). Added to `total`. `LossResult` gains `feel` (weighted) and `feelTerms` (every
  sub-term, raw and normalised, the note counts and noteSet). `result.json` reports them for best, alts and the starter.
- Without a matched pair (soft targets): same features from the reference's guitar-dominant excerpt, onsets detected on the
  reference itself for tightness, all comparisons as W1 of the per-note / per-frame distributions, every weight x 0.5.
- Stage 1's cross-spectral pair x pair blend screen stays LTAS-only (no render). The feel term enters everywhere the full
  loss is evaluated (re-score, cab sweep, stage 2, finals).
- Cost: report the time per `evaluate()` call on a 6 s excerpt before and after. The target is at most 1.5x.
- Unit tests (synthetic, no captures): slow vs fast decaying low chug, so the floppy one scores worse and `t12` is within
  5 ms of the analytic value; white vs shaped (harmonic, low-passed) HF content, so the fizz features order correctly and an
  identical pair scores 0; a gated vs ungated gap, so `floor` orders correctly; gain invariance (+6 dB = same feel); the
  term is dropped with < 3 notes.

## Task B: search space

- **Tight boost** (single-path candidates): for every single combo re-scored in stage 1 (and refined in stage 2), also
  score a variant with the modeled `pedal.ts` (slot `boost`, `modelVersion` 1) directly in front of the amp (after any
  pedal). Its params enter stage 2's gain group: `drive` 0-3, `level` 6-10, `tone` 3-8 (preset defaults 1/8/5). The boost
  variant competes as its own candidate. Occam: the boost costs like one extra block (prefer no boost within 0.1 dB).
  `result.json` records `tightBoost: {tried, won, params}`. No capture of a TS is required (any `drive` capture already
  competes through `single2`).
- **Gate matched to the reference:** after stage 2, on the final chain, sweep threshold = DI floor + {4, 8, 12, 16, 20} dB
  x release {80, 150, 250} ms. Pick the one minimising `floor` (A.3) subject to the LTAS error rising <= 0.05 dB and the
  tightness term not getting worse. Without a matched pair, use the reference's own inter-note floor (soft). Record the
  sweep. Also **diagnose the Bloodbath `gap_noise` −10.8 dB** on a synthetic case: is the gate not closing (threshold),
  not reached (gap detection), or is it the post-gate chain (NAM noise, cab tail)? Fix what is the matcher's to fix and
  report the rest.
- **Post-cab filters** (post EQ, after the shared cab): `post.hp` 60-140 Hz and `post.lp2` 6-11 kHz, slope 12 or 24 dB/oct
  (24 = two cascaded biquads, Butterworth Qs), slope a discrete param per filter (continuous [0, 1] thresholded at 0.5).
  Neutral defaults (hp 60 Hz/12 dB, existing `post.lp` stays). They do not count toward the EQ regulariser. Both must
  survive the preset round trip (`eq` band types `highPass`/`lowPass` already exist).
- **Cab/IR breadth:** for the top 3 candidates per topology (after stage 2), sweep **every** cab in the pool (all IRs and
  mic positions) with the full loss, re-using the memoised NAM output (IR swap = linear stage only). Then re-run the
  last linear CMA-ES block on the winner if the cab changed. Record the sweep (n cabs, best/worst, whether the cab changed).
- `--quick` must stay <= 5 min on 4 cores for the real pool: give the Mac command that measures it; locally show the added
  cost per candidate in timings.

## Task C: honest level for listening

- BS.1770-4 integrated loudness (K-weighting, 400 ms blocks, 75 % overlap, -70 LUFS absolute + -10 LU relative gate) in
  `match/sawblade_match/matcher/loudness.py` (numpy/scipy). Unit tests, within 0.1 LU: a mono 997 Hz sine at -20 dBFS
  peak = -23.0 LUFS; EBU Tech 3341 case 1 (stereo 1 kHz sine, -23 dBFS peak each channel) = -23.0 LUFS; a gated case (tone
  plus -80 dBFS silence segments) is unaffected by the silence. No new dependency.
- With `--listen` and a reference: write `listen/ref.wav` and `listen/render.wav` (float WAV, 48 kHz, mono for a matched mono
  pair), 30 s from the same section (the guitar-dominant 30 s, or the whole thing if shorter), time-aligned with the found
  offset, the render loudness-matched to the reference (integrated LUFS over that same section). Also `listen/before.wav`
  (the starter) when it exists. No peak normalisation, float so nothing clips; report true-peak (4x oversampled) of each.
- Every other listening file the matcher writes is loudness-matched to the reference the same way (no peak normalisation).
- `result.json -> listening`: `{section: [s0, s1], lufsRef, lufsRenderRaw, gainDb, lufsBefore?, gainBeforeDb?, offsetMs,
  truePeakDb: {...}}`; the run log prints "render was X dB louder/quieter than the reference before matching".

## Task D.1: synthetic known answer (CI)

- Extend `known_answer.py` and the tests: hidden chain = fixture captures + `pedal.ts` boost + post HPF/LPF (24 dB/oct) +
  a gate with threshold above the DI floor; the DI fixture gets inter-note gaps with a realistic noise floor (synthetic
  noise added in the test, -70 dBFS).
- Pass (all on the whole section): A-weighted LTAS error <= 0.5 dB (unchanged); tightness median |delta t12| <= 10 ms and
  |delta sustain| <= 1.5 dB; fizz W1(hfRatioDb) <= 1.0 dB, W1(hfFlat) <= 0.02; flux W1 <= 0.3 dB; floor |d| <= 3 dB.
  Report the same numbers with the feel term off (weights 0) to show what it buys; that run does not have to pass.
- Weights: run the synthetic case (and the 6b fixture) for at least 3 weight sets, pick the set, document why. Real tuning
  comes from the user's run (D.2/D.3).
- Existing tests incl. the 6b known-answer fixtures stay within their tolerances.

## Report

`docs/specs/v0_4m-matcher_feel_REPORT.md`: reviewer verdicts per task, feature definitions + weights, synthetic results,
timings, the Mac commands (lead writes those), user results when back.

## Task B2: all four suspects (phase spec "Task B2"), plus the UBR findings

Second data point (user, Bloodbath L DI -> real Überschall amp track, `--quick`): A-weighted 3.45 dB, chose Hex Drive ->
Sovtek MIG 50 -> Marshall 1960BV "V30 3 SM58 6"; `gap_noise` -11.0, `fizz_texture` fail. The HM2 run chose the **same IR**.
The pool has two Überschall captures (tone 57492, 79751) and neither won.

### B2.1 Two-IR blend (dsp-engineer: renderer hook, then match-engineer)
- Core: `cab.mode: "irMix"` already exists (`h = (1-mix) hA + mix hB`, no alignment). Add optional `offsetSamplesB` (int,
  -256..256, default 0: hB shifted right when positive, zero-padded, applied before the sum) and `invertB` (bool, default
  false), both only valid in `irMix` mode (strict keys), round-tripped, in `PRESET_SCHEMA.md`. Latency unchanged (the shift
  is part of the IR). Tests: offset 0 / no invert is bit-identical to today; a shifted copy of the same IR at offset -k,
  invert true, mix 0.5 cancels to <-100 dB; round trip. `core/` + `bindings/` + `tests/` only; no `plugin/` edits (the plugin
  just carries the keys through the core parser).
- Matcher: after the B cab sweep, for the winner try IR pairs from the top-6 single IRs: offset = the lag maximising
  |xcorr| of the two IRs' first 5 ms (|lag| <= 256), invertB = sign of that peak, mix searched in stage 2's last linear
  block (0.2-0.8). Competes with the single IR; Occam: a pair must win by >= 0.05. Live-compatible (one combined IR).

### B2.2 Boost and filters as mandatory candidates
The Task B boost variant and post-cab HP/LP are always in the search (quick and thorough). Report their contribution.

### B2.3 Studio processing in the reference
- Detection (after stage 2, on the best candidate without a bus comp): `compressed` if the reference's median 400 ms crest
  is >= 1.5 dB below the candidate's, or its short-term loudness range (3 s windows, 10-95 % quantile spread) is >= 2 LU
  narrower; `eqd` if the candidate's post-EQ gains sit at >= 90 % of their range, or the LTAS residual's best 3rd-order
  polynomial in log-frequency explains >= 60 % of the residual's variance with the residual RMS >= 1.0 dB.
  `result.json -> studio: {compressed, eqd, evidence}` is always written.
- When detected: a "studio" stage refines the winner with the bus comp (threshold -30..-6 dB re the pre-headroom level,
  ratio 1.5-4, knee 6, attack 1-30 ms, release 30-150 ms, makeup = level-neutral) and/or the post-EQ gain range widened to
  +-9 dB. Kept only if the total loss falls by >= 0.05. Release stays <= 150 ms (trainable by the export rules). The bus
  comp sits after the cab, so the no-cab export drops it and the existing export plan already says so (`export/plan.py`).
  Record `studio.busCompUsed` and append "bus comp added by the matcher (studio processing); dropped from no-cab exports"
  to the preset `notes`. No new preset keys.

### B2.4 Ablations and diagnostics (needed for "each suspect's contribution")
- `--ablate LIST` (comma list of `feel, boost, filters, irsweep, irblend, studio`) turns a suspect off (`irsweep` off = the
  pre-v0.4M cab sweep). `result.json -> ablate` echoes it. The user runs on/off pairs on the Mac.
- `--trace-tones ID[,ID...]` (TONE3000 tone ids): for each, `result.json -> trace[id]`: downloaded? models, gear class,
  pre-screen rank / score / survived, best pair rank and loss, best candidate loss with it as the amp (render it once on the
  excerpt with the winner's pedal + cab and stage 2's linear block if it was not refined), and why it lost (loss breakdown
  vs the winner). Answers "why did the Sovtek beat the two Überschalls".
- `result.json -> cabSweep` lists every IR's loss (Task B) so "the same IR twice" can be checked.

### D.1 additions
- Synthetic: hidden chain also with an irMix pair (offset, invert) and a fast bus comp. Recovery tolerances as D.1; the
  studio detector must fire on it and must not fire on the plain hidden chain (no comp, no EQ).
- Report each suspect's contribution on the synthetic case: the full run vs each `--ablate` item off (A-weighted error and
  each feel term).

## Task B3: IR library (phase spec "Task B3")

- `match/sawblade_match/matcher/irlib.py` (new). `--ir-dir DIR` (repeatable) plus `~/.config/sawblade/ir_dirs.json`
  (`{"dirs": [...]}`; `sawblade-match --ir-dirs-add DIR` / `--ir-dirs-list` maintain it). Recursive `.wav` scan; index at
  `~/.cache/sawblade/ir_index.json` keyed by (path, size, mtime) so an unchanged file is not re-hashed; dedupe by content
  sha256 (first path wins, the others listed as aliases). Tags from folder/file names (case-insensitive token match:
  cab/speaker e.g. V30, G12T75, Greenback, 1960, Mesa, OS/standard; mic e.g. SM57, MD421, R121, 414, SM58; position
  e.g. cap, edge, cone, off-axis, distance in inches). Never fatal per file:
  - Paths with spaces/unicode, nested pack folders, symlink loops (resolve, visit each real dir once).
  - Formats: `.wav`, `.aif`/`.aiff`, `.flac` (soundfile); everything else is counted by extension as `not-audio`, not an
    error. Unreadable/corrupt files are rejected with the reason.
  - Multi-channel: left channel used (same as the core), recorded as `channels: N, used: left`.
  - Length: IRs > 2 s are **kept** and truncated to 2 s like the core, recorded `truncated: true` with the original length;
    < 2 ms or all-zero/silent rejected (`too-short`, `silent`).
  - Duplicates: exact (content sha256) and **near** duplicates, e.g. the same IR at 44.1/48/96 kHz: resample to 48 kHz,
    truncate at 2 s, L2-normalise, align by the peak, and treat as duplicates when the waveform correlation is >= 0.999 over
    the first 50 ms. Keep the 48 kHz (else the highest-rate) copy and list the rest as aliases with the reason.
- One-time index / sanity command (for the user's Mac, run before matching):
  `python -m sawblade_match.matcher.irlib --scan DIR [--scan DIR ...] [--json OUT]` prints files seen, audio files, accepted,
  exact duplicates, near duplicates, rejected by reason (with up to 20 example paths each), truncated count, rate/channel
  histograms, and tag coverage (% with cab/speaker, mic, position tags; top 15 values each). Exit 0 unless no IR is
  accepted. Same code path as `--ir-dir` (the index is shared).
- Tests: a tmp tree with spaces and nesting, a stereo IR, a 3 s IR, a silent file, a `.txt`, a corrupt `.wav`, and the same
  IR at 44.1 and 48 kHz → the counts above are exact.
- Preset reference: `{"file": <absolute path>, "sha256": ..., "source": {"provider": "local", "id": <sha256[:16]>,
  "title": <file stem>, "license": "user-owned"}}`. Check that the core parser accepts `provider: "local"` (the cache
  fallback is tone3000-only, so a moved file fails cleanly). If it rejects it, stop and tell the lead (that would be a
  dsp-engineer hook). Never upload or commit local IRs. `best.preset.json` (portable) keeps the file stem + hash.
- `sawblade-t3k pull --gear ir`: `--max-models-per-tone` defaults to all for IR tones (pedal/amp keep 3); `--ir-search
  QUERY` (repeatable) adds IR tones from search results, under the same quality filter and licence rules. `t3k/` edits only
  in the pull/search CLI path.
- Analytic screen (`irscreen.py`): per candidate, the pre-cab output on the excerpt is rendered once (cab disabled, post EQ
  neutral). Each IR's magnitude response is precomputed once per run at the Welch resolution (rfft of the IR, normalised
  as the core does: L2 = 1, truncated at 2 s, resampled to 48 kHz) and cached in the index (`.npy` sidecar keyed by sha).
  Predicted band PSD = Welch PSD(pre-cab) x |H|^2 band-integrated, giving the LTAS error (A-weighted) plus the fizz
  sub-terms that are spectral (hfRatio; hfFlat from the predicted per-frame spectra using the frame PSDs x |H|^2).
  The top 24 IRs per candidate get full renders and the full loss; the top 6 feed the B2.1 pair search. Validation test:
  the analytic predicted LTAS error is within 0.3 dB of the full-render error for the fixture IRs, and the rank
  correlation is >= 0.9 over a synthetic 200-IR set (random 2nd-order-filtered fixture IRs).
- Speed: 2000 IRs <= 60 s on 4 cores. Unit-test with 2000 synthetic short IRs (in memory, no files) and assert < 60 s on
  the CI runner. The index build (hashing) is measured separately and reported.
- `result.json -> irPool: {local, tone3000, total, skipped, screenSeconds}` and the winning IR's source (local path or
  tone id).

## Task E: NAM export notes (phase spec "Task E")

- `match/sawblade_match/export/notes.py` (new) builds `exportNotes` from the resolved preset + the export plan:
  `{"stages": [{"stage": "gate"|"preampEq"|"cab"|"postEq"|"busComp"|"output", "position": "before NAM"|"after NAM",
  "inModel": false, "settings": {...}, "hardware": "<one-line instruction>"}], "loaderOrder": "<one line>"}` in signal
  order. Only stages that are enabled and not in the trained model are listed (no-cab: cab and everything after it;
  with-cab: whatever the plan dropped or bypassed). Settings use hardware units: comp threshold dB (re the chain's
  pre-headroom level, also given re 0 dBFS out), ratio, attack/release ms, knee dB, make-up dB; gate threshold dB, attack/
  hold/release ms, range dB, "keyed on the DI = put it first"; cab IR file/title/source/mic tags; EQ bands (type, Hz, dB, Q;
  HP/LP slope in dB/oct); output gain dB.
- Writes `<name>.export_notes.txt` (human, the same content) next to the `.nam` and adds `exportNotes` to the export
  JSON/report. Documented in the export section of `match/README.md`. `export/` only, no plugin work.
- Tests: a preset with gate + fast bus comp + post EQ exported no-cab lists gate (before), cab, post EQ, bus comp (after),
  in that order, with the exact numbers; a with-cab export of a preset with nothing dropped lists only the gate; a preset
  with nothing outside the model writes "nothing to add".
