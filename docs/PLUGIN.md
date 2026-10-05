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
drawing; no PLAY ALONG hardware render exists yet. Controls: CHOOSE SONG FILE… (files-only picker, audio filter) and CHOOSE STEMS FOLDER…
(directories-only picker, no filter); a song file or stems folder dropped anywhere on the editor, including the panel itself
(which is its own drop target), also loads it (`PlayAlongPanel::loadDroppedFiles`). Never combine files and directories in
one native chooser with a type filter: the macOS panel greyed the .wav out (v0.2.1 Task G), play / pause, position display, seek bar, loop SET A / SET B / LOOP, COUNT-IN with BPM, guitar
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
guitarMode, backingLevelDb, otherRole, hostSync, and, only when set, `songFile` and `separationModel` (`"htdemucs"` = the 4-stem fallback); see `docs/PRESET_SCHEMA.md`). It is written only once the play-along has
been touched, so untouched sessions save exactly the preset, and a state without it leaves the play-along as it is.
Restoring a folder starts a background load (after `prepareToPlay` when the rate is not known yet); a missing, empty
or undecodable folder shows a message in the panel and never throws; the saved path is kept. Nothing from a song is ever
saved in the state, only its path.

**Known limits.** The panel offers no slow-down / transpose (5.3). A new song loaded in plugin mode while the host plays
is adopted when the host stops. Toggling KEEP KEYS reloads the song (decode time; resampling too if the files are not at
the host rate). The whole song is held in memory at the host rate (5.1 open question 1).

### Capture browser (phase 8b, `plugin/src/browser/`)

BROWSE CAPTURES (inspector) opens a full-editor overlay (`CaptureBrowser`) for the selected rig piece; spec
`docs/specs/phase8_capture_browser.md`. Layers, bottom to top:

- `T3kRunner`: runs `sawblade-t3k` (a `juce::ChildProcess`, stdout + stderr merged) on one worker thread with a
  watchdog thread that kills the child at the per-job deadline (30 s queries, 120 s fetch, login = code expiry).
  Results reach the message thread through `callAsync` behind an "alive" flag, so nothing is delivered after
  destruction. A newer job with the same supersede key replaces a queued older one.
- `T3kJson` (pure) parses the CLI's JSON contract; `T3kClient` builds the argv per command and maps a run to a
  parsed struct or an error (`launch`, `timeout`, `parse`, `exit` plus the CLI's own codes). A non-zero exit with no
  error object shows the output tail. Login output is never kept: only parsed `device_code` / `logged_in` lines
  are used.
- `BrowserSettings`: the executable path (`<app-data>/Sawblade/browser.settings`, not plugin state). Default
  `<repo>/match/.venv/bin/sawblade-t3k` (`SAWBLADE_REPO_DIR`).
- `SlotTarget` (pure): rig piece -> preset capture; `withCapture` substitutes a fetched file (and `source`) into a
  copy of the preset. USE hands that copy to `SawbladeProcessor::loadPreset` (the normal background build +
  lock-free swap).
- `BrowserController` holds the state (login, query, records, selection, models, status) and runs USE / PREVIEW;
  `CaptureBrowser` only shows it.
- Preview: the candidate preset is rendered with `renderPreset` on a preview worker (the embedded
  `preview_riff.wav` at the host rate, peak-normalised to -3 dBFS) and handed to the processor's `PreviewPlayer`
  (a `SwapSlot` of immutable buffers). `PreviewPlayer::process`, the only audio-thread code of the feature, runs at
  the end of `processBlock`: it cross-fades the rig output into the preview over 10 ms, plays it dual-mono, and
  fades back at the end or on `stop()`. No allocation, no locks (covered by the alloc / lock harness); the preview
  adds no host-reported latency.

### Live pedal parameters, the circuit switch and the pedal face (phase 7b)

Four modeled-pedal circuits exist (CHAINSAW = `pedal.hm`, BIG FUZZ = `pedal.muff`, MODDED SAW = `pedal.hmx`, ONE-KNOB SAW
= `pedal.eye`; the last two are phase 7c, one more row each in `CircuitFaces`). Each has its own host-parameter set
(`hm*` / `muff*` / `hmx*` / `eye*`, built from the core's live-parameter descriptors in `plugin/src/pedals/CircuitParams`), plus the
`sawCircuit` choice. The sets control the first circuit block of the preset (path a, then b) and are inert when there is
none. Every circuit parameter is live: `Engine::setParams` forwards the active set to `Chain::setBlockLiveParams` when a
value changed (core ramps gains over 20 ms; no rebuild, no allocation or lock on the audio thread). `sawCircuit` is the one
parameter that rebuilds: the processor swaps the block type (level/volume, mix, tightness and clip carried over where the
new circuit has them, the rest at defaults) and hands the new preset to the loader like any preset load (build off-thread, cross-fade). A change coming
from a non-message thread is flagged and handled by a message-thread timer. The saved state is the preset, so it always
carries the block type that is playing. The editor adds `PedalFace` (live controls laid over the SAW pedal render: six
knobs, CLIP and FOCUS switches, the CIRCUIT switch, label chips, OLED overlay) and `AdvancedDrawer` (deep controls, slides
out to the right of the pedal on double-click; close with a second double-click, the x or Escape; UI state, never saved).
Both are driven by one table row per circuit (`CircuitFaces`). A preset chaining two circuits exposes the first one.

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

`playAlong` is `null` when no song was loaded. `songFolder` is the stems directory that was playing: the folder for a song loaded as a folder, and the separation cache directory when the song came from a file (LOAD SONG on an audio file); MATCH uses it as the reference. `stemSampleIndex` is the stem sample that plays at the take's **first
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

**MATCH is Standalone only.** MATCH runs the Python matcher, so for now it is Standalone-only: `matchEnabled()` is
`playAlong().standalone()` (a processor outside the Standalone wrapper counts as a plugin). In plugin mode the button
only shows "MATCH runs in the Standalone app: open the Standalone app." Recording works in both. EXPORT NAM is available
in both (phase 12, "NAM export" below).

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
- *Spawn hygiene.* Empty signal mask, SIGPIPE / SIGTERM / SIGINT back to default, and no inherited descriptors beyond
  0, 1, 2 (`POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS, `addclosefrom_np` with glibc >= 2.34).
- *Cancel.* SIGTERM to the whole process group, then SIGKILL to the group after 2.5 s, then a last group SIGKILL once the
  tool has exited, so helpers it started (worker processes) die with it. The job is then `cancelled` in job.json.
- *Re-attach.* job.json stores `pid` and `pgid`. A job survives the app quitting (the tool keeps running, its output
  keeps going to log.txt). A runner started later re-attaches by pid, after checking that it is still that tool: its own child is waited for;
  any other pid must be in the recorded process group (`getpgid`) and have started within 4 s of the recorded
  `spawnedEpochMs` (`/proc/<pid>/stat` + `btime` on Linux, `sysctl KERN_PROC_PID` on macOS). A reused pid fails the
  check, is treated as gone and is never signalled. It then follows log.txt and progress.json, cancels through the group, and when the pid is gone decides the final
  state from the files: `result.json` (match) / `export_report.json` or a `.nam` (export) means succeeded, otherwise
  failed ("interrupted"). Exit codes are only known for jobs the runner started itself.
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

**Two-pass MATCH: quick, then thorough (phase 6a.1).** Spec: `docs/specs/phase6a_1_quick_then_thorough.md`.

- *Passes.* The runner reads `<exe> --help` once (the same cached probe as `--progress-json`). If it lists both `--quick`
  and `--thorough`, MATCH starts `sawblade-match ... --quick` first; its candidates fill the list with a **PREVIEW** badge.
  When the quick job succeeds and **auto-refine** is on, the runner starts `... --thorough` by itself, on a thread of
  its own, with the same `--di`, `--ref`, `--ref-channel`, `--offset-ms` and `--pool` and its own job folder. While it runs, a
  thin bar and **REFINING... n%** sit on the results header. If the tool does not list both flags (a tool without them, or
  with only one), MATCH is the single run of phase 6a: no PREVIEW badge, no refinement.
- *Match session.* A MATCH is a quick job folder plus an optional thorough one. Each `job.json` records
  `"pass": "quick" | "thorough"` and `"pair": "<the other folder's name>"` (a single run has no `pass`), and a `request`
  object (DI, reference, offset, labels) so the refinement can be started from the quick job alone. A re-attach (panel or
  app closed and reopened) rebuilds the pair from these. A quick job that finished with no thorough job, and auto-refine on,
  starts the refinement once (its folder then exists, whatever became of it); a thorough job that is running is re-attached
  and is not restarted; a quick job that is still running starts the refinement itself when it ends.
- *Auto-refine.* A toggle in the match screen's TOOLS area (default on), kept in the settings file
  (`juce::PropertiesFile`, key `autoRefine`), never in the plugin state. With it off the thorough job is not started and
  the quick results simply stay PREVIEW.
- *Age limit.* A refinement is only auto-started (on re-attach, when the screen opens) for a quick job younger than
  `kRefineMaxAgeHours` = 24 h, measured from its finish time (its spawn time if it has none). For an older finished quick
  job with no thorough partner no refinement starts, and the results header says "Preview is over 24 h old: re-run MATCH to
  refine."
- *Never interrupts audio.* When the thorough result arrives **nothing is loaded**. The list gains a **REFINED** section at
  the top (header + its candidates) with a "REFINED READY" badge, the quick candidates move below it under a PREVIEW header,
  the selection stays on the candidate you had, and whatever you auditioned or applied keeps playing (no `loadPreset`; the
  tests assert that the engine build counter does not move). A refined candidate reaches the rig only when you AUDITION /
  APPLY one of its rows, or press **APPLY REFINED BEST** (it goes through the normal audition path: off-thread build, cross-fade).
- *Auto-promote.* If the **applied** quick candidate is the same chain as the thorough best, only the badge changes
  (PREVIEW to REFINED, "same chain as the refined best") and APPLY REFINED BEST is greyed out; nothing is loaded. "Same chain"
  is the pure function `sameChain(json, json)` in `MatchGlue.h`: the same structure (paths, the blocks in order with their
  types and slots, EQ bands, cab mode, flags), the same captures per slot (by TONE3000 `source` id when both have one, else by
  file name, ignoring the folder), every number whose key ends in `Db` (levels, input / output / EQ gains, thresholds) within
  **0.5 dB**, `blend` within 0.01, and every other number (frequencies, q, times, align samples) within a relative 1e-3. Names,
  notes, block ids and `playAlong` are ignored. When in doubt it says "different", which only costs a badge.
- *Cancel.* During the quick pass CANCEL cancels everything and no refinement starts. During the refinement the button reads
  CANCEL REFINE and cancels only the thorough job: the quick results stay. Starting a new MATCH (which is allowed while only the
  refinement runs), or choosing another take with USE FOR MATCH, cancels a running refinement (the old thorough process group is
  terminated on its own monitor thread and its folder is marked `cancelled`).
- *Job folder housekeeping.* On launch the processor asks the runner to prune, **on the runner's own thread** (never the
  message thread): the match job folders (`<jobs>/*-match`, quick and thorough) are grouped by take (the DI path in
  `job.json`), the groups ordered by their newest folder, and everything outside the **5 most recent takes' groups** is
  deleted. A group with a running job (its process alive and the recorded tool, or one of this runner's own jobs, or a job that
  has not spawned yet) is never pruned. Export jobs, `<jobs>/inputs/`, folders without a `job.json` and takes are never touched.
  Folders of phase 6a (no `request`) are grouped by the `--di` in their command line.
- *Top bar.* In Standalone the top-bar **MATCH** button opens the match screen directly. In plugin mode it opens the
  play-along panel (its record + match band) and shows the same "MATCH runs in the Standalone app: open the Standalone app."
  note as the panel's MATCH. EXPORT NAM in the top bar opens the export panel (phase 12).

**EXPORT NAM** moved to its own panel in phase 12 (see "NAM export (phase 12)" below); `MatchScreen` is MATCH only. The
export source (the auditioned / applied candidate's resolved file, else the current preset written to `<jobs>/inputs/`) and
the check that blocks a capture without a file are unchanged.

**Tests.** `plugin/tests/test_record_match.cpp` (headless; recorder: no allocation or lock while recording, WAV bit-exact for
mixed block sizes, overrun counting with a stalled writer and the silence padding, a take shorter than a writer pass, the
sidecar offset with a running StemPlayer in Standalone and host-follow modes, rename / delete, re-prepare; runner against a fake
child written at test time (`plugin/tests/fake_tools.h`, a Python script that answers `--help` with and without
`--progress-json`, writes progress.json, log lines, result.json and presets, and can wait on gate files): progress in both
modes, the command line, result list, cancel (SIGTERM and the SIGKILL escalation), failure message, missing tools, re-attach to
a running and a finished job, a re-attached cancel, a child that floods the pipe, export progress; settings persistence;
audition / A-B / apply; the plugin-mode gating) and the editor tests in `plugin/tests/test_editor.cpp` (the band, the
Standalone-only buttons, the missing-tool message, a job that survives the editor, and the screenshots
`build/screenshots/sawblade_record_armed_1x.png`, `sawblade_match_progress_1x.png`, `sawblade_match_results_1x.png`; the export screenshot moved with the export panel).
The 6a.1 tests add `[twopass]`, `[prune]` and `[topbar]` cases (the fake child understands
`--quick` / `--thorough`, can hide them from `--help`, writes different results per pass and can make the thorough best
differ by a level offset) and the screenshots `sawblade_match_preview_refining_1x.png` and `sawblade_match_refined_1x.png`.
All test data is synthesised into temp dirs; `SAWBLADE_DATA_DIR` keeps the default
locations out of the home folder.

**Known limits.** MATCH is Standalone-only (EXPORT NAM is not). The take list is rescanned when the recorder changes it and every 10 s while the panel is open. Takes and export job folders are never deleted by the plugin; match job folders are pruned as described above (5 most recent takes).

### NAM export (phase 12)

Spec `docs/specs/phase12_export_in_plugin.md`. `ExportPanel.{h,cpp}` (the skin; layout after `design/mockups/FullExport.dc.html`),
`ExportGlue.{h,cpp}` (what the panel shows and what it starts), `ExportSettings.{h,cpp}` (the saved settings), `Sha256.h`,
and the export half of `JobRunner`. EXPORT NAM opens from the top bar and from the play-along panel, in the Standalone app
and in a host alike (no Standalone gating). The job runs in the processor's `JobRunner`, so it survives the panel and the
editor closing; the audio thread never touches the panel, the runner or the sidecar (a test runs `processBlock` under the
allocation and lock guards while a fake export job runs and the panel's glue is polled on another thread).

**Panel.** Three views, driven by the export job's snapshot:
- *Configure*: the two mode cards of the mockup (NO-CAB + IR, WITH CAB) plus the STUDIO BLEND information card, SIZE
  (FEATHER / LITE / STANDARD, each with "last run: N min" or "no run yet" from `MatchSettings` `exportWallSeconds.<size>`),
  VALIDATION DI (LAST TAKE = the newest take, shown by name, else BUILT-IN SIGNAL), BUS COMP (only when the comp is on),
  OUTPUT FOLDER (default `<app data>/exports`, CHOOSE...), the rig summary (blocks per path, cab mode), "what goes into the
  model" (gate left out, the comp line depends on the comp and its release, time effects none), the credits (title - creator
  (licence) per capture; a NON-COMMERCIAL badge if any capture is `cc-by-nc*`; the file stem then gets `-nc`), the personal-use
  notice, TRAIN EXPORT, and RESUME when a cancelled run of this rig can continue (epoch N of M, mode, size).
- *Training*: stage, progress bar, `epoch N / M`, best ESR, elapsed / ETA, CANCEL.
- *Result*: MET (green) / NOT MET (red) / NOT JUDGED (amber), held-out ESR and DI LTAS error with their limits, the model
  path, the sidecar path, REVEAL (`File::revealToUser` on the `.nam`), OPEN FOLDER, A/B LISTEN (opens
  `listen/ab_original_then_export.mp3`, else `.wav`, with the system player; hidden when neither exists; there is no in-plugin
  playback), the licence note.

**Mode and comp rules.** A rig whose no-cab export is exact (shared cab, `irMix`, or no cab) defaults to NO CAB; per-path cabs
(studio blend) default to WITH CAB and the NO CAB card is disabled. A mode saved in the state is honoured only while it is exact
for the loaded rig. Comp on, NO CAB: DROP COMP (default, exact: the exported preset has `busComp.enabled = false`, since
`sawblade-export` refuses a no-cab export of a rig with the comp on) or KEEP COMP (`--allow-inexact`, the comp stays in the preset
and the error is reported). Comp on, WITH CAB: trained into the model if the release is <= 150 ms, else the panel shows the
refusal note (the exporter refuses). The gate is always left out by the exporter.

**Runner contract (`JobRunner::startExport`).** `sawblade-export <preset> --mode m --size s --device auto --require-accept
[--di <take wav> | --di builtin] [--allow-inexact] (--exports-root <root> | --resume <dir>) --progress-json <job>/progress.json`
(`--progress-json` only if `--help` lists it, probed once per executable like MATCH; without it the 4.1 checkpoint
`progress.json` is the fallback, and the run folder is found by scanning the exports root for the newest folder created after the
job started). `--out` is no longer passed: the exporter names its folder and reports it as `outDir` in the progress file.
Progress keys parsed into `JobProgress`: `stage` (plan / signal / render / train / validate / done / cancelled / error),
`fraction`, `etaSeconds`, `epoch`, `epochs`, `bestEsr`, `resumable`, `outDir`, `message`. The default exports root (no
`exportsRoot` in the request) is `<job>/export`.
- *Cancel* of an export job: SIGINT to the process group, SIGTERM after the grace period (default 15 s for exports), SIGKILL
  after another (MATCH is unchanged: SIGTERM, SIGKILL). Exit 130 or a cancelled job = `Cancelled`; the checkpoint stays.
- *Exit codes*: 0 with a result = `Succeeded`; 2 with `export_report.json` = `Succeeded`, `accepted = "NOT MET"`; 1 or no result =
  `Failed` with the exporter's `error:` / `refused:` line.
- *Result*: `export_report.json` -> `validation.acceptance.{status,summary,heldOutEsr,diLtasDb,esrLimit,ltasLimitDb}`,
  `training.namFile`, `nonCommercial`, `totalWallSeconds` (else `training.wallSeconds`, written to `exportWallSeconds.<size>`).

**job.json** (export jobs, besides the match fields): `sourcePreset`, `sourceSha256` (sha256 of the resolved preset file's
bytes: the "same rig" key), `exportsRoot`, `allowInexact`, `diBuiltin`, `outDir` (written as soon as it is known), and on
finish `accepted` (`"met"` / `"NOT MET"` / `"not judged"`), `resumable`, `sidecar`.

**Sidecar.** On `Succeeded` (met or not) the monitor thread copies the resolved preset file that was exported to
`<outDir>/<nam stem>.sawblade.json`, byte for byte. The preset written for an export is named by the hash of its bytes
(`<jobs>/inputs/<sha16>.preset.json`), so the same rig is always the same file and key.

**RESUME.** Offered when the newest export job is `Cancelled`, `outDir/checkpoint/progress.json` exists without `complete`, and
the key of what would be exported now (with the comp handled as that run handled it) equals the job's `sourceSha256`. It starts
`--resume <outDir>` with that run's mode and size (and validation take, if it still exists); the exporter's own identity check
is the final guard and its refusal text is shown.

**Plugin state.** An optional `"export"` object `{mode, size, diSource, compChoice, outputFolder}` beside `playAlong`
(`mode` empty = follow the rig; omitted when all defaults; a non-object value is dropped; unknown values keep the defaults;
nothing goes into the preset). The core preset parser accepts and ignores the key like `playAlong`
(`docs/PRESET_SCHEMA.md`).

**Tests.** Runner (`test_record_match.cpp`, fake exporter in `fake_tools.h` that follows this contract: progress shape, SIGINT
leaves `checkpoint/progress.json` + `last.ckpt` and records the signals it got in `signals.json`, exit 0 / 2 / 130 / 1, `.nam`
with a `metadata.sawblade` block, report, listening file): progress to the snapshot, the command line, cancel with SIGINT and the
escalation, resume, NOT MET / MET / not judged, the byte-identical sidecar, wall times, the glue (request, deterministic key,
RESUME lookup) and the allocation / lock guard. Editor (`test_editor.cpp`): opening from the top bar, mode defaults, comp row,
licences and NC badge, the state round trip, training / cancel / RESUME / result buttons, and the screenshots
`export_configure.png`, `export_training.png`, `export_result.png` (+ `export_result_not_met.png`) in `build/screenshots/`.

### Editor

A skinned prototype of the main rig screen (`design/mockups/RigReal.dc.html`, spec
`docs/specs/phase2_5_skin.md`); the final UI is the user's design. A fixed 1280 x 800 design laid out in one
content component that the editor scales with an `AffineTransform` (resizable, fixed 1.6 aspect, 640x400 to
2560x1600). It reads `status()` and the APVTS only. Layout: top bar (preset button opening the preset file chooser,
latency chip, LIVE / STUDIO chip, A/B compare, previous / next preset, MATCH (Standalone: opens the MATCH screen; plugin: opens the play-along panel's record + match area), EXPORT NAM (opens the export panel), rig area (amp heads, cab, pedalboard
with two pedals, footswitches and LEDs; click a piece to select it) and an inspector (BLEND, MASTER and POST EQ
knobs; all 12 parameters are bound to exactly one knob each outside the rig panel). The top bar also carries the gear button (title
"Settings", after PLAY ALONG) that toggles the Settings overlay (below). To make room for it the preset button is 170
design px wide (upstream 200) and the latency chip 136 (upstream 150). The window opens at 1280 x 800 times the `uiScale` setting
(0.5 to 2.0, default 1.0).

Pictures come from `plugin/assets/` (our own renders, embedded with `juce_add_binary_data`). Controls live in
`plugin/src/skin/`: `SkinAssets` (decodes the PNGs and JSON sidecars once), `FilmstripKnob`, `FootswitchButton`,
`LedIndicator`, `RigView`. Colours, fonts and plain-widget drawing are in `SawbladeLookAndFeel`. Placeholders
(disabled, titled, with a tooltip saying so): "+ PEDAL", CPU readout, the footswitch (visual bypass only).
A/B, previous / next preset, BROWSE CAPTURES, MATCH and EXPORT NAM are live (see Presets, Record + Match).

### Rig editor (phase 10)

The RIG button in the top bar (next to PLAY ALONG) opens an overlay of 940 x 742 design px exactly over the rig area
(the inspector stays visible). It makes every blend feature of the engine usable from the UI; open / closed and the
active tab are UI state and are never saved. Spec: `docs/specs/phase10_rig_editor.md`. Code: `plugin/src/rig/`.

- **Top strip.** Topology `SINGLE | SINGLE + 2 PEDALS | BLEND`, tabs `CHAIN | EQ | BLEND | CAB | GATE | COMP`, one status
  line (loading / error / first warning, or the last message of a failed ADD).
- **CHAIN.** Two lanes, `A . SAW` and `B . BODY`, of block cards in preset order (max 8): slot and type, title and
  credit from the capture metadata (`@creator . licence . VIA TONE3000`, `LOCAL FILE`), BYPASS, an INPUT knob (nam blocks,
  live), move left / right, remove, and `+ ADD`, whose menu lists `BlockRegistry::typeNames()` (a new block type, for
  example a modelled pedal, appears without UI changes; `nam` opens a `.nam` chooser, `eq` adds a flat band). A pedal is
  inserted in front of the path's amp. In the single topologies lane B shows only `BLEND OFF - choose BLEND above to use
  path B` (its blocks are kept).
- **EQ.** `A PRE | A POST | B PRE | B POST | POST`; the graph (`EqGraph.h` documents the pixel mapping) draws the combined
  analytic response; drag a node (frequency + gain, or Q for high / low-pass), shift-drag or wheel (Q), double-click a node
  (on / off), right-click (type, remove), double-click empty space (add a band), `+ BAND`.
- **BLEND.** Blend knob (`SAW 79 / BODY 21`), path levels with `M` / `S` (mute / solo, monitoring only), ALIGN
  `AUTO | MANUAL | OFF` with the resolved values, nudge `-10 -1 +1 +10`, `INVERT B`, `RE-MEASURE`. Phase 10.1 adds a `LINEAR | CONSTANT` blend-law toggle next to the blend knob (a live edit, no
  rebuild), a trim read-out under each path LEVEL knob (`+4.2 dB auto`, `+4.2 dB manual`, `0.0 dB off`, followed by ` · +1.0 dB` when
  the player's own level offset is not zero) and `MATCH LEVELS`.
- **CAB.** `SHARED | PER PATH`, `CAB ON`, IR cards with `CHOOSE...`; the notice `LIVE-COMPATIBLE: the no-cab NAM export is
  exact` (shared, or any cab mode with `CAB ON` off: a cab-less rig is live-compatible) or `STUDIO BLEND: only the with-cab
  NAM export is exact` (per path with the cab on).
- **GATE.** `GATE ON`, `GATE | EXPANDER`, THRESHOLD, HYSTERESIS, ATTACK, HOLD, RELEASE, RANGE, RATIO (expander only),
  KEY HPF (bottom = off), release curve, LEARN. **COMP.** `COMP ON`, THRESHOLD, RATIO, KNEE, ATTACK, RELEASE, MAKEUP; a
  release above 150 ms shows `release > 150 ms: not NAM-trainable`.

**How edits reach the engine.**

| edit | path |
|---|---|
| block add / remove / move / bypass, topology, band type / on-off / add / remove, cab mode / IR / on-off, align mode, gate and comp settings (except the threshold) | structural: `RigController::edit` -> `SawbladeProcessor::loadPreset` -> loader thread build (models and IRs come from the loader's persistent `CaptureCache`) -> 30 ms cross-fade. Knobs submit on drag end, wheel / typed values after a 150 ms debounce; a failed build keeps the previous preset and shows the error |
| EQ frequency / Q, gain of a pre / path EQ band or of a post band without a parameter slot, nam block input gain | live: `applyLiveEdit` -> `LiveSnapshot` (a second `SwapSlot`, published under the processor mutex, tagged with the engine generation) -> `Chain::setLiveParams` ramps (20 ms; gains in dB, frequency in log2, Q in log, on the absolute 32-sample redesign grid), no rebuild |
| blend, path levels, gate threshold, input / output gain, the six post EQ slot gains | host parameters (automatable), unchanged: the 12 parameters and their ids do not change |
| mute / solo | transient monitor state (`setMonitor`): ramps the path level to 0; cleared by user loads and state restores, kept by the editor's structural edits and by rebuilds |

A live edit made while a structural load is in flight can be overwritten when that load commits (EQ drags are absolute
and heal on the next mouse move). Everything the panel edits lives in the preset and round-trips through
`getStateInformation` / `setStateInformation`; transient and never saved: mute / solo, the open panel and tab, the
SINGLE + 2 PEDALS choice while the preset reads SINGLE, the remembered blend. The engine's alignment, latency and
`status().generation` are reported per block as before; re-measuring never runs on the audio thread.

**Topology rules** (`rig/RigModel.h`). A path's amp is its last `slot: "amp"` block, else its last `nam` block; every other block is a pedal slot.
`BLEND` iff path B is enabled; else `SINGLE + 2 PEDALS` iff path A has two or more non-bypassed pedal slots. Going to
SINGLE or SINGLE + 2 PEDALS disables B and sets `blend = 0` (the chain computes `(1-blend)*A + blend*B`, so a disabled B
at 0.5 would halve A); B's blocks are kept. Going to SINGLE bypasses the extra pedal slots of A (never deletes), going to
SINGLE + 2 PEDALS un-bypasses one. Going back to BLEND restores the remembered blend. In the single topologies the blend
knob is disabled in the UI, but the host may still automate `blend`: with B disabled the output is `(1-blend)*A`.

**Alignment.** `AUTO` presets stay `AUTO` across loads; `RE-MEASURE` builds the engine in auto mode off the audio thread and
writes `manual` values (`delaySamplesB`, `invertB`) back, without a further rebuild. Nudging or inverting while in `AUTO`
switches to `MANUAL` seeded with the measured values.

**Level match (phase 10.1).** The chain measures both paths with a guitar-shaped probe whenever it is built with both paths enabled
(docs/PRESET_SCHEMA.md "Blend"). `MATCH LEVELS` builds the engine with `levelMatch` auto off the audio thread and writes the measured
trims back as `levelMatch.mode = manual` without a further rebuild; a preset loaded in `auto` stays `auto` and the read-outs show the
resolved trims. Turning a LEVEL knob never changes the trims. Switching a rig that carries neither level-match key (both at their
defaults) to the BLEND topology sets `auto` + `constantLoudness`.

**Gate LEARN.** The audio thread pushes one input peak per block into a lock-free ring (`rig/InputMeter.h`, 512 blocks);
LEARN waits 1 s, takes the loudest peak since, and sets `gateThreshold = peak dB + input gain dB + 6 dB` (clamped to -80..-20; the gate sits after the input gain, the meter before it). The ring keeps only the last 512 blocks, so with 64-sample blocks at 48 kHz about 0.68 s is measured. The
inspector's LEARN GATE button does the same.

### Cab mic page (phase 9a)

Double-click the cab in the rig to open the mic page (an overlay over the rig and inspector; "< RIG" closes it; open /
closed is UI state and is never saved). Code: `plugin/src/mic/`.

- **IR pack:** one TONE3000 IR tone with many models (one IR per model), or a local folder of `.wav` files, or just the cab's
  current IR. `IrNameParser` reads each model name (speaker type and slot, mic model and type, distance, position, cab
  size; tolerant, unknown tokens are ignored). `IrPack` maps every model to a point on the open cab view using the sidecar
  (`assets/cab_*_open.json`: driver centres and cone radii in image pixels): the speaker slot picks the driver, the position
  gives the offset from the dust cap as a fraction of the cone radius (cap 0, cap edge 0.3, cone 0.6, edge 0.9, off axis
  0.6; a bare position number spreads over 0..0.9; unknown 0.3), towards the cab centre. Models at the same point are
  one dot. The 2x12 view is used when most models that name a cab size say 2x12.
- **Snap:** the mic follows the mouse; on mouse-up it snaps to the nearest dot (ties to the lower model) and loads the
  dot's model with the current mic and the nearest distance (else its first model). Nothing loads while dragging.
- **Loading** goes through `SawbladeProcessor::loadPreset` (the normal off-thread build and the 30 ms equal-power swap);
  `MicSession` builds the preset from `currentPreset()` with only the cab replaced. A/B toggles the last two chosen IRs
  of the active mic, NEXT POSITION goes to the next dot.
- **BLEND 2 MICS** switches the cab to `irMix` (`irA` = current IR, `irB` = the same IR, mix 0.5), adds a second draggable
  mic and the MIX fader (not a host parameter; at most one rebuild per 150 ms while dragging, one on release). BLEND off
  returns to `shared` with `irA`. An `irMix` preset opens with BLEND on. In `perPath` mode the page is read-only.
- **LOAD PACK** (shown when the cab's capture has a TONE3000 tone id) runs `sawblade-t3k pack <toneId> -o
  <appdata>/sawblade/packs/<toneId>.json --progress-json` (`presets/T3kTool`) on a background thread with a progress bar and
  CANCEL; the manifest is cached and found again by the cab's tone id on reopen. The executable is
  `t3kExecutable` in `<appdata>/sawblade/settings.json` (default `<repo>/match/.venv/bin/sawblade-t3k`; LOCATE... saves
  it). Exit code 4 means "not logged in" (run `sawblade-t3k login` in a terminal). `<appdata>` is
  `~/.local/share/sawblade` (Linux), `~/Library/Application Support/Sawblade` (macOS); `SAWBLADE_APPDATA` overrides it.

### Preset browser, factory presets, A/B compare (phase 9b)

Code: `plugin/src/presets/` (`PresetLibrary`, `PresetLoadFlow`, `AbCompare`, `PresetBrowser`, `PresetInfoPanel`, `T3kTool`).

- **Browser:** clicking the top-bar preset selector opens an overlay over the rig and inspector: search, banks (Factory: Classic /
  Styles / Matched; User), the categories present with counts, the preset list (double-click loads), and the info panel (name,
  category, bank, file, notes, and every capture of path A, path B and the cab with title, @creator, licence, TONE3000 URL, a
  NON-COMMERCIAL tag when the licence contains `nc`, or "local file: no attribution recorded"). LOAD FILE... keeps the file chooser.
  The footer has SAVE, SAVE AS, RENAME, DELETE (user presets only; delete moves the file to the OS trash) and the resolve status.
- **Banks:** factory presets are `presets/*.json`, `presets/styles/*.json`, `presets/matched/*.json` (key `factoryPresetDir` in
  `settings.json`, default `<repo>/presets`); user presets are `<appdata>/sawblade/presets/*.json`. `*.resolved.json` files are never
  listed; an unparseable file is listed greyed out with its error. Scanning runs on a background thread and parses without loading
  captures. An uncategorised preset shows as "Uncategorised" (in `matched/`: "Matched").
- **Search:** case-insensitive, whitespace-separated terms ANDed over name, category, notes and capture titles / creators.
- **Resolve on load:** a preset with a TONE3000 capture whose file is missing (and not in the capture cache) is resolved first with
  `sawblade-t3k resolve <preset> -o <appdata>/sawblade/resolved/<bank>/<stem>.resolved.json --progress-json`; progress shows in the
  footer ("Resolving 2/5: <title>") with CANCEL, exit 4 shows the not-logged-in message, a missing tool shows LOCATE.... The current
  sound is unchanged until the resolved preset has loaded. A resolved file that is newer than the preset and complete is used with
  no child process. The core loader also finds a TONE3000 capture in the capture cache (`$SAWBLADE_CACHE_DIR`, else
  `~/.cache/sawblade/captures/<id>/<modelId>.nam|.wav`) when its file is missing (docs/PRESET_SCHEMA.md).
- **Previous / next** (the top-bar < > buttons) step through the browser's filtered list, wrapping.
- **A/B compare:** the top-bar button shows A or B. Switching stores the current preset (with its parameter values) in the active
  slot, activates the other (an empty B starts as a copy) and loads it: one engine build and the 30 ms swap. A browser load replaces
  the active slot only. Right-click: Copy A to B, Copy B to A, Reset compare. Not saved in the plugin state.

### Settings, ToolRunner, first run, About (phase 11)

Spec: `docs/specs/phase11_settings.md`. Code: `plugin/src/settings/` (`Settings`, `ToolRunner`, `LoginFlow`,
`SettingsPanel`) and `plugin/src/about/` (`AboutBox`, `CaptureList`, `BuildInfo.h.in`). Nothing here runs on the audio
thread. `Settings` and `ToolRunner` are JUCE-free in their interfaces (`juce::File` and `juce::ChildProcess` are used
inside), so they are tested headless.

**Files.** Every location can be overridden by an environment variable, which is how the tests (and portable setups)
keep off the real home directory. `Env` (home, executable, source dir, `getenv`, `exists`, `isMac`) is injectable;
`Env::system()` reads `getenv` at call time.

| what | default | override |
|---|---|---|
| settings | macOS `~/Library/Application Support/Sawblade/settings.json`; Linux `$XDG_DATA_HOME/sawblade/settings.json`, else `~/.local/share/sawblade/settings.json` (the same file `presets/T3kTool` and phase 9 use; each store keeps the other's keys); `SAWBLADE_APPDATA` moves the whole app-data dir | `SAWBLADE_SETTINGS_FILE` |
| TONE3000 token file (the one `sawblade-t3k` writes) | `~/.config/sawblade/t3k_tokens.json` on all platforms | `SAWBLADE_T3K_TOKEN_FILE` |
| capture cache | `~/.cache/sawblade/captures` | `SAWBLADE_CACHE_DIR` |
| takes (recordings) | `<appDataDir()>/takes` (`defaultTakesDir()`; follows `SAWBLADE_APPDATA`, `SAWBLADE_DATA_DIR`, `XDG_DATA_HOME` like the settings file; a test asserts `Paths::takesDir` equals it). `TakeRecorder` resolves `Settings::effectiveTakesDir()` lazily (in `takesDir()` and `start()`, on the message thread; its constructor does not touch Settings), unless `setTakesDir()` set an explicit folder, so the panel's Takes folder applies to the next take | the `takesDir` key |

**One app-data dir, one settings file.** `appDataDir()` (`plugin/src/AppPaths.h`, the single definition) is, in order:
`SAWBLADE_APPDATA`, `SAWBLADE_DATA_DIR`, macOS `~/Library/Application Support/Sawblade`, else `$XDG_DATA_HOME/sawblade`
or `~/.local/share/sawblade`. `presets/T3kTool`'s `settingsFile()` / `packCacheDir()`, the take and job directories and
`Paths::settingsFile` (which additionally honours `SAWBLADE_SETTINGS_FILE` first) all follow it; a test checks that they
agree for both variables. The file is shared: phase 9's `t3kExecutable` and `factoryPresetDir` live next to our keys.
Both writers (`Settings` and `T3kTool::setT3kExecutable`) use a unique temp name (`.tmp.<pid>.<n>`) created 0600, and `T3kTool::settingsFile()` honours `SAWBLADE_SETTINGS_FILE` like `Paths::settingsFile`.
`Settings::save()` is read-modify-write: it re-reads the file, keeps every key it does not own as found on disk (so a
`T3kTool` write between our load and save survives), takes its own nine keys from memory (removals included) and writes
atomically. `T3kTool::setT3kExecutable` also leaves the file mode 0600.

**Cache dir coherence.** The core's `captureCacheRoot()` used to read only `SAWBLADE_CACHE_DIR`, while child tools get the
effective dir from `ToolRunner`. The shared `Settings` instance now calls `sawblade::setCaptureCacheRootOverride()`
(core, mutex-guarded, load-time only, never on the audio thread) whenever a stored `captureCacheDir` is loaded or set;
clearing it, or dropping the instance, clears the override. The process environment is never modified. Instances built
by tests with an injected `Env` never touch the override.

**Load errors.** `Settings::load()`'s text (malformed file, a dropped `t3k_cs_` key) is kept as `loadError()` and shown in
red under the Appearance section of the panel ("Settings file problem"); the next save overwrites a malformed file.

**URL safety.** The About rows and the login box's OPEN button launch only `http://` and `https://` URLs
(`about::isWebUrl`); anything else (`file:`, `javascript:`, custom schemes) is shown as text, and OPEN is disabled.

**Tool-path fallbacks.** `MatchSettings::defaultMatchExecutable()` / `defaultExportExecutable()`,
`BrowserSettings::defaultExecutable()` and `settings::defaultT3kExecutable()` return
`Settings::shared().toolPath("<tool>")` when `effectiveMatchVenvDir()` is set (stored or auto-detected), else their
compile-time `<repo>/match/.venv/bin/<tool>`. Explicit per-feature overrides those stores hold still win, so the
Settings panel's venv drives MATCH, EXPORT, the capture browser and the preset resolver.

**Keys** (`settings.json`, version 1; unknown keys survive a round trip; writes are atomic via `settings.json.tmp` +
rename, file mode 0600, directory 0700; a malformed file loads as defaults plus an error text and the next save
overwrites it):

| key | meaning |
|---|---|
| `matchVenvDir` | Python venv with `bin/sawblade-t3k` and `bin/sawblade-match`. Absent = auto-detect. A stored value wins even when invalid. |
| `captureCacheDir` | absent = `SAWBLADE_CACHE_DIR`, else the default above |
| `tone3000ClientId` | the publishable key (`t3k_pub_...`). A value containing `t3k_cs_` (any case) is refused by `setTone3000ClientId`, never stored, written or logged; `load()` also drops one found in a file. Anything not starting with `t3k_pub_` is accepted with a warning. Effective value: stored, else the `TONE3000_CLIENT_ID` environment variable (never a `t3k_cs_` value). |
| `separationModel` | `htdemucs_6s` (default), `htdemucs`, `htdemucs_ft` |
| `takesDir` | absent = the platform default above |
| `theme` | only `"dark"` exists |
| `uiScale` | 0.5 to 2.0 (clamped), initial editor size = 1280 x 800 x `uiScale` |
| `firstRunCompleted` | set by DONE / closing the panel on a first run |

**Match venv auto-detect** (`detectMatchVenv`; a candidate is valid when `<dir>/bin/sawblade-t3k` exists; first valid
wins): 1. `SAWBLADE_MATCH_VENV`; 2. the build tree: `<SAWBLADE_SOURCE_DIR>/match/.venv` when the running binary lies
inside the source checkout (`SAWBLADE_SOURCE_DIR` is a compile definition); 3. `~/sawblade/match/.venv` (the
`docs/MAC.md` layout); 4. none.

**`ToolRunner`** is the one way the plugin runs a `sawblade-*` tool. Usage (the capture browser, presets and
record+match sessions adopt it: resolve the tool, build a `ToolRequest`, stream lines, read `result.json`):

```cpp
using namespace sawblade::plugin::settings;
ToolRunner runner(Settings::shared());            // member of your panel; its destructor cancels and joins the jobs
ToolRequest req;
req.tool = "sawblade-t3k";                        // resolved: <venv>/bin/<tool>, else PATH, else StartFailed
req.args = {"whoami", "--json"};
req.timeout = std::chrono::seconds(30);           // 0 = none
auto job = runner.run(req,
    [](const std::string& line) { /* every complete line, stdout+stderr merged, redacted */ },
    [](const ToolResult& r) {                     // exactly once, after the last line
      if (r.outcome == ToolResult::Outcome::Ok && r.json) { /* (*r.json)["username"] ... */ }
      else { /* r.error (StartFailed/TimedOut) or r.lines.back() */ }
    });
// job->cancel();  job->wait(ms);  job->result();
```

Hardening: redaction is a manual case-insensitive scan (safe on multi-megabyte token lines); an executable path containing
`=` is refused with a `StartFailed` message (the `/usr/bin/env` prefix would read it as an assignment); `cancel()` and the
timeout watchdog never signal a child that has already been waited for (`reaped_`). Callbacks are posted to the message thread (`juce::MessageManager::callAsync`) and dropped if the runner is gone;
`callbacksOnMessageThread = false` runs them on the worker thread (headless tests). The environment: JUCE's
`ChildProcess` takes no environment table, so on POSIX the command is `/usr/bin/env KEY=VALUE ... exe args`, with
`PYTHONUNBUFFERED=1`, `TONE3000_CLIENT_ID` (effective id, when non-empty) and `SAWBLADE_CACHE_DIR` (effective cache
dir) added, then `request.env` (which wins). **Redaction:** every `t3k_cs_[A-Za-z0-9_-]+` in an output line becomes
`t3k_cs_[redacted]` before the line is stored or delivered. **`result.json`** is the last line that parses as a JSON
object or array, else the whole text if it parses, else empty. Limits of `juce::ChildProcess`: arguments that are the
empty string are dropped; a child killed by a signal it did not cause reads as exit 0 (JUCE reports 0); cancelling kills
only the direct child (a tool that spawns grandchildren which keep the pipe open delays the end of the job until they
exit; the `sawblade-*` entry points exec in place).

**Login protocol** (`sawblade-t3k login --json`, an alias of `--json-events`, one JSON object per line; the refresh
token is never printed in this mode): `{"event":"device_code","user_code","verification_uri","verification_uri_complete"|null,"expires_in"}`,
then `{"event":"logged_in", ...}` where `username`, `display_name`, `id` and `token_file` are optional; failures are a
line `{"error":"<msg>","code":"auth|network|..."}` (the CLI's shape; `{"event":"error","message"}` is accepted too) and a
non-zero exit, and exit code 4 means "not logged in". `LoginFlow` is the state machine the panel drives (other lines are
ignored). `whoami --json` prints one `{"id","username","display_name","token_file"}` line, or the `{"error","code"}` line
with exit 1 (4 when not logged in). The capture browser's `T3kClient`/`T3kJson` parsers ignore the extra keys (tested).

**Settings panel** (`SettingsPanel`): an overlay over the rig area (940 x 742 design px), closed by default, opened by the
gear button, closed by Esc, the x button or DONE (UI state, never saved). Sections: Setup checklist, Tools (match venv
path, Browse, Auto, status light, Test = `sawblade-t3k --help`), TONE3000 (client id with the secret-key refusal shown in
red, token-file status, Test = `whoami --json`, Log in with the device code box: code in a 40 px mono font, COPY CODE,
COPY URL, OPEN, CANCEL, countdown, RETRY on failure), Captures (cache folder, count of `*.nam` / `*.wav` counted on a
background thread, Open folder), Separation, Recording (takes folder), Appearance (theme, UI scale; applies when the
window is next opened), and a footer with the settings file path and **About Sawblade...**.

**First run.** `Settings::isFirstRun()` is true when no settings file existed at the instance's first `load()`. The
editor constructor then opens the panel with the checklist expanded, once per process (a process-wide atomic, so a DAW
with 12 instances shows it once). Closing the panel or DONE calls `markFirstRunCompleted()`, which writes the file, so
it never shows again. Checklist rows (light, text, fix button): **Tools found** (both executables exist; LOCATE...),
**Logged in** (token file present, or the last whoami result; LOG IN), **Captures cached** (every TONE3000 capture of the
current preset has an existing `resolvedPath`; FETCH CAPTURES writes the preset to `<cache>/_resolve/<name>.json`, runs
`sawblade-t3k resolve ... -o <name>.resolved.json` through `ToolRunner` and loads the resolved preset). It re-evaluates
when the panel opens, after each fix job and on the editor's 4 Hz tick while visible. Note that the processor keeps the
previous preset when a load fails, so this row sees a preset that references missing captures when a host restores a
session (state restore commits the preset before it builds) or before `prepareToPlay`, not after a failed
`loadPresetFile`.

**About box** (`AboutBox::show`, an overlay over the whole editor): the icon (`icon_256.png` embedded), `Sawblade
<version> · <short sha> · clean|dirty · built <date>` (`BuildInfo.h` is regenerated on every build by the always-run
`SawbladeBuildInfo` target running `cmake/GenBuildInfo.cmake`, written only when its content changes: `PROJECT_VERSION`,
`git rev-parse --short HEAD`, `dirty` when tracked files changed, `unknown` without git, a UTC date; the Standalone window
title is `Sawblade - <version> · <short sha> · clean|dirty`), the licence
note (JUCE 8 under the AGPLv3, personal non-commercial use, not sold), the captures of the current preset (slot, title,
creator, licence as stored, a clickable TONE3000 link, a dot for on disk / missing, the tag "non-commercial" for any
`-nc` licence; captures without a `source` say "local file, no TONE3000 metadata") and `docs/THIRD_PARTY.md` in a
read-only scrollable text box (embedded with `juce_add_binary_data`). CLOSE or Esc closes it.

**App icon.** `design/render/app_icon.py` (Pillow + numpy, deterministic) renders `plugin/assets/icon/icon_{16..1024}.png`:
the 16-tooth saw blade of `pedal_b2.py` in saw orange with bone ring highlights on the dark ground, in the macOS rounded
square with a 10 % transparent margin. `--check` re-renders to a temp dir and compares the decoded pixels with the
committed files (ctest `plugin: app icon is reproducible`; skipped with exit 77 if Pillow is missing). CMake passes
`icon_1024.png` / `icon_256.png` to `juce_add_plugin` as `ICON_BIG` / `ICON_SMALL`.

## Tests

`sawblade_editor_tests` (`plugin/tests/test_editor.cpp`, ctest prefix `editor: `, run under `xvfb-run -a` when
available; the rig editor tests write `build/screenshots/rig_{single,blend,eq,gate}.png` plus `rig_tab_*.png`): snapshots to `build/screenshots/sawblade_skin_{1x,2x}.png`, resizing, parameter bindings, filmstrip
mapping, knob interaction, footswitch / LED, accessibility, and the `plugin/assets/` budget (< 25 MB). The play-along
tests cover the panel (exists, closed by default, opens from the top bar), its controls bound to the processor, folder
drop, the missing-folder message, and screenshots `build/screenshots/sawblade_playalong_{closed,open}_1x.png`.
`plugin/tests/test_mic_page.cpp` covers the mic page (open from the rig, dots, drag and snap, fields and combo boxes, BLEND
and the MIX fader, read-only studio mode, LOAD IR FOLDER, LOAD PACK through a fake `sawblade-t3k`) and the screenshots
`build/screenshots/sawblade_micpage_{rig,single,blend}_1x.png`.
`plugin/tests/test_presets.cpp` (library, user operations, A/B, resolve flow with fake executables, info panel) and
`plugin/tests/test_preset_browser.cpp` (the browser, stepping, the A/B button, screenshot `sawblade_browser_1x.png`) cover phase 9b. The Record + Match tests are described in that section.
The phase 11
editor tests (each points `SAWBLADE_SETTINGS_FILE` at a temp file) cover the gear button, the first-run checklist, the
secret-key refusal, the login view against a fake `sawblade-t3k` (`plugin/tests/tools/fake_t3k.sh`, `FAKE_T3K_MODE`
selects approve / error line / exit 4), the panel raising above the RIG overlay, the checklist lights,
the About box and the icon PNGs, with screenshots `sawblade_{settings,firstrun,login,about}_1x.png` in the screenshot
directory.

`sawblade_plugin_tests` (headless): `plugin/tests/test_rig_model.cpp` and `plugin/tests/test_rig_controller.cpp` (rig
model, controller, live edits, mutes, re-measure, blend automation, concurrent-edit RT test, LEARN),
`plugin/tests/test_mic.cpp` (IR name parser, pack and dot layout, snapping, choosing
models through the processor, the `sawblade-t3k` runner and settings, with fake executables), `plugin/tests/test_engine.cpp` (Engine, no JUCE),
`plugin/tests/test_processor.cpp` (the processor driven like a host) and `plugin/tests/test_settings.cpp` (the settings store, auto-detect order, secret-key rule, `ToolRunner` against the fake scripts in `plugin/tests/tools/`, `LoginFlow`) and `plugin/tests/test_playalong.cpp` (the backing:
level rule, queue, Standalone and host-follow transport, rig-latency alignment, offset, state, zero allocations and
locks with the backing playing; stems are synthesised into a temp dir, no audio is committed). Audio-thread rules are
enforced with the existing `AllocGuard` plus `LockGuard` (`plugin/tests/lock_guard.cpp`, counts
`pthread_mutex_lock`/`trylock`/rwlock via linker `--wrap`; Linux only, skipped elsewhere). Core
additions are tested in `tests/test_rt_resample.cpp` and `tests/test_chain_live.cpp`.

### Song files: on-device separation (phase 5.1b)

LOAD SONG (picker and drag-and-drop) accepts an audio file as well as a stems folder. A file (mp3, wav, flac; m4a / aac /
aiff / ogg through JUCE's `AudioFormatManager`, i.e. CoreAudio on macOS) is separated by the htdemucs ONNX model
(ONNX Runtime CPU EP, `core/src/separator.cpp`) on a **separation thread** (`PlayAlong`, started on the first song file;
never the audio or message thread), written to the stem cache, and the cache directory then goes through the unchanged 5.2
loader. The panel shows progress with an ETA and a CANCEL button; the second load of the same file (same bytes, same
model) is a cache hit and takes a hash of the file. Cancel (CANCEL, a newer request, or destroying the processor) stops
within about one network evaluation (`RunOptions::SetTerminate`), leaves nothing in the cache, and goes back to the
previous song. Closing the editor does not cancel: the job belongs to the processor and finishes in the background.
Host-friendly: the plugin separates with max(1, cores/2) threads on a lowered-priority thread (the CLI uses cores - 1). A
state restore only uses stems that are already cached; with a cache miss the panel says "Song not separated yet - LOAD SONG
to separate it" and nothing is separated until the user loads the song. Separation streams the stems into the cache as
they become final, so peak memory does not grow with the song's length (ONNX Runtime's working set dominates).

The 6/4-stem toggle beside the song name picks the model (6-stem `htdemucs_6s`, default, has a guitar stem; 4-stem
`htdemucs`: `other` is treated as the guitar as in 5.2). With a 6-stem model piano is summed into `other`.

Model files are never bundled. They live in `$SAWBLADE_MODELS_DIR`, else `~/Library/Application Support/Sawblade/models/`
(macOS) / `$XDG_DATA_HOME/sawblade/models/` or `~/.local/share/sawblade/models/`, as `<id>-core-opset17.onnx` plus a
`.sha256` sidecar, and are fetched with `match/.venv/bin/sawblade-models fetch --model htdemucs_6s` (run from the repository
root). If the model is missing the panel says so with that command; nothing crashes. The stem cache is
`$SAWBLADE_STEMS_DIR`, else the models dir's sibling `stems/` (32-bit float WAV, one directory per
`sha256(file)-<model>`).

Build: `SAWBLADE_WITH_SEPARATOR` (default ON with `SAWBLADE_BUILD_PLUGIN`, else OFF) fetches the pinned ONNX Runtime 1.30.0
and copies its shared library (plus its LICENSE and ThirdPartyNotices.txt) next to the plugin binary (`$ORIGIN`; `Contents/Frameworks` + `@loader_path/../Frameworks`
on macOS). With it OFF the plugin shows "Separation is not available in this build." for a song file. CLI: `tonerender
--separate SONG --stems-out DIR` and `sawblade-stems SONG DIR [--model htdemucs_6s|htdemucs] [--threads N]`.
