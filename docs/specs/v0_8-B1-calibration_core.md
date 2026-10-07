# v0.8 B1 — calibration core math (new files only)

Parent spec: `docs/specs/v0_8-input_calibration.md` (Task B). Owner: dsp-engineer. Reviewer: reviewer.

Scope: pure, allocation-free-at-use math in **new files only**:
`core/include/sawblade/calibration.h`, `core/src/calibration.cpp`, `tests/test_calibration.cpp`, plus the minimal
CMake lines to build/test them. **No** edits to `chain.*`, `nam_block.*`, `preset*`, `render.*`, `plugin/`, `cli/`,
`match/`, `bindings/` (v0.4M and v0.6 are open on those). Integration is a later task.

## Model

A "reference" is the dBu level that corresponds to 0 dBFS at some point in the chain (NAM convention: a full-scale
sine at the converter equals `X` dBu; Task A confirms the exact wording). Levels are tracked through a path:

- Start: `ref = deviceDbu` (interface max input at minimum gain, dBu at 0 dBFS). Device unset / non-finite / out of
  range → `ref = kAssumedDeviceDbu` (+12 dBu: NAM plugin default, Scarlett 4i4 4th gen, Audient iD4 MKII — sources in the
  REPORT A1b) and `deviceUncalibrated` set. Lead decision 2026-10-07 after Task A: a neutral fallback would make amp
  swaps change drive again (the problem v0.8 fixes); an assumed level keeps every capture-to-capture difference exact
  and only shifts the absolute level (≈ ±1 dB across sourced interfaces). Metadata outside [−60, +60] dBu = missing.
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
constexpr double kAssumedDeviceDbu = 12.0;                    // cited; used while uncalibrated
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
struct PathPlan { std::vector<BlockGain> blocks; double refOutDbu; bool deviceUncalibrated; bool anyUncalibrated; };

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
6. NaN/Inf/out-of-range metadata → treated as missing. Uncalibrated device: amp alone → `12 − in` + plan flag; amp
   swap → exactly `−d`; pedal→amp hop exact.
7. Nominal-output DSP pedal sets the reference for the next NAM block.
8. Full suite passes; warnings-as-errors clean.
