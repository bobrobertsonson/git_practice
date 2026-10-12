# Running RASP work on your own Mac

A cloud session can't see your disk. A session on your Mac can read the DIs,
stems and mixes where they already are, and it can also build the plugin straight
into Logic. Remote Control keeps it on your phone.

## One-time setup

```sh
xcode-select --install                     # compilers (skip if already installed)
brew install cmake ninja ffmpeg python     # Homebrew: https://brew.sh
pip3 install numpy
git clone https://github.com/bobrobertsonson/git_practice.git
cd git_practice
git checkout claude/vibrant-keller-4y1kh9
make -C rasp                               # desktop tools: rasp_wav, live_test, presets_dump
```

Then start Claude Code in that folder. Use the Claude desktop app (open the
folder), or `claude remote-control` in a terminal there, which also appears in
the Claude app on your phone.

## Tell it where your audio is

Nothing needs copying. Point the tools at your folders, for example:

```sh
python3 rasp/tools/takes.py classify ~/Music/NTM_Vocals/**/*.wav
python3 rasp/tools/mixprint.py --mix "~/Music/NTM_Vocals/Song/final.mp3" ~/Music/NTM_Vocals/Song/*VOX*.wav
python3 rasp/tools/presetfit.py --mix final.mp3 --lead lead.wav other_stems/*.wav
```

`.gitignore` keeps every audio and zip file out of git, so stems can't be
pushed by accident, even if they're inside the repo folder.

## Build the plugin straight into Logic (no GitHub download)

```sh
cmake -S rasp/plugin -B rasp/plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRASP_PLUGIN_TESTS=ON
cmake --build rasp/plugin/build
rasp/plugin/build/plugin_test
cp -R rasp/plugin/build/RASP_artefacts/Release/AU/RASP.component ~/Library/Audio/Plug-Ins/Components/
auval -v aufx Rsp1 RspA                    # Apple's check, same as Logic's
```

A locally built plugin isn't quarantined, so `xattr` isn't needed. Restart Logic
(or rescan in Plug-in Manager) after copying.

## Where things stand (for the new session)

- **Engine:** `rasp/dsp/rasp.h`. **Presets:** `rasp/dsp/presets.h`, mirrored in
  `rasp/playground/index.html` (`make -C rasp check` keeps them equal).
- **Momentary effects:** Mega (KnockedLoose-style burst) and Throw.
- **Reference tools:**
  - `tools/mixprint.py`: a producer's EQ, pan and space per stem, from the stems plus the mix.
  - `tools/presetfit.py`: which preset or tuning comes closest to that producer
    (`--try` for candidates).
  - `tools/takes.py classify`: guesses a take's style.
- **Findings so far** (Bloodbath, Gojira, Immortal Disfigurement, KnockedLoose):
  - Every producer kept more top end than RASP's "huge".
  - "dark" fit worst every time.
  - Immortal Disfigurement lifts 6–10 kHz by about 13 dB.
  - Meshuggah's mix gave no usable fingerprint: the vocal explains under 4% of it.
- **Next:**
  - Retune "huge" and add an "air" preset using `presetfit.py`.
  - Stop `classify` recommending "dark".
  - Run the fit over every session in the NTM_Vocals folder, not just the four uploaded.
