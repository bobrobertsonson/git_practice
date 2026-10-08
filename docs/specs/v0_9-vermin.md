# v0.9 — VERMIN: a rat-style distortion pedal (modelled, not captured)

Source: user decision 2026-10-08 ("lets do the rat first. can call it Vermin"). Names: `docs/NAMES.md`.
Why: the modelled pedals cover the Swedish saw (THE SAW MILL), fuzz (TAR PIT) and a TS-style boost; the op-amp
hard-clipping distortion used across crust, grind, doom, hardcore and thrash is missing. CLAUDE.md requires any heavy
tone, not only HM-2 tones.

Owners: dsp-engineer (core, tests, render script), reviewer on every task. match-engineer only for Task D.
Branch: `claude/sawblade-v0_9-vermin`, from the working branch. **Scheduling:** Tasks A–C touch only new core files,
the block registry, the preset schema doc and `design/render/`, so they may run now. Task E (plugin face wiring)
waits until v0.8 has merged, because v0.8 I4b edits the plugin and preset schema.

## Task A — the circuit (core, `pedal.rat`)

A DSP model of the classic rat-style topology, built from published circuit analysis (no capture fitting yet):

1. **Input buffer and coupling HPF** (~ 20 Hz).
2. **Op-amp gain stage**: non-inverting, gain = 1 + DIST-pot / (two RC legs to ground), i.e. a gain that rises with
   frequency and has two shelving corners (~ 60 Hz and ~ 1.5 kHz at full DIST). Max gain ~ 67 dB.
3. **Op-amp slew-rate and gain-bandwidth limit** (LM308-class: slew ~ 0.3 V/µs, GBW-limited high end). This is what
   gives the circuit its character at high DIST; it must be modelled (a rate limiter on the op-amp output plus the
   GBW pole that moves with gain), not approximated by a fixed low-pass.
4. **Hard clipper to ground**: CLIP selector `silicon` (stock, 1N914 pair), `led`, `none` (op-amp rail clipping
   only, "turbo"-style louder and more open), `asymmetric` (1+2 diodes). Reuse `adaa_clipper` where it fits.
5. **FILTER**: the passive RC low-pass after the clipper (fixed R + 100 k reverse-log pot into a fixed C); FILTER at 0
   is brightest, at 10 darkest, as on the original.
6. **Output buffer and VOLUME**.

Controls (face): **DIST**, **FILTER**, **VOLUME**. Drawer extras for heavy use: **TIGHT** (pre-gain HPF 20–250 Hz,
shared semantics with the other pedals), **CLIP** (above), **MIX** (clean blend, latency-matched), **RUETZ**
(the well-known mod that removes part of the low-frequency gain leg: tighter low end). Stock = DIST 5, FILTER 5,
VOLUME at unity, TIGHT off, CLIP silicon, MIX 100, RUETZ off.

Rules (CLAUDE.md): no allocation, locks, I/O or exceptions in `process()`; oversampling through the existing
`oversampler` with the same alias budget as the other modelled pedals (state the measured alias level); latency
reported; deterministic and block-size independent; NAM-trainable = true (it is a static nonlinearity plus filters).
Calibration (v0.8): declare the pedal's nominal output level in dBu like the other modelled pedals.

## Task B — tests

- Frequency response of the linear path at DIST 0 / 5 / 10 against the analytic transfer function (± 0.5 dB).
- Slew limiting: a high-level 5 kHz sine at DIST 10 shows the triangle-like slew distortion (THD and the output's
  maximum slope within 10 % of the modelled slew rate).
- FILTER sweep: the −3 dB corner moves monotonically across the pot range; values at 0 / 5 / 10 within 10 % of the
  RC formula.
- Each CLIP mode: output peak and harmonic signature differ as expected (`none` louder, `asymmetric` has even
  harmonics).
- RUETZ on: low-frequency gain at DIST 10 is lower by the documented amount.
- Zero allocations in `process()`; block-size independence; latency reported; alias level ≤ the other pedals'.

## Task C — face art and starter presets

- `design/render/pedal_vermin.py`, in the same deterministic render pipeline and visual language as
  `pedal_b2.py` / `pedal_fuzz.py`: worn metal enclosure, three knobs (DIST, FILTER, VOLUME), footswitch, LED, the
  name VERMIN. No trademark text or trade dress copied from the original (no imitation of its logo or lettering).
- Starter presets in `presets/modeled/vermin/` (generic names), e.g.: Crust Grinder, Doom Filter Down, Thrash
  Boost (low DIST into an amp), Turbo Crust (CLIP none), Tight Hardcore (RUETZ + TIGHT), Grind Wall. Each states its
  intended use; all load and render in the golden test harness.

## Task D — matcher pool (match-engineer, small)

Register `pedal.rat` as a modelled-pedal candidate in the matcher's pedal pool (as `pedal.ts` is), with a coarse
parameter grid for the prescreen. No change to matcher logic.

## Task E — plugin (after v0.8 merges)

The pedal appears in the pedalboard with its face (filmstrip knobs, footswitch, drawer), like the existing modelled
pedals. Display names follow `docs/NAMES.md`.

## Acceptance

Tasks A–D reviewer-ACCEPTed, full CI green; REPORT `docs/specs/v0_9-vermin_REPORT.md` with the measured responses,
alias level, CPU cost per instance (RTF) and renders of the starter presets. User check after Task E: play the
starter presets in Logic; feel note in the REPORT. A capture-based fit (as v0.5 does for the saw and TS) comes later.
