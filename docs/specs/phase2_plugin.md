# Phase 2 spec — plugin skeleton (dsp-engineer, worktree branch)

JUCE 8 (pin a tag) + VST3 (AU on macOS builds; Linux CI builds VST3 + Standalone).
**Licensing gate (lead/user):** JUCE 8 is dual-licensed (AGPLv3 or commercial tiers);
Sawblade is commercial → a JUCE commercial licence is required before distribution. Record
in docs/THIRD_PARTY.md. Development builds are fine.

- `plugin/`: AudioProcessor wrapping `sawblade::Chain`; mono-in → mono or stereo-out (dual
  mono). State = preset JSON (`getStateInformation` / `setStateInformation`).
- Preset load / capture swap on a background thread: build `ChainResources` + new `Chain`,
  `prepare()` at the host rate, hand over via `SwapSlot`; old chain freed off the audio
  thread. No allocation, locks or I/O in `processBlock` (allocation-guard test with a
  headless processor harness).
- Real-time sample-rate conversion when host rate ≠ model rate: fixed-ratio polyphase
  resampler (in/out), latency reported via `setLatencySamples` (includes resampler +
  chain latency); varying host block sizes handled with internal FIFOs.
- Parameters (APVTS) for the continuous controls (input/output gain, gate threshold, blend,
  per-path level, post EQ gains) mapped onto the preset; discrete choices via the preset.
- UI: functional generic editor only (labelled sliders + preset load button + latency and
  live-compatible readouts). The user's hardware-style design replaces it later; keep all
  look-and-feel in one replaceable layer.
- Tests: headless processor tests (prepare/process at 44.1/48/96 kHz, block sizes 1–4096,
  state round-trip, swap under load without allocations), and a pluginval run if buildable.
