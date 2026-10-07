# v0.8 B1 — calibration core math (new files only)

Parent spec: `docs/specs/v0_8-input_calibration.md` (Task B). Owner: dsp-engineer. Reviewer: reviewer.

Scope: pure, allocation-free-at-use math in **new files only**:
`core/include/sawblade/calibration.h`, `core/src/calibration.cpp`, `tests/test_calibration.cpp`, plus the minimal
CMake lines to build/test them. **No** edits to `chain.*`, `nam_block.*`, `preset*`, `render.*`, `plugin/`, `cli/`,
`match/`, `bindings/` (v0.4M and v0.6 are open on those). Integration is a later task.

## Model

A "reference" is the dBu level that corresponds to 0 dBFS at some point in the chain (NAM convention: a full-scale
sine at the converter equals `X` dBu; Task A confirms the exact wording). Levels are tracked through a path:

- Start: `ref = deviceDbu` (interface max input at minimum gain, dBu at 0 dBFS). **No numeric default** (lead,
  2026-10-07: user interfaces differ, Scarlett 4i4 +12/+12.5 dBu, Darkglass Anagram unpublished). Device unset →
  `ref` unknown; the first block needing it resolves neutrally (NAM: `ref := inputDbu`, gain 0 dB = today's
  behaviour; NominalOutput: sets `ref`); later hops are exact; `deviceUncalibrated` set.
- NAM block with `inputDbu`: its input gain is `gainInDb = ref − inputDbu`; afterwards `ref = outputDbu`.
- A block with no level conversion (EQ, gain-neutral DSP): `gainInDb = 0`, `ref` unchanged.
- A modelled DSP pedal declaring a nominal output: `gainInDb = 0`, afterwards `ref = nominalOutputDbu`.
- Missing `inputDbu` / `outputDbu`: take the per-gear-type default from `CalibrationDefaults`; if the default is
  itself unset, use **neutral** (`inputDbu := ref`, so `gainInDb = 0`; `outputDbu := ref`), i.e. today's behaviour.
  Either way the block is flagged `uncalibrated` with which side(s) were missing — never silent.

Consequence the tests must show: a pedal→amp hop is `pedal.outputDbu − amp.inputDbu`; swapping an amp whose
`inputDbu` differs by `d` changes its gain by exactly `−d` and nothing else.

## API (names may be refined; keep the shape)

```cpp
namespace sawblade::calibration {
double dbfsToDbu(double dbfs, double deviceDbu) noexcept;   // dbfs + deviceDbu
double dbuToDbfs(double dbu, double deviceDbu) noexcept;    // dbu - deviceDbu
double dbToLinear(double db) noexcept;

enum class GearKind { Amp, Pedal, FullRig, Unknown };          // mirror TONE3000 gear types
enum class LevelKind { Nam, Neutral, NominalOutput };
struct BlockLevelInfo { LevelKind kind; GearKind gear; std::optional<double> inputDbu, outputDbu; };
struct GearDefault { std::optional<double> inputDbu, outputDbu; };
struct CalibrationDefaults { GearDefault amp, pedal, fullRig, unknown; };   // documented values
struct DeviceCalibration { std::optional<double> dbu; bool calibrated() const noexcept; }; // method/date/gain-at-min owned by caller layer
struct BlockGain { double gainInDb; float gainInLinear; double refAfterDbu;
                   bool inputMissing, outputMissing; bool uncalibrated() const noexcept; };
struct PathPlan { std::vector<BlockGain> blocks; std::optional<double> refOutDbu; bool deviceUncalibrated; bool anyUncalibrated; };

PathPlan planPath(const DeviceCalibration&, std::span<const BlockLevelInfo>, const CalibrationDefaults&);
}
```

`planPath` runs at load/swap time (it may allocate its result vector); the audio thread will only read the
precomputed linear gains later. Non-finite metadata is treated as missing. All math in `double`.

## Acceptance (Catch2, `tests/test_calibration.cpp`)

1. dBu↔dBFS round trip and known points (e.g. device +12 → −18 dBFS = −6 dBu).
2. Single amp: `gainInDb == deviceDbu − amp.inputDbu`, linear value matches `10^(dB/20)` within 1e-6.
3. Pedal→amp hop equals `pedal.outputDbu − amp.inputDbu`; neutral EQ between them changes nothing.
4. Amp swap: two plans differing only in amp `inputDbu` by `d` → amp gain differs by exactly `−d`, every other block
   identical.
5. Missing metadata: gear default applied and flagged; no default → neutral (0 dB) and flagged; both flag fields correct.
6. NaN/Inf metadata → treated as missing. Uncalibrated device: amp alone → 0 dB + flag; pedal→amp → first 0 dB, hop
   exact; amp swap → 0 dB both (absolute level needs device calibration).
7. Nominal-output DSP pedal sets the reference for the next NAM block.
8. Full suite passes; warnings-as-errors clean.
