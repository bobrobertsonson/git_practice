# Export notes parity fixtures

`*.notes.json` and `*.export_notes.txt` are the output of the v0.4M Python
(`match/sawblade_match/export/notes.py`, `NOTES_VERSION = 1`, commit 9074c0b on `claude/sawblade-v0_4m-matcher-feel`)
for the three `*_preset.json` files. `plugin/tests/test_export_notes.cpp` requires `plugin/src/ExportNotes.cpp` to reproduce them.

Regenerate (after v0.4M changes the format, or on a merge of that branch where `notes.py` is on the base):

```
git show origin/claude/sawblade-v0_4m-matcher-feel:match/sawblade_match/export/notes.py > /tmp/notes.py
python3 plugin/tests/fixtures/export_notes/generate.py /tmp/notes.py
```
