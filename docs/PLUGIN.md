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

Loading a preset writes its values into the parameters (clamped to the ranges above and snapped to
the 1e-4 grid by `snapParam`; the engine's baseline is built from the same clamped, snapped preset,
and `readParams()` snaps what the audio thread reads the same way, so the two are bit-identical on
every platform (an FMA-fusing compiler perturbs `convertFrom0to1`, and the float parameter is not
the preset's double) and nothing jumps or ramps spuriously). The saved state is the loaded
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

### Play-along (phase 5.2)

A backing track from a folder of already-separated stems (`drums`, `bass`, `vocals`, `other`, `guitar`/`guitars`;
`.wav` or `.flac`), played through core's `StemPlayer` and mixed **after** the rig. Spec:
`docs/specs/phase5_2_playalong_plugin.md`. Code: `plugin/src/PlayAlong.{h,cpp}` (JUCE-free: transport, settings,
loader, loudness), `plugin/src/PlayAlongPanel.{h,cpp}` (the panel).

**Panel.** An overlay docked along the bottom of the 1280 x 800 design, toggled by the PLAY ALONG button in the top
bar, closed by default (open / closed is UI state and is not saved). It uses the skin's palette and plain-widget
drawing; no PLAY ALONG hardware render exists yet. Controls: LOAD SONG (folder picker; dropping a folder anywhere on
the editor also loads it), play / pause, position display, seek bar, loop SET A / SET B / LOOP, COUNT-IN with BPM, guitar
stem MUTE / GHOST / FULL, KEEP KEYS, backing level, offset, and in the plugin SYNC TO HOST.

**Standalone vs plugin.** `wrapperType == wrapperType_Standalone` (a processor built outside any wrapper, as in the
tests, counts as a plugin; `PlayAlong::setStandalone` overrides it). Standalone: always enabled, free-run, the panel's
play / pause / seek drive it. Plugin: the backing is off by default; SYNC TO HOST enables it and it then follows the host
transport through the 5.1 host-follow API (play / stop and position jumps; the panel's transport controls are disabled
because the host owns the transport).

**Threading.**

| Thread | Does | Never does |
|---|---|---|
| audio (`processBlock`) | drains the command queue, runs `StemPlayer::process` (backing delayed by the rig latency), measures the rig output loudness, publishes a snapshot (atomics) | allocate, lock, I/O, throw |
| play-along loader (own worker) | `loadStemDirectory`, `StemPlayer::setStemSet` (SwapSlot), `collectGarbage()` (also every 0.5 s, so a retired set is freed here and never on the audio thread) | touch the player's audio-thread state |
| message / host / tests | every `PlayAlong` setter: updates the saved settings under a mutex and pushes a command into a preallocated lock-free SPSC queue (256 entries; producers are serialised by a mutex the audio thread never takes) | |

The play-along loader is its own thread, not the `EngineLoader`: a long song load must not hold up preset builds or
`prepareToPlay` (which waits for the engine loader). Latest request wins. A new set replaces the old one only while the
transport is stopped (`StemPlayer` rule); a **user** load in Standalone (LOAD SONG or a drop) therefore **pauses the
playback** so the new set is adopted, whereas a reload (state restore, rate change, KEEP KEYS) does not touch the
transport and the old set keeps playing until the next stop. `StemPlayer`'s own
setters are audio-thread-only, so the message thread never calls them: continuous values and transport go through the
queue, `setStartOffsetSamples` is atomic. A loop is re-applied by the audio thread whenever a set is adopted. If the queue is ever full, the dropped command
sets a resync flag; the loader's 0.5 s tick then re-sends the whole settings state (level, guitar mode, count-in, loop,
host sync, offset), so the audio side cannot stay out of step with the panel. Transport events (play, pause, seek) are
not replayed.

**Rig latency.** `setRigLatencySamples(chain latency)` is called whenever the engine is published and in
`prepareToPlay`, so the backing is delayed by the same number of samples the host is told (the plugin reports the
latency; the host compensates other tracks by it). A latency change while playing is not click-free (core rule).

**Offset.** The panel, the saved state and `tonerender --backing-offset-ms` use the matcher's convention (the matcher's
`--offset-ms`: where the DI starts inside the song). Positive: the stems lead (stem audio from `offset` plays at
playhead 0). Negative: the backing starts `-offset` into the playhead. `StemPlayer::setStartOffsetSamples` has the
opposite sign (playhead p plays stem sample p - offset); the plugin negates. Seek, loop points and the position display
are in playhead time; in plugin mode a host position p plays stem sample p + offsetMs. The offset takes effect while the
transport is stopped.

**Suggested level.** The backing level is a plain control (-40..+6 dB, default 0). When the user loads a song (LOAD SONG
or a drop; never on a state restore, a sample-rate reload or a KEEP KEYS reload) it is set **once** to

```
level = clamp(rigLufs - StemSet::backingLoudnessLufs, -40, +6)
```

`rigLufs` is a running estimate of the rig's output loudness taken on the audio thread (K-weighted, 100 ms hops,
absolute gate -70 LUFS and a -10 LU relative gate, 30 s leaky memory, dual-mono output counted as two channels;
needs 0.5 s of gated signal). If no rig signal has been observed yet the reference is a fixed -18 LUFS. If the stems are
silent the level stays 0. The backing then sits at the rig's loudness; it is never adjusted automatically again.

**Stem roles.** 4-stem separations put the guitars in `other`. By default (`OtherRole::Guitar`) a folder without a real
guitar file loads `other` as the guitar stem, so MUTE removes it. KEEP KEYS loads it as `other` instead (it reloads the
folder). A real `guitar`/`guitars` file always wins.

**State.** Plugin state is the preset JSON plus an optional top-level `playAlong` object (folder, offsetMs, loop, countIn,
guitarMode, backingLevelDb, otherRole, hostSync; see `docs/PRESET_SCHEMA.md`). It is written only once the play-along has
been touched, so untouched sessions save exactly the preset, and a state without it leaves the play-along as it is.
Restoring a folder starts a background load (after `prepareToPlay` when the rate is not known yet); a missing, empty
or undecodable folder shows a message in the panel and never throws; the saved path is kept. Nothing from a song is ever
saved in the state, only its path.

**Known limits.** The panel offers no slow-down / transpose (5.3). A new song loaded in plugin mode while the host plays
is adopted when the host stops. Toggling KEEP KEYS reloads the song (decode time; resampling too if the files are not at
the host rate). The whole song is held in memory at the host rate (5.1 open question 1).

### Record + Match (phase 6a)

The loop: play along to a song, record the clean DI, MATCH it against the song, audition the results, apply one, keep
playing. Spec: `docs/specs/phase6a_record_match_plugin.md`. Code: `plugin/src/TakeRecorder.{h,cpp}` (recorder, JUCE-free),
`JobRunner.{h,cpp}` (child processes, settings, parsers), `MatchGlue.{h,cpp}` (what MATCH / EXPORT start from),
`PresetAudition.{h,cpp}` (audition / A-B / apply), `MatchScreen.{h,cpp}` (the overlay), and the record band at the bottom of
`PlayAlongPanel`.

**Recorder.** REC in the play-along panel records the plugin's **input**: the clean DI, before the gate and the rig (a
stereo input is summed to mono exactly as the rig does). The audio thread pushes each block into a preallocated lock-free
ring (the next power of two above 2 s at the host rate: 131072 samples at 48 kHz, allocated in `prepareToPlay`); a writer
thread drains it into a **32-bit float mono WAV** and writes the sidecar when the take ends. Takes live in
`~/Library/Application Support/Sawblade/takes/` (macOS) or `~/.local/share/sawblade/takes/` (Linux); the environment
variable `SAWBLADE_DATA_DIR` replaces the `Sawblade` / `sawblade` root, and `TakeRecorder::setTakesDir` overrides the folder
(tests). Nothing on the audio thread allocates, locks or does I/O: a take starts and stops at a block boundary through a
small preallocated event queue, and a full ring never blocks. A block the ring cannot take is dropped, counted
(`overruns`, shown in the panel and stored in the sidecar) and the writer later pads the file with the same number of
zeros, so the take stays on the DI timeline and `lengthSamples` is always the number of samples that went by. Only a take
that has ended has a sidecar, so a take in progress never shows up in the list.

Sidecar `<take>.json`:

```json
{ "version": 1, "sampleRate": 48000.0, "channels": 1, "lengthSamples": 480000, "overruns": 0, "droppedSamples": 0,
  "playAlong": { "running": true, "stemSampleIndex": 1507200, "stemSampleRate": 48000.0, "songFolder": "/songs/x" },
  "createdUtc": "2026-10-04T12:00:00.000Z" }
```

`playAlong` is `null` when no song was loaded. `stemSampleIndex` is the stem sample that plays at the take's **first
sample**: the playhead (the player's position in Standalone, the host position in plugin mode) minus the applied player
offset, so the offset setting is already in it. The backing is delayed by the rig latency, which is exactly what lines the
DI sample up with the stem sample the player plays at that moment. `running` is false when the backing was paused, counting
in, or (plugin mode) not following the host: the index is then only where the playhead was, and MATCH does not use it.
`droppedSamples` is an addition to the spec's field list. The matcher's `--offset-ms` is
`stemSampleIndex / stemSampleRate * 1000` (the DI starts that far into the song). A loop that wraps during the take
breaks the single offset (the take is one straight run of the DI; the offset is valid until the first wrap).

**Panel.** The play-along panel is 112 px taller (`PlayAlongPanel::kHeight` = 170 + 112); the new band holds REC / STOP with
a timer and a lamp, the take list (name, length, position in the song, overruns, a tag on the one used for MATCH), RENAME,
DELETE (both confirm in a dialog), USE FOR MATCH, MATCH and EXPORT NAM. The selection used for MATCH is a setting, not tone
state.

**Standalone only.** MATCH and EXPORT NAM run the Python tools, so for now they are Standalone-only: `matchEnabled()` is
`playAlong().standalone()` (a processor outside the Standalone wrapper counts as a plugin). In plugin mode the two buttons
only show "... runs in the Standalone app: open the Standalone app." Recording works in both.

**Job runner.** `JobRunner` (owned by the processor, so jobs survive the editor and the panel closing) starts
`sawblade-match` and `sawblade-export` with `juce::ChildProcess`. Every job has a folder
`<app data>/jobs/<timestamp>-match|export/` with `job.json` (kind, state, pid, start / finish time, exit code, the command
line, the progress mode), `log.txt` (the child's output) and the tool's own files. The folder is the source of truth: a runner that finds a
`running` job whose pid is alive monitors it again (`attachExisting()`, called when the screen opens), a finished one is
loaded with its results, and a `running` job whose process is gone is reported as failed (or as succeeded if it left its
result). One match and one export can be active at the same time; a second job of the same kind is refused.

- *Threads.* The message thread only starts, cancels and copies snapshots. Each job has one monitor thread (probe,
  launch, follow the log and the progress file, notice the exit). There is no pipe and no reader thread.
- *Process.* The tool is started with `posix_spawn` as the leader of its own process group (`POSIX_SPAWN_SETPGROUP`, so
  pgid = pid), with stdout and stderr appended to `<job>/log.txt` (a file: a child that floods its output cannot stall,
  and the log survives the app). The monitor parses new complete lines of that file; stage and message come from them
  when there is no progress file.
- *Cancel.* SIGTERM to the whole process group, then SIGKILL to the group after 2.5 s, then a last group SIGKILL once the
  tool has exited, so helpers it started (worker processes) die with it. The job is then `cancelled` in job.json.
- *Re-attach.* job.json stores `pid` and `pgid`. A job survives the app quitting (the tool keeps running, its output
  keeps going to log.txt). A runner started later re-attaches by pid (`kill(pid, 0)`, reaping it if it is its own
  child), follows log.txt and progress.json, cancels through the group, and when the pid is gone decides the final
  state from the files: `result.json` (match) / `export_report.json` or a `.nam` (export) means succeeded, otherwise
  failed ("stopped without a result"). Exit codes are only known for jobs the runner started itself.
- *Tools and settings.* The matcher executable (default `<repo>/match/.venv/bin/sawblade-match`, `<repo>` from the build's
  source dir), the exporter (`.../sawblade-export`) and the pool manifest
  (`~/.cache/sawblade/captures/pool_manifest.json`) are settings in `juce::PropertiesFile` application properties
  (`<app data>/settings.properties`), **not** in the preset or the plugin state: the tone state bytes are unchanged. A
  missing or non-executable tool, or a missing pool, gives a clear message and a LOCATE... button (async file chooser).
- *Progress.* The runner runs `<exe> --help` once per executable. If it lists `--progress-json`, the matcher is started with
  `--progress-json <job>/progress.json` and that file (`{stage, fraction, etaSeconds, bestErrorDb, message}`, partial or
  missing reads ignored) feeds the bar, the stage, the ETA and the best error. Otherwise the log lines give the stage
  (`stage2` / `stage3` markers), the last line as the message and an indeterminate bar, with no ETA. The exporter's progress
  is `<out>/checkpoint/progress.json` (epoch against the epoch or minute budget).

**MATCH.** The screen (after `design/mockups/FullMatch.dc.html`, in the plugin's skin) shows the reference, the DI take and
the tools, a progress bar with stage, message, ETA and best error, CANCEL, and the results. The command line is
`sawblade-match --di <take.wav> --ref <stem file> --ref-channel mid --pool <manifest> [--offset-ms <ms>] --out <job dir>
[--progress-json <job>/progress.json]`:

- the reference is the loaded song folder's guitar stem (`guitar` / `guitars`), else `other`, else a mix, else the first
  audio file; the label on the screen says which. `--stems-dir` is not passed (it is the calibrate stem cache);
- `--offset-ms` comes from the take's sidecar and is omitted when no backing was running or when the take was recorded
  against another song (the matcher then searches within +-3 s).

The results (`result.json`: `best` and `alternatives`, `loss` shown as dB, `topology`, a captures summary) list the matcher's
choice first. **AUDITION** loads the candidate's `*.preset.resolved.json` through the processor's normal off-thread preset
load (the EngineLoader builds it, the audio thread cross-fades) and keeps the preset that was current before the first
audition; **A / B** switches between that preset (A) and the candidate (B); **APPLY** makes the selected candidate the
current preset (what a normal preset load leaves; the audition ends); **REVERT** goes back to A. Auditioning is a real load:
the plugin state holds whichever side is playing.

**EXPORT NAM.** The same screen in export mode: NO CAB / WITH CAB (the hint says which is exact for the current blend: a
live blend, both paths on one cab, is exact without the cab; a studio blend only with it), FEATHER / LITE / STANDARD, device
`auto` (the tool's default; `--device auto` is passed), the licence note of the export, progress, and REVEAL
(`File::revealToUser` on the result folder). It runs `sawblade-export <preset> --mode <m> --size <s> --device auto
[--di <selected take>] --out <job>/export`. The preset is the auditioned / applied candidate's resolved file when that is what
is still exactly the current preset (compared as state JSON, so any later preset load or parameter change forgets the association), otherwise the current preset written to `<jobs>/inputs/` (so the export is what is playing, parameter changes included).
The selected take is also passed as the validation DI. A capture without a file (NAM model, or the cab IR while the cab is on) blocks the export with a message instead of launching; the written file keeps absolute capture paths (tested by loading it back with `loadPresetFile`).

**Tests.** `plugin/tests/test_record_match.cpp` (headless; recorder: no allocation or lock while recording, WAV bit-exact for
mixed block sizes, overrun counting with a stalled writer and the silence padding, a take shorter than a writer pass, the
sidecar offset with a running StemPlayer in Standalone and host-follow modes, rename / delete, re-prepare; runner against a fake
child written at test time (`plugin/tests/fake_tools.h`, a Python script that answers `--help` with and without
`--progress-json`, writes progress.json, log lines, result.json and presets, and can wait on gate files): progress in both
modes, the command line, result list, cancel (SIGTERM and the SIGKILL escalation), failure message, missing tools, re-attach to
a running and a finished job, a re-attached cancel, a child that floods the pipe, export progress; settings persistence;
audition / A-B / apply; the plugin-mode gating) and the editor tests in `plugin/tests/test_editor.cpp` (the band, the
Standalone-only buttons, the missing-tool message, a job that survives the editor, and the screenshots
`build/screenshots/sawblade_record_armed_1x.png`, `sawblade_match_progress_1x.png`, `sawblade_match_results_1x.png`,
`sawblade_export_progress_1x.png`). All test data is synthesised into temp dirs; `SAWBLADE_DATA_DIR` keeps the default
locations out of the home folder.

**Known limits.** MATCH / EXPORT are Standalone-only. The take list is rescanned when the recorder changes it and every 10 s while the panel is open. Takes and job folders are never deleted by the plugin.

### Editor

A skinned prototype of the main rig screen (`design/mockups/RigReal.dc.html`, spec
`docs/specs/phase2_5_skin.md`); the final UI is the user's design. A fixed 1280 x 800 design laid out in one
content component that the editor scales with an `AffineTransform` (resizable, fixed 1.6 aspect, 640x400 to
2560x1600). It reads `status()` and the APVTS only. Layout: top bar (preset button opening the preset file chooser,
latency chip, LIVE / STUDIO chip, placeholders for A/B, MATCH, EXPORT NAM; MATCH and EXPORT NAM are opened from the
play-along panel, see Record + Match), rig area (amp heads, cab, pedalboard
with two pedals, footswitches and LEDs; click a piece to select it) and an inspector (BLEND, MASTER and POST EQ
knobs; all 12 parameters are bound to exactly one knob each).

Pictures come from `plugin/assets/` (our own renders, embedded with `juce_add_binary_data`). Controls live in
`plugin/src/skin/`: `SkinAssets` (decodes the PNGs and JSON sidecars once), `FilmstripKnob`, `FootswitchButton`,
`LedIndicator`, `RigView`. Colours, fonts and plain-widget drawing are in `SawbladeLookAndFeel`. Placeholders
(disabled, titled, with a tooltip saying so): A/B, MATCH, EXPORT NAM, previous / next preset, BROWSE CAPTURES,
LEARN GATE, "+ PEDAL", CPU readout, the footswitch (visual bypass only).

## Tests

`sawblade_editor_tests` (`plugin/tests/test_editor.cpp`, ctest prefix `editor: `, run under `xvfb-run -a` when
available): snapshots to `build/screenshots/sawblade_skin_{1x,2x}.png`, resizing, parameter bindings, filmstrip
mapping, knob interaction, footswitch / LED, accessibility, and the `plugin/assets/` budget (< 25 MB). The play-along
tests cover the panel (exists, closed by default, opens from the top bar), its controls bound to the processor, folder
drop, the missing-folder message, and screenshots `build/screenshots/sawblade_playalong_{closed,open}_1x.png`. The Record + Match
tests are described in that section.

`sawblade_plugin_tests` (headless): `plugin/tests/test_engine.cpp` (Engine, no JUCE),
`plugin/tests/test_processor.cpp` (the processor driven like a host) and `plugin/tests/test_playalong.cpp` (the backing:
level rule, queue, Standalone and host-follow transport, rig-latency alignment, offset, state, zero allocations and
locks with the backing playing; stems are synthesised into a temp dir, no audio is committed). Audio-thread rules are
enforced with the existing `AllocGuard` plus `LockGuard` (`plugin/tests/lock_guard.cpp`, counts
`pthread_mutex_lock`/`trylock`/rwlock via linker `--wrap`; Linux only, skipped elsewhere). Core
additions are tested in `tests/test_rt_resample.cpp` and `tests/test_chain_live.cpp`.
