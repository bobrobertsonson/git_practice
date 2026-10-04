# Sawblade on the Mac

Apple Silicon, Homebrew, Command Line Tools only (no Xcode needed).

## First time

```
xcode-select --install            # Command Line Tools, if you do not have them
brew install cmake ninja python@3.12 git
git clone <your Sawblade repo URL> ~/sawblade
cd ~/sawblade
git checkout claude/sawblade-plugin-setup-7k0b8q

# TONE3000 (needed only to resolve presets; see match/README.md)
python3.12 -m venv match/.venv
match/.venv/bin/pip install -e match
export TONE3000_CLIENT_ID=t3k_pub_xxxxxxxx       # publishable key, never the t3k_cs_ secret
match/.venv/bin/sawblade-t3k login               # prints a code; open the URL, sign in, enter it
```

Put the `export` line in `~/.zshrc` so it persists. Then run the update script once (below);
the first run configures and builds everything and takes a while.

## Every time

```
~/sawblade/scripts/mac_update.sh
```

It pulls (fast-forward only), builds `build-mac/` (Release), copies the AU and VST3 into
`~/Library/Audio/Plug-Ins/`, runs `auval -v aufx Swb1 Swbl`, resolves any presets that need
TONE3000 captures, and prints what changed. Restart your DAW afterwards.

Options: `--no-resolve` (skip the TONE3000 step), `--clean` (wipe `build-mac/` first),
`--standalone` (open the Standalone app at the end), `--dry-run` (print commands only).

The Standalone app is at
`build-mac/plugin/SawbladePlugin_artefacts/Release/Standalone/Sawblade.app`
(Options > Audio/MIDI Settings to pick your interface).

If a pull is refused (local changes or diverged history) the script stops; fix that first.
Resolved presets (`*.resolved.json`) and `.mac_update_last` are git-ignored.
