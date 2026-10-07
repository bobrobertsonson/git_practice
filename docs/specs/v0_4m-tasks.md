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
- `feel = W_TIGHT * tight + W_FIZZ * fizz + W_POLISH * polish`, initial weights 0.25 / 0.25 / 0.125 (lowered from 0.5 / 0.5 / 0.25 after the known-answer CI failure; each
  normalised sub-term is Huber-softened, delta 1 normaliser; tuned in D.1; documented in
  the `loss.py` docstring and README). Added to `total`. `LossResult` gains `feel` (weighted) and `feelTerms` (every
  sub-term, raw and normalised, the note counts and noteSet). `result.json` reports them for best, alts and the starter.
- Without a matched pair (soft targets): same features from the reference's guitar-dominant excerpt, onsets detected on the
  reference itself for tightness, all comparisons as W1 of the per-note / per-frame distributions, every weight x 0.5.
- Stage 1's cross-spectral pair x pair blend screen stays LTAS-only (no render). The feel term enters everywhere the full
  loss is evaluated (re-score, cab sweep, stage 2's gain and final linear blocks, finals). Stage 2's first linear block is
  LTAS-only (`loss.without_feel`), so the spectral fit leads and feel refines it (known-answer CI fix).
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
- Scale (user catalog: 3,899 WAVs; with a wide TONE3000 pull assume 5-6k): **6,000 IRs screened in <= 90 s on 4 cores in
  quick mode** (from a warm index), and the 2000-IR CI test <= 60 s. |H| on the screening grid is cached per IR (sha-keyed
  `.npy` sidecar, or one stacked `.npy` + offsets, beside the index), so warm runs do no IR FFTs. The first (cold) indexing
  run may be slow but prints progress (files done / total, ETA) at least every 2 s and is resumable (an interrupted scan
  keeps what it indexed).
- Cap: above `--ir-screen-max` (default 6000) the pool is prefiltered, never randomly sampled: first by tags matching the
  current candidates' cab hints where present, then by broad spectral family (k-means, k = 32, seeded, on the cached
  1/3-octave |H| shape) keeping the IRs nearest each centroid in proportion to cluster size. `result.json -> irPool` says
  `prefiltered: true`, the method and counts.
- Speed test detail: 2000 IRs <= 60 s on 4 cores. Unit-test with 2000 synthetic short IRs (in memory, no files) and assert < 60 s on
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

## Task B4: pre-EQ before the drive (phase spec "Task B4")

- Per path (path A for single topologies; both paths for blends, searched independently on each path's own winner),
  in `space.py` as discrete pre-EQ settings written to the path's `preEq` (existing schema bands: `highPass` 12 dB/oct,
  `peak`, `lowShelf`). Defaults = off, so every existing preset and test is unchanged when the grid picks "off".
- Pruned grid (coordinate descent, <= 12 renders per top candidate on the screen excerpt, full loss incl. feel):
  1. HPF {off, 80, 110, 150 Hz} with the others off (4); 2. at the best HPF, mid peak {+3, +6 dB} at {700, 900 Hz}, Q 0.8
  (4); 3. at the best so far, low shelf -3 dB at 200 Hz (1); 4. up to 3 joint neighbours of the best (one step in two
  dimensions). Keep a setting only if it beats the previous best by >= 0.02. Full renders/stage 2 only for the winner.
  Runs on the top 2 candidates per topology after stage 1's re-score (before stage 2, since it changes the NAM input).
- Guitar-difference widening, from the DI's LTAS (active segments): `diTilt` = least-squares slope in dB/octave over the
  1/3-octave bands 100 Hz-3 kHz; `diLowExcess` = mean level 80-200 Hz minus mean level 200-800 Hz (dB). If `diTilt` <
  -4.5 dB/oct (dark), add a +9 dB mid option; if `diTilt` > -1.5 (bright), add HPF 180 Hz; if `diLowExcess` > +3 dB
  (bassy), add a -6 dB shelf option. The thresholds are provisional and get checked against the fixture DI and the user's
  Bloodbath DIs (the run prints both numbers). `result.json -> preEq: {diTilt, diLowExcess, widened: [...], grid: [...
  each tried setting with its loss], chosen, gainVsOff}`.
- `--ablate preeq` (pre-EQ stays off). Export notes (Task E): add an informational line "pre-EQ (before the amp): trained
  into the model" listing the bands, with `inModel: true`, position "inside NAM" — not a stage to add on hardware.
- Tests: the grid recovers a hidden HPF 110 Hz + mid +6 dB pre-EQ on a fixture chain (exact grid point); grid size <= 12
  (plus widening options) and deterministic; `--ablate preeq` leaves `preEq` empty; widening fires on a synthetic dark DI
  and not on the fixture DI; export notes list the pre-EQ as in-model.

## Task F: blend reference and per-path check in the validation script (user correction, 2026-10-07)

Why: the album guitar is a blend of two same-take amp tracks per side (HM2 path + body amp). Matching HM2 (18) and UBR
(19) separately never tests the product's main case. Single-amp runs stay as per-path diagnostics.

### F.1 `sawblade_match.matcher.refsum` (match-engineer)
`python -m sawblade_match.matcher.refsum --a <hm2.wav> --b <body.wav> --out <blend.wav> [--blend-db A_DB,B_DB] [--json r.json]`
- Reads both tracks (any channel count; reduce to mono as the matcher does for `--matched mono`), requires equal sample
  rates (exit 2 otherwise), truncates to the shorter length, sums `a*10^(A_DB/20) + b*10^(B_DB/20)` (default 0,0 =
  unity faders) and writes **float32** WAV (no clipping, no normalisation; peak reported).
- Alignment check, report only (never shifts, never flips: the mic/amp phase is part of the record): lag of max |xcorr|
  of the two tracks within +-50 ms (on the loudest 30 s), its sign (polarity), and the normalised correlation.
  |lag| > 2 ms or negative peak -> a `WARNING:` line; the sum is still written.
- JSON: `{gainsDb, lagMs, polarity, corr, peakDb, lufsA, lufsB, refRatioDb}` where
  **refRatioDb = LUFS(a*gA) - LUFS(b*gB)** (BS.1770, `loudness.integrated_lufs`). Note: at unity faders the reference
  ratio is the two tracks' loudness difference, not 0 dB.
- Output paths are under the run's `$OUT`; nothing is written into the repo.

### F.2 `sawblade_match.matcher.pathcheck` (match-engineer)
`python -m sawblade_match.matcher.pathcheck --result <run>/result.json --di <di.wav> --ref-a <hm2.wav> --ref-b <body.wav>
 [--ref-blend <blend.wav>] [--blend-db A_DB,B_DB] [--json out.json]`
- Takes the winning preset from result.json and renders the DI over the full length three ways with the core engine:
  full preset; path B `enabled: false` (A alone); path A `enabled: false` (B alone). Everything else unchanged (cab mode,
  post EQ, bus comp, alignment). Gate as in the preset.
- Uses the run's stored DI offset / alignment so the renders line up with the references as in the run (read from
  result.json; if absent, the same offset search the matcher uses).
- Per render, the same metrics the run reports for `after`: A-weighted LTAS error dB (`loss.ltas_error`, level offset
  removed) and the feel terms (tight / fizz / polish values and the raw feel measures). Pairs: A alone vs ref-a, B alone
  vs ref-b, full vs ref-blend (if given). Also the **swapped** pairing (A vs ref-b, B vs ref-a) for both LTAS errors, so
  a role swap is visible; primary is A<->HM2 (18), B<->body (19).
- **Blend ratio**: chosenRatioDb = LUFS(A alone) - LUFS(B alone) on the renders, vs refRatioDb (as F.1, with --blend-db);
  report both and the difference.
- Single-path result (one path disabled or level <= -60 dB in the winner) -> report `"singlePath": true`, which path,
  and the full-vs-blend metrics only; not an error.
- Prints a short human block and writes the JSON. Exit 0 unless inputs are unreadable.
- Held-out transfer: the same tool pointed at the L_blend result with the R DI and R refs scores the L preset on R
  without re-fitting (no extra code; the script calls it).

### F.3 Script changes (`scripts/run_v04m_validation.sh`)
- New option `--blend-db HM2_DB,BODY_DB` (default `0,0`), validated as two numbers; passed to refsum and pathcheck.
- After the IR scan, build `$OUT/refs/L_blend.wav` (and `R_blend.wav` when R_OK) with refsum (+ `.json`), resumable
  (skip if both exist unless --force).
- `--quick-only`: runs `L_hm2_quick`, `L_ubr_quick`, then **`L_blend_quick`** (DI 17 vs L_blend).
- Full: adds **`L_blend` (thorough)** first in step 3; step 5 adds **`R_blend` (thorough)** when R_OK. Ablations stay on
  HM2 quick (unchanged).
- New step "per-path check": pathcheck on every finished blend run (`L_blend_quick` / `L_blend`, `R_blend`) against
  its side's 18/19 (21/22) and blend ref; plus the held-out transfer `L_blend` preset on R (when both exist), output
  `$OUT/<run>/pathcheck.json` and `$OUT/L_blend_on_R.pathcheck.json`. A pathcheck failure is reported, non-fatal.
- Summary printer and listen list: blend runs first, then single-amp runs; summary also prints each pathcheck's
  per-path LTAS errors, swapped errors, chosen vs reference ratio, singlePath.
- Dry run (`--dry-run`) prints the refsum / blend / pathcheck commands; the existing dry-run test covers them.
- Summary printer (`sawblade_match.matcher.validation_summary`): for every run, the matched gate and bus comp in full, one
  line each, read from the winning preset (`best.preset`, not just `gateFinal`): gate `enabled, mode, thresholdDb,
  hysteresisDb, attackMs, holdMs, releaseMs, rangeDb, ratio (expander only), keyHighPassHz, releaseCurve` plus the DI noise
  floor the threshold was set from (`diNoiseFloorDb`, when recorded); busComp `enabled, thresholdDb, ratio, kneeDb, attackMs,
  releaseMs`, or `busComp: off` when absent. Unit-tested on a fixture result.json (`match/tests/test_validation_summary.py`).

### Tests
- refsum: two synthetic tracks (a, b = a delayed 0 samples, different spectra) -> output == a*gA + b*gB to 1e-6, float32,
  lagMs 0, refRatioDb matches LUFS difference to 0.05 dB; a 5 ms delayed b -> WARNING and lagMs ~5; a polarity-flipped b
  -> polarity -1; mismatched sample rates -> exit 2; --blend-db applied.
- pathcheck: a two-path fixture preset (blend) rendered from a fixture DI; refs = the same preset rendered with each path
  alone -> per-path A-weighted errors < 0.1 dB, swapped errors larger, chosen ratio == ref ratio within 0.1 dB; a
  single-path fixture -> singlePath true. Synthetic audio only, generated in the test, nothing committed.
- Script: the dry-run test asserts the blend refs, `L_blend_quick` under --quick-only, `L_blend`/`R_blend` in full,
  pathcheck lines and `--blend-db` parsing (bad value -> exit 2).

### F.4 Dynamics sweep (lead decision 2026-10-07; match-engineer)
`python -m sawblade_match.matcher.dynsweep --result <run>/result.json --di <di.wav> [--json out.json]`
- Renders the winning preset over the full DI at input offsets -12, -6, 0, +6 dB (DI scaled before the chain), twice:
  dynamics as matched (the record set, see Task G) and with gate and bus comp both disabled. 8 renders.
- Per render: integrated LUFS, median 400 ms crest factor (feel.py's crest windows), inter-note floor (feel ``floor_db``
  at the DI gaps). Per adjacent step: slope = dLUFS_out / dB_in. Table matched vs bypassed, plus max |slope difference|.
- When Task G lands, a third set: live dynamics.
- Script: run on every finished run (blend and single-amp), `$OUT/<run>/dynsweep.json`; non-fatal. The summary prints
  the table, matched-vs-bypassed first.
- Test: on a fixture preset with gate + bus comp the bypassed slopes are ~monotone and smooth; a fixture with a
  high-threshold gate shows a slope knee at the low step; a 4:1 comp flattens the matched slope vs bypassed.

## Task G: live dynamics policy (lead decision 2026-10-07)

Principle: the matcher may copy the record's gating and bus compression to score the match; a rig played live must not
inherit mix processing by default.

### G.1 Preset (dsp-engineer; preset version 4, PRESET_SCHEMA updated)
- The existing `gate` and `busComp` objects are the **record set** (fitted by the matcher; unchanged meaning, so the matcher,
  the exporter and every golden keep working).
- New optional `liveDynamics: { "gate": {...}, "busComp": {...} }` (same object schemas) and
  `dynamicsMode: "live" | "record"`. **File-format default when absent: "record"** (old files and goldens render
  bit-identically). The writer emits version 4 when either key is present (v3 otherwise is fine too; follow the schema's
  existing versioning convention: bump to 4, read 1-4).
- New optional `origin: "match" | "user" | "official"` (v4; absent = "user"). The matcher writes "match".
- **Derivation rule** (core, single source of truth; applied by the parser when `liveDynamics` is absent):
  - `origin` != "match" (hand-made, official, old files): `liveDynamics` = the stored `gate` and `busComp` unchanged, so
    deliberate settings play as set.
  - `origin` == "match":
  - gate: `enabled` = record gate enabled; mode expander, ratio 2, rangeDb -24, keyHighPassHz 80,
    thresholdMode `floorRelative`, floorOffsetDb +8, holdMs = max(record holdMs, 40), releaseMs = max(record releaseMs,
    120), attack/hysteresis/releaseCurve from the record gate. Record gate absent/disabled -> live gate disabled.
  - busComp: disabled.
- `dynamicsMode` selects which set the engine runs.
- **NAM export follows the active set** (lead decision): a "live" rig trains with the live busComp (off for a match), a
  "record" rig trains the record busComp if it passes the existing export rules; the gate stays excluded as before.
  Core exposes one resolver (active gate/busComp of a preset) used by the render path, the exporter and the plugin.
  Export notes state which set was used; the plugin export panel shows "dynamics: live / record" next to the model type.

### G.2 Gate floor follower (dsp-engineer; core gate)
- New GateParams: `thresholdMode` absolute (default, bit-identical) | floorRelative, `floorOffsetDb` (default 8).
- floorRelative: threshold = floorEstimate + floorOffsetDb, re-evaluated per sample (hysteresis applies below it).
- floorEstimate: RT-safe minimum statistics on the (key-HPF'd) key: 50 ms RMS frames -> running minimum over a 3 s window
  as a fixed ring of 100 ms sub-window minima (std::array, no allocation), counted in samples so it is independent of
  block size. **Only frames with RMS < floorEstimate + 20 dB feed the sub-minima** (playing never feeds the floor).
  **Upward leak:** when no frame has qualified for 10 s, the estimate rises +1 dB/s (so a genuinely higher floor after a
  gain/interface change is learned). Clamped to [-96, -40] dBFS; seed -70 dBFS.
- Rendering is deterministic and block-size independent (existing determinism tests extended to a floorRelative gate).

### G.3 Plugin (dsp-engineer; plugin/ authorized for this task only)
- Presets loaded/created in the plugin with no `dynamicsMode` get `"live"` (the plugin state is the preset, so it is then
  written). The matcher's emitted presets carry `dynamicsMode: "live"` (G.4).
- A plain **RECORD DYNAMICS** toggle (on = record) in Settings or the existing rig/CAB page, not a new main-UI element.
  Report flags it "needs a UI home" (the user designs the main UI).
- The switch is atomic: the whole dynamics set (gate + busComp params) is built off the audio thread and handed over
  lock-free as one object, applied between blocks. Gate/comp UI controls (if any) edit the active set.

### G.4 Matcher (match-engineer, after F and G.1)
- Scoring, listening and all match quality numbers render with `dynamicsMode: "record"` (unchanged numbers).
- Emitted presets (result.json `preset`, alts, export input) carry `dynamicsMode: "live"` and no explicit `liveDynamics`
  (core derives it).
- Emitted presets carry `origin: "match"`. The exporter (plan/notes) uses the core active-set resolver; notes state
  "dynamics: live" or "record".
- `--listen` writes a third file `render_live.wav` (live set, same section, same loudness match gain as render.wav).
  The validation script lists it in the listen pairs. dynsweep gains the live set.

### Tests (G)
- Floor follower: converges to a known noise floor within 3.5 s; 30 s of continuous riffing at -12 dBFS over a -75 dB
  floor keeps the estimate within 3 dB of -75; a floor step -75 -> -60 dB is learned within ~25 s; follows a -12 dB input change (threshold moves -12 +-1
  dB) with no re-match; zero allocations in process(); block sizes 1/64/512/odd give identical output (float tolerance
  per existing determinism tests).
- Derivation, both paths: a v4 `origin: "match"` preset with a gate (hold 10, release 20) and a bus comp -> live gate
  expander/2/-24/80 Hz, hold 40, release 120, floorRelative +8, live busComp off; the same preset with origin absent /
  "user" (and an old v3 file) -> liveDynamics == stored gate/busComp. Old v3 preset with a gate (hold 10, release 20) and a bus comp -> live gate expander/2/-24/80 Hz,
  hold 40, release 120, floorRelative +8; live busComp off; renders in record mode bit-identical to v3.
- Toggle: switching sets atomically (no block runs half old / half new; test via the handover object); state round-trip.
- Schema: v4 round-trip; a v3 reader rejects v4 (existing strictness test pattern).
