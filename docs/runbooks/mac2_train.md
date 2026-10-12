# Mac 2: NAM export training on Apple GPU (runs on a loop)

One-time setup (skip any step that is already done):
- Build the core Python module (`-DSAWBLADE_BUILD_PYTHON=ON`) and `tonerender`.
- Create the match venv with the `export` extra; `torch.backends.mps.is_available()` must
  be True.
- Log in with `sawblade-t3k` (device flow). Put the code and URL at the top of the report
  and push it, so the lead can relay it to the user.

Each run of this loop:

1. `git fetch origin`. Check out `claude/sawblade-mac-train` (create it from
   `origin/claude/sawblade-plugin-setup-7k0b8q` if it doesn't exist), then
   `git merge --no-edit origin/claude/sawblade-plugin-setup-7k0b8q`. Merge only.
2. Work queue: every `presets/matched/*.json` that has no entry yet in
   `docs/reports/mac_train_REPORT.md`. Process one per loop run, oldest first. For each:
   - Resolve and download its captures (`sawblade-t3k resolve` / `pull`).
   - Run `sawblade-export --mode nocab --size standard --device mps --require-accept`,
     then the same with `--size lite`.
     - If the preset is a studio blend (per-path cabs), use `--mode withcab` instead.
     - On a NOT MET result, retry once with double the epochs, then record it as it
       stands.
3. Append to `docs/reports/mac_train_REPORT.md`:
   - preset file
   - git commit
   - chip, torch version and device
   - training wall time per size
   - held-out ESR and LTAS, DI-excerpt ESR and LTAS, acceptance status
   - local paths of the .nam/IR files and the A/B mp3

   **Never commit .nam, IR, audio or captures** (CLAUDE.md licensing). Commit the report
   and `git push -u origin claude/sawblade-mac-train`, with these trailer lines:
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
   `Claude-Session: https://claude.ai/code/session_01TkHt6WUmRQEkryYfArnFjL`
4. If the queue is empty, say "queue empty" in one line and stop.
5. If you're blocked on the user, put one short line at the top of the report and push it.
