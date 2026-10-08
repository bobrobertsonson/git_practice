# Export notes parity fixtures

`*.notes.json` and `*.export_notes.txt` are the output of the v0.4M Python
(`match/sawblade_match/export/notes.py`, `NOTES_VERSION = 1`, commit 9074c0b on `claude/sawblade-v0_4m-matcher-feel`)
for the three `*_preset.json` files. `plugin/tests/test_export_notes.cpp` requires `plugin/src/ExportNotes.cpp` to reproduce them.

Regenerate (after v0.4M changes the format, or on a merge of that branch where `notes.py` is on the base):

```
git show origin/claude/sawblade-v0_4m-matcher-feel:match/sawblade_match/export/notes.py > /tmp/notes.py
python3 plugin/tests/fixtures/export_notes/generate.py /tmp/notes.py
```

## Export reports (v0.6 Task C)

`report_a2_anagram.json` and `report_a1_generic.json` are hand-written `export_report.json` files in the shape of
docs/specs/v0_6-a2_everywhere.md decision 14 (`arch`, `size`, `files`, `validation.{full,lite}`; the A1 one has
`validation.acceptance` and no `deviceProfiles`). The plugin reads the `deviceProfiles.anagram` keys defensively and renders
them generically (ExportNotes.h); the fixture is the shape the plugin tests build against, not a copy of the exporter's output.
