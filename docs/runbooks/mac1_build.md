# Mac 1: plugin build + test (runs on a loop)

Each run of this loop:

1. `git fetch origin && git checkout claude/sawblade-mac-build 2>/dev/null || git checkout -b claude/sawblade-mac-build origin/claude/sawblade-plugin-setup-7k0b8q`,
   then `git merge --no-edit origin/claude/sawblade-plugin-setup-7k0b8q`. Merge only, never
   rebase or force-push.
2. If the main branch has new commits since the last build recorded in the report (or there
   is no report yet):
   - Configure and build: `cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAWBLADE_BUILD_PLUGIN=ON`,
     then build.
   - Run `ctest --test-dir build-mac --output-on-failure`.
   - Copy the AU to `~/Library/Audio/Plug-Ins/Components` and the VST3 to
     `~/Library/Audio/Plug-Ins/VST3`.
   - Run `pluginval` at level 10 on both (install it from the Tracktion pluginval GitHub
     releases into `~/tools` if missing), then `auval -v` on the AU.
   - Fix **macOS build/packaging problems only**: use the dsp-engineer subagent and get
     reviewer ACCEPT. No DSP changes. Commit on `claude/sawblade-mac-build` with these
     trailer lines:
     `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
     `Claude-Session: https://claude.ai/code/session_01TkHt6WUmRQEkryYfArnFjL`
3. Write `docs/reports/mac_build_REPORT.md` covering:
   - the main-branch commit built
   - the macOS and Xcode versions and the chip
   - the ctest summary
   - the pluginval and auval results
   - the fixes made
   - how to open the Standalone app and which preset to load

   Commit it and `git push -u origin claude/sawblade-mac-build`.
4. If nothing changed since the last run, do nothing and say "no change" in one line.
5. If you're blocked on something only the user can do (a password, a permission dialog,
   a hardware choice), say so in one short line at the top of the report and push it. The
   lead relays it to the user.
