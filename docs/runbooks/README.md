# Runbooks for the user's Macs

The user's two Macs run Claude Code sessions in `~/sawblade`. They coordinate with the lead
(cloud) session **through git only**, so the user doesn't have to relay messages:

- **Inbox:** the lead commits work requests (e.g. presets to train) to
  `claude/sawblade-plugin-setup-7k0b8q`. Each Mac `git pull`s that branch on a loop.
- **Outbox:** each Mac writes a report file and pushes it to its own branch. The lead reads
  those branches on its check-ins.
- Machine-local secrets, captures, exports and audio are never committed (CLAUDE.md).

| Mac | Runbook | Pushes to | Report file |
|---|---|---|---|
| Mac 1 | `mac1_build.md` | `claude/sawblade-mac-build` | `docs/reports/mac_build_REPORT.md` |
| Mac 2 | `mac2_train.md` | `claude/sawblade-mac-train` | `docs/reports/mac_train_REPORT.md` |

Start each one with `/loop 20m Follow docs/runbooks/<runbook>.md` inside `claude` in
`~/sawblade`.
