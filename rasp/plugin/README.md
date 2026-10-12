# RASP plugin: Logic Pro (AU), VST3 and standalone

The same DSP engine as the pedal (`../dsp/rasp.h`), wrapped with
[JUCE 8](https://juce.com) as an effect plugin. The panel follows the pedal:
Control / Thicken / Flavour / Bus+Live, with the seven factory presets.

## Get it

Each push to `rasp/` runs the **RASP plugin** GitHub Actions workflow. Its macOS
job builds a universal (Apple silicon + Intel) Audio Unit, checks it with
Apple's `auval` (the same validation Logic runs), and uploads **RASP-macOS**
(`RASP-AU.zip`, `RASP-VST3.zip`, `RASP-Standalone.zip`). Download it from the
run's page under *Artifacts*.

## Install in Logic Pro

1. Unzip `RASP-AU.zip` to get `RASP.component`.
2. The build is ad-hoc signed, not notarized, so clear the download quarantine
   (Terminal):
   ```sh
   xattr -dr com.apple.quarantine ~/Downloads/RASP.component
   ```
3. Copy it to `~/Library/Audio/Plug-Ins/Components/`.
4. Open Logic. If it doesn't appear, open **Logic Pro → Settings → Plug-in Manager**,
   select RASP and click **Reset & Rescan Selection**.
5. On a vocal track: **Audio FX → Audio Units → RASP → RASP**. Choose
   **Mono → Stereo** on a mono vocal track so the doubles and reverb are stereo.

## How it behaves in Logic

| | |
|---|---|
| Presets | Huge, Natural, Dark, Bright, Live, Raw, Air, in Logic's preset menu and the plugin's own. Input gain, tempo and the Live switches stay put when you change presets |
| Bypass | Logic's bypass button uses the engine's level-matched, click-free bypass, so A/B is fair |
| Tempo | Delay follows the project tempo (turn **Sync to host** off to set it by hand) |
| Channels | Mono → Stereo (recommended), Stereo → Stereo (input summed to mono, like a mic) or Mono → Mono (comb taps drop out automatically) |
| Sample rates | 44.1 – 192 kHz |
| Latency | None reported: there's no lookahead; the drive adds only ~0.15 ms of filter phase delay |
| Mega / Throw | Hold the **MEGA** / **THROW** buttons, or draw them on automation lanes. Both are "on while held" and survive preset changes |
| Feedback guard | Off by default in the plugin (it's for a live mic near speakers). Turn it on when tracking through monitors |
| Input gain | Set it so screams peak around −6 dBFS, the same as the pedal's preamp knob |

## Build it yourself

macOS needs Xcode command-line tools and CMake ≥ 3.22; Linux needs the JUCE
dependencies listed in the workflow.

```sh
cmake -S rasp/plugin -B rasp/plugin/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DRASP_PLUGIN_TESTS=ON
cmake --build rasp/plugin/build -j
rasp/plugin/build/plugin_test            # headless checks
```

`plugin_test` checks that:

- the plugin matches the bare engine to −100 dB
- any block size gives identical output
- 44.1–192 kHz runs at the same loudness and delay timing
- saved state reproduces the sound exactly
- mono and stereo layouts are correct
- the bypass is the level-matched one

## Licence note

JUCE is dual-licensed: AGPLv3, or a commercial licence (free tier for small
revenue). That's fine for your own use. Selling a closed-source RASP plugin
needs a JUCE licence.
