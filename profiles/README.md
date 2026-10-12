# Tone profiles

A profile = guardrail **rules** + tolerances + provenance for a style. The matcher's target is always the
reference's own LTAS; profiles only decide which rules `sawblade-tonecheck` (and the matcher's report) judge.

Schema `sawblade.profile` v1: `{schema, version, id, name?, status, provenance, analysis, rules[], metrics, notes}`;
`analysis`, `rules` (`id`, `expr` like `dipMid >= saw - 2.5`, `toleranceDb`, `why`) and `metrics` are exactly those of
`docs/tone_targets.json`, so any profile can be passed to `sawblade-tonecheck --targets`.

* `swedish_death_hm2.json`: docs/tone_targets.json v2 (calibrated on the Gatecreeper original). `docs/tone_targets.json`
  stays as the reference copy; a test keeps the rule sets identical.
* **Reference-derived default** (`sawblade-match --profile derived`, the default): the rule skeleton of the base profile
  with every rule the reference's isolated guitars fail (or pass marginally) loosened so the reference passes with its
  tolerance as margin; rules the reference passes are untouched. Written to `<out>/profile.derived.json`.
* `chainsaw_grind.json`, `chainsaw_hardcore.json`, `chainsaw_crust.json`: stubs (`"calibrated": false`) in the chainsaw
  family: `swedish_death_hm2` expressions with tolerances loosened x1.5 / x1.5 / x2.0, nothing presented as measured.
  Their rule `why` texts are inherited unchanged from `swedish_death_hm2`. The HM-2/chainsaw is a gear class (`distortion`), selectable by the search under ANY profile; profiles never choose gear.
* `us_death.json`, `cavernous_death.json`, `melodic_death.json`: **uncalibrated hypotheses** (rule skeleton of
  `swedish_death_hm2` with adjusted offsets, status says so) awaiting lead approval and calibration on the user's
  references (`sawblade-calibrate` / reference-derived profile). They are guardrails only; the reference LTAS is the target. Their offsets are lead-unapproved priors: they are never selected by default and
  stay that way until they are calibrated from user references.
* Later seeds (deathcore, djent, `modern_metalcore`, `doom_fuzz`, `thrash`, `black_metal`) are added when the user supplies
  references for calibration (numbers need lead approval).

* `calibrated`: `true` (hand-calibrated), `false` (stub/hypothesis) or `"reference-derived"` (`--profile derived`; the rule
  offsets are measured from the reference, `metrics` and rule `why` texts are inherited from the base profile).
