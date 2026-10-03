# Sawblade plugin (phase 2 skeleton)

JUCE 8 plugin on top of `sawblade_core`: VST3 + Standalone on Linux (AU additionally on macOS).
Spec: `docs/specs/phase2_plugin.md`.

> **Licensing gate.** JUCE 8 is AGPLv3 or commercial. Sawblade is commercial: a JUCE commercial
> licence is required before distributing any plugin binary. Development builds are fine. See
> `docs/THIRD_PARTY.md`.

## Build

The plugin is off by default, so the core build and the existing tests are unaffected.

```
cmake -S . -B build-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON
cmake --build build-plugin
ctest --test-dir build-plugin --output-on-failure
```

Outputs: `build-plugin/plugin/SawbladePlugin_artefacts/Release/{VST3/Sawblade.vst3,Standalone/Sawblade}`
(`AU/` on macOS). JUCE is fetched (pinned commit, shallow) by FetchContent. The first build compiles
the JUCE modules twice (once for the plugin, once into the test executable), a few minutes each.

Linux system packages (Debian/Ubuntu names) needed to build JUCE with these targets:

```
libasound2-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
libfreetype-dev libfontconfig1-dev libgl1-mesa-dev libcurl4-openssl-dev
```

Running the Standalone or pluginval needs a display (use `xvfb-run -a` on a headless machine). The
test executable `sawblade_plugin_tests` needs neither a display nor an audio device: it links the
JUCE modules but never creates a window, a message loop or an audio device.

### pluginval (optional)

pluginval is not fetched by our CMake. Build it once (it is a JUCE application):

```
git clone --depth 1 --branch v1.0.4 --recurse-submodules --shallow-submodules https://github.com/Tracktion/pluginval.git
cmake -S pluginval -B pluginval/build -G Ninja -DCMAKE_BUILD_TYPE=Release   # also needs ladspa-sdk
cmake --build pluginval/build
cmake -S . -B build-plugin -DSAWBLADE_PLUGINVAL_EXECUTABLE=/path/to/pluginval
cmake --build build-plugin && ctest --test-dir build-plugin -R pluginval
```

The registered test runs `pluginval --strictness-level 10 --validate-in-process` on the VST3
(under `xvfb-run` when available). Result at the commit this was written: SUCCESS at level 10.

## Design

```
host in (mono, or stereo summed (L+R)/2)
  -> Engine::process
       [RtResampler host->model] -> Chain (model rate) -> [RtResampler model->host]
  -> mono out copied to every output channel (mono, or dual-mono stereo)
```

`Engine` (`plugin/src/Engine.*`, JUCE-free) is one preset's `Chain` plus the real-time rate
conversion. The models run at their training rate (all non-bypassed NAM blocks on enabled paths
must agree; a model that records no rate counts as 48 kHz, the NAM convention; no NAM blocks means the
host rate). `probeNamRates` / `commonModelRate` in `core/chain.h` implement this once for both the
plugin and `tonerender --render-rate auto`.
If that differs from the host rate the two `RtResampler`s are inserted; otherwise there are none.

### Threading model

| Thread | Does | Never does |
|---|---|---|
| audio (`processBlock`) | reads the parameter atomics, takes the current `Engine` from the `SwapSlot` (wait-free), `Engine::setParams`, `Engine::process` | allocate, lock, I/O, throw, call `setLatencySamples` |
| loader (`EngineLoader` worker) | `Engine::build` (loads models/IRs, prepares the chain, sizes buffers, runs the alignment probe), `SwapSlot::publish`, destroys engines the audio thread replaced (`collectGarbage`), calls `setLatencySamples` | touch an engine the audio thread owns |
| message / other | `loadPreset*`, `get/setStateInformation`, `status()`, editor | |

The only mutex (`SawbladeProcessor::mutex_`, plus the loader's request mutex) is taken by
non-audio threads only. The loader keeps the latest request only (a newer request supersedes one
in flight; stale results are discarded). Engines are never destroyed on the audio thread: the
`SwapSlot` retires them to a lock-free stack that the loader frees.

A rebuild happens only for a preset load, a state restore, or `prepareToPlay` with a changed host
rate / larger block size. `prepareToPlay` waits for its rebuild (up to 60 s, on the message thread)
so the latency the host reads right afterwards is right; if that build fails it publishes a
pass-through engine rather than leave one of the wrong rate running. Parameter changes never
rebuild anything.

On a swap the audio thread cross-fades (equal power, 30 ms, `kFadeSeconds`) from the outgoing engine to
the new one. Both engines run on the same input during the fade; the fade buffer is preallocated. The
`SwapSlot` carries an `EngineRef` (a `shared_ptr`): the audio thread copies it when it adopts an
engine, so it can keep the outgoing engine alive, and the loader keeps its own reference to every
engine it publishes and frees one only once nothing else holds it (so the last reference is never
dropped on the audio thread). The new engine starts with fresh state and its own latency, and the two
engines are not time-aligned (their latencies differ), so the two signals can add coherently (up to +3 dB
at the fade midpoint) or partially cancel (a dip, in the worst case of opposite phase a deep one);
the measured test case (identical presets) stays within 0 to +2.9 dB.

### Parameters (APVTS) and the preset

State is the preset JSON (`getStateInformation` writes it, `setStateInformation` parses it), with
capture paths made absolute so a host restoring a session from another working directory still
finds the files. The preset is the source of truth for everything discrete; these continuous
controls are host parameters:

| id | range | maps to |
|---|---|---|
| `inputGain`, `outputGain` | -24..+24 dB | `input.gainDb`, `output.gainDb` |
| `gateThreshold` | -80..-20 dB | `gate.thresholdDb` (audible when the preset's gate is enabled) |
| `blend` | 0..1 | `blend` (0 = Saw only, 1 = Body only) |
| `levelA`, `levelB` | -24..+12 dB | `paths.a/b.levelDb` |
| `postEq1..6` | -18..+18 dB | gain of the k-th gain-bearing band (peak/shelf, in order, skipping high/low-pass) of `postEq` |

Loading a preset writes its values into the parameters (clamped to the ranges above; the engine's
baseline is built from the same clamped preset, so nothing jumps). The saved state is the loaded
preset with the current parameter values written back (rounded to 1e-4 so a state round trip is
byte-stable). Smoothing lives in `Chain::setLiveParams`: input/output gain, path levels and the blend
ramp linearly per sample over 20 ms (block-size independent); post-EQ gains ramp in dB with the
band redesigned every 32 samples; the gate threshold moves immediately.

### Latency accounting (exact, in host samples)

```
total = H1 + (C + H2 + s2/L2) * L2/M2        (resampling)        total = C   (same rate)
```

`C` = `Chain::latencySamples()` (model-rate samples, processing latency only; the alignment delay
is part of the tone, as in `tonerender`). `H1` = input converter delay (host samples), `H2` =
output converter delay (model samples), `L2/M2` = host/model rate ratio. `s2` is a sub-sample
phase offset of the output converter, chosen at build time so that the total is a whole number of
host samples; this is why the reported latency equals the measured impulse delay exactly, for any
`C`. The Engine's output sample `j` is the input delayed by exactly `total`. `setLatencySamples`
gets `total`; the editor shows it. Measured (impulse through the whole processor): 44.1 kHz host
with 48 kHz models: 92 samples (2.09 ms); 96 kHz host: 183 samples (1.91 ms); 48 kHz host: 0.

### Real-time resampler (`core/include/sawblade/rt_resample.h`)

Fixed-ratio polyphase, streaming, mono float; separate from the offline `resample()` but the same
filter design (Kaiser beta 10, cutoff 0.97 and transition 0.14 of the lower Nyquist; constants
shared via `resample.h`). Exact rational ratio (e.g. 44.1<->48 kHz is 160/147; rates with no
representable ratio below 8192 phases use a continued-fraction approximation). All index
arithmetic is integer, so the output depends only on the input stream, not on how it is split into
blocks (bit-identical across block sizes). Output sample `j` needs input samples up to
`floor(j*M/L)` only, so no look-ahead FIFO is needed and the engine's output FIFO never
underruns (a counter is exposed and tested to stay 0). Everything is preallocated in `prepare()`;
`process()` takes any block size up to the prepared maximum, `Engine` splits larger ones.

Achieved response (`tests/test_rt_resample.cpp`, "frequency response" test; the offline spec is
passband within 0.05 dB to 20 kHz at 44.1<->48 kHz and stopband >= 90 dB). The stopband is a dense
sweep: every tone from the stopband edge (1.04 x the lower Nyquist) to the input Nyquist in 10 Hz steps
when downsampling, and every in-band tone whose image lands in the stopband (50 Hz steps) when
upsampling. The worst point is always at the stopband edge:

| conversion | passband worst abs gain | stopband worst alias/image (dense sweep) |
|---|---|---|
| 44.1 -> 48 kHz (to 20 kHz) | 0.0044 dB | -104.2 dB |
| 48 -> 44.1 kHz (to 20 kHz) | 0.0044 dB | -97.1 dB (22.935 kHz, 5 Hz scan; -99.6 dB at 10 Hz steps) |
| 96 -> 48 kHz (to 21.5 kHz) | 0.0001 dB | -95.6 dB (24.96 kHz) |
| 48 -> 96 kHz (to 21.5 kHz) | 0.0001 dB | -104.0 dB |
| 88.2 -> 48 kHz (to 21.5 kHz) | 0.0001 dB | -95.7 dB (24.96 kHz) |

Worst case over everything: about -95.6 dB, 5.6 dB better than the 90 dB spec. (An earlier, sparse
version of this table quoted -100 to -111 dB because it sampled a handful of tones away from the edge.)

Output equals the offline converter's output on the delayed input to within 2e-6 (float
coefficients vs double) for 11 rate pairs. Kernel sizes: 92 taps (44.1->48, 48->96), 100 taps
(48->44.1), 182 taps (96->48); one dot product per output sample.

A regression found in review: with the output converter's phase offset `s > L` (model rate above host
rate, i.e. 44.1 kHz and lower hosts) the first outputs' whole kernel lies before the stream start;
the tap count underflowed and `process()` read outside its buffer. It is fixed (the count is clamped,
the output there is exactly 0) and covered by a sweep of every `s` in `0..M-1` and of every chain
latency `C = 0..2M` for host rates 8000 to 192000 with 48 kHz models.

### Editor

Functional only: preset-load button, preset name, latency / rate / live-compatible read-outs,
load state and errors, one labelled slider per parameter (post-EQ slots without a band are
greyed). All colours, fonts and metrics live in `SawbladeLookAndFeel.h`; replacing the look is a
change to that class (plus the editor's layout), nothing else.

## Tests

`sawblade_plugin_tests` (headless): `plugin/tests/test_engine.cpp` (Engine, no JUCE) and
`plugin/tests/test_processor.cpp` (the processor driven like a host). Audio-thread rules are
enforced with the existing `AllocGuard` plus `LockGuard` (`plugin/tests/lock_guard.cpp`, counts
`pthread_mutex_lock`/`trylock`/rwlock via linker `--wrap`; Linux only, skipped elsewhere). Core
additions are tested in `tests/test_rt_resample.cpp` and `tests/test_chain_live.cpp`.
