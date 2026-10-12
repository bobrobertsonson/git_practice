# v0.3.0.1 hotfix — TONE3000 calls fail inside the DAW

Source: user's v0.3 hand test (2026-10-07, Logic, macOS): capture swap / capture browser "can't connect to
TONE3000"; plugin login shows "login did not complete (exit status 1)". Settings shows the client id set
(t3k_pub_...) and the token file present. Terminal `sawblade-t3k whoami` works; under
`env -i HOME=$HOME PATH=/usr/bin:/bin` it fails: `TONE3000_CLIENT_ID is not set`.

Lead diagnosis: two launch paths. `plugin/src/settings/ToolRunner.cpp` (Settings panel) injects
`TONE3000_CLIENT_ID` (from `Settings::effectiveTone3000ClientId()`) and `SAWBLADE_CACHE_DIR` via `/usr/bin/env`;
`plugin/src/presets/T3kTool.cpp` `runBlocking` (capture browser, body fill, preset resolve/fetch, ladders, mic
page...) starts the tool with the host's environment only. A DAW launched from the Dock has no shell exports, so
every T3kTool call fails. Separately, the CLI requires the client id even for commands that only use a stored token.

## Task A (dsp-engineer) — one environment for every tool launch

- Every plugin launch of `sawblade-t3k` / `sawblade-match` / `sawblade-export` gets the same injected environment
  as ToolRunner (client id when known, `SAWBLADE_CACHE_DIR`, `PYTHONUNBUFFERED`). One shared helper used by
  ToolRunner, T3kTool and JobRunner (check JobRunner's existing env handling, `extern char** environ`); no second
  copy of the rule. Never inject a `t3k_cs_` value.
- Errors: when the tool exits non-zero, the user-facing message includes the tool's last stderr line (e.g. the
  "TONE3000_CLIENT_ID is not set" text), not only "exit status 1". Applies to the login screen too.
- Tests: a fake tool (existing `plugin/tests/fake_t3k.py` pattern) that fails unless `TONE3000_CLIENT_ID` is in
  its environment; run with the test process's own env scrubbed of it; every T3kTool entry point used by the
  capture browser / body fill / preset fetch succeeds when Settings holds the id, and the failure message carries
  the stderr line when it doesn't.

## Task B (match-engineer) — the CLI needs the client id only to log in

- `sawblade-t3k` commands that use an existing token (whoami, models, fetch, list, pull, search, resolve, ladder,
  pack, suggest-body) must not require `TONE3000_CLIENT_ID`. Refresh needs a client id only if TONE3000's token
  refresh requires it: check the code path and the API docs in the repo; if it does, use the client id stored in
  the token file. `login` stores the publishable client id in the token file (it already has a `client_id` field —
  make sure login writes it). Never store or accept `t3k_cs_`.
- Tests: whoami / fetch with a stored token and no env var succeed (mocked HTTP, respx); refresh path uses the
  token-file client id; login writes client_id.

## Acceptance

Both tasks reviewer-ACCEPTed; full CI green. The user's Mac check: with `launchctl unsetenv TONE3000_CLIENT_ID`,
Logic restarted, capture swap and BLEND body-amp download work.
