# Building AxeElevator

## Engine, tools, tests (any OS with a C++17 compiler)

```sh
cd axe
make            # axe_wav, synth_di, engine_test
make check      # engine unit tests
tools/render_all.sh            # synthetic DIs through every mode (needs python3 + numpy + ffmpeg)
tools/render_all.sh samples/   # your own DIs
```

## Logic (macOS): AU, VST3, Standalone

```sh
cmake -S axe/plugin -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release -j 8
cp -R build/AxeElevator_artefacts/Release/AU/AxeElevator.component ~/Library/Audio/Plug-Ins/Components/
auval -v aufx AxEv Snch
```

Or download the `AxeElevator-macOS` artifact from the **AxeElevator plugin**
workflow run on GitHub (Actions tab). It's built, signed ad-hoc and validated
with `auval` in CI. A downloaded, unsigned component may need
`xattr -dr com.apple.quarantine AxeElevator.component`.

## Linux desktop: LV2 (+ VST3)

```sh
sudo apt install ninja-build libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxext-dev liblilv-dev lv2-dev
cmake -S axe/plugin -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAXE_PLUGIN_TESTS=ON
cmake --build build
./build/plugin_test
make -C axe lv2check LV2_BUNDLE_DIR=$PWD/build/AxeElevator_artefacts/Release/LV2
cp -r build/AxeElevator_artefacts/Release/LV2/AxeElevator.lv2 ~/.lv2/
```

Use an LV2 host that provides the `nominalBlockLength` option, such as
Carla, Ardour or mod-host. Darkglass's wrapper requires it, and `jalv` /
`lv2apply` on Ubuntu 24.04 don't provide it. `axe/lv2_host run
urn:snotch:axeelevator in.wav out.wav mode=3 gain=80` processes a file
through the LV2 itself.

## Darkglass Anagram (ARMv8.2 64-bit)

The Anagram can't load custom plugins without Darkglass's developer access.
To build:

**On GitHub:** run the **AxeElevator Anagram LV2** workflow (Actions tab →
Run workflow). It bootstraps Darkglass's plugin-builder toolchain (GCC 9.4,
glibc 2.34, `-mcpu=cortex-a76.cortex-a55`). The first run takes ~1 h, then
the toolchain is cached. The artifact is `AxeElevator.lv2`.

**Locally, the Docker route** (Plugin-Dev-Setup's BUILDING-DOCKER.md):

```sh
git clone https://github.com/Darkglass-Electronics/Plugin-Dev-Setup
docker buildx build --tag darkglass-anagram Plugin-Dev-Setup/docker      # ~1 h, ~15 GB
docker run -ti --name axe-anagram -v $PWD:/root/source darkglass-anagram
# inside the container:
cmake -S /root/source/axe/plugin -B build-anagram -DCMAKE_BUILD_TYPE=Release -DAXE_FORMATS=LV2
$(which cmake) --build build-anagram
```

(Inside the container `cmake` is an alias that adds Darkglass's toolchain
file. `$(which cmake)` is needed for `--build`, as their docs say.)

**Deploy** (Anagram in Developer Mode):

```sh
scp -O -r build-anagram/AxeElevator_artefacts/Release/LV2/AxeElevator.lv2 root@192.168.51.1:/root/.lv2/
ssh root@192.168.51.1 "systemctl restart jack2 lvgl-app"
```

## Other aarch64 (Raspberry Pi, MOD-like boxes)

The **linux-arm64** CI job builds the LV2 for generic `armv8-a` and for
`armv8.2-a` in Debian 11, so it needs glibc ≥ 2.31. It links libstdc++
statically and needs freetype and fontconfig on the target. The MOD Dwarf
(glibc 2.27) needs MOD's mod-plugin-builder toolchain instead: see README,
"MOD Dwarf".

## Identifiers (placeholders until release)

| | |
|---|---|
| Product / company | AxeElevator / Snotch |
| AU manufacturer / plugin code | `Snch` / `AxEv` |
| Bundle ID | `com.snotch.axeelevator` |
| LV2 URI | `urn:snotch:axeelevator` |
| Anagram abbreviation | `AXE` |

Change them before the first public release, never after: hosts key saved
sessions on them. In `plugin/CMakeLists.txt` the JUCE keyword is `LV2URI`
(`LV2_URI` is silently ignored).
