# Phase 9b: preset browser, factory presets, A/B compare

Builds on 9a (`plugin/src/presets/T3kTool.*`, settings file). The same rules hold: new code
goes in `plugin/src/presets/`, with minimal edits to `PluginEditor.*` / `PluginProcessor.*`.

## 1. Core + schema (dsp-engineer)
1. **`category`.** Add an optional top-level `"category": "<string>"` to the preset (UI
   metadata). It is not tone: the chain ignores it, and the writer writes it only when it is
   non-empty. Document it in `docs/PRESET_SCHEMA.md`, with the recommended values below.
2. **Recommended categories.** These cover the scope in CLAUDE.md:
   "Death metal", "Swedish death (HM-2)", "Black metal", "Thrash", "Doom / Sludge / Fuzz",
   "Hardcore / Crust", "Grind", "Metalcore / Djent", "Nu-metal", "Prog", "Other".
3. **Factory presets.** Set `category` in every factory preset: `presets/*.json`,
   `presets/styles/*.json` and `presets/matched/*.json`. Pick each one from the preset's
   name/notes. Change nothing else in them, and keep the formatting style.
4. **Matcher output.** The Python matcher passes `category` through when it starts from a
   preset that has one (match-engineer; a one-line change plus a test, only if the matcher
   writes presets from a template; otherwise state that no change is needed).

## 2. Library (dsp-engineer), `plugin/src/presets/PresetLibrary.{h,cpp}`
1. **Banks.**
   - **Factory** (read-only): `presets/*.json` ("Classic"), `presets/styles/*.json`,
     `presets/matched/*.json`, under the settings key `factoryPresetDir` (default
     `<repo>/presets`, through the same compile definition as 9a).
   - **User** (read-write): `<appdata>/sawblade/presets/*.json`, created on first save.
   - `*.resolved.json` files are never listed.
2. **Entries.** Each entry holds: bank, sub-bank (Classic / Styles / Matched / User), file,
   name, category (an empty category shows as "Uncategorised"; anything in `matched/` without
   one shows as "Matched"), notes, and capture summaries.
   - Scanning parses with the core parser in a mode that does **not** load captures.
   - Unparseable files are listed greyed out, with their error, and are not loadable.
   - Scanning runs off the message thread.
3. **Search.** Case-insensitive. Whitespace-separated terms are ANDed over the name, category,
   notes, and capture titles/creators. Combined with the bank and category filters.
4. **User operations.**
   - **Save:** writes `currentPreset()` over the current user preset file. When the current
     preset is not a user preset, it acts as Save As.
   - **Save As:** name + category → `<sanitised name>.json`. A collision asks before
     overwriting.
   - **Rename:** changes the `name` field and renames the file.
   - **Delete:** asks for confirmation, then moves the file to the OS trash.
   - Factory presets can't be modified; Save on one acts as Save As.
   - The play-along state is never written into a saved preset.
   - All of these go through the core writer.
5. **Previous / next.** The top-bar ‹ › buttons step through the browser's current filtered
   list.

## 3. TONE3000 resolve (dsp-engineer)
1. **When it runs.** Loading a preset where a capture has `source.provider == "tone3000"` and
   its `file` does not exist (relative to the preset dir) resolves it first:
   - It runs `sawblade-t3k resolve <preset> -o <appdata>/sawblade/resolved/<bank>/<stem>.resolved.json
     --progress-json`, through T3kTool (9a).
   - It then loads the resolved file.
   - When a resolved file exists, is newer than the preset, and all its files exist, it is used
     directly, with no child process.
2. **Feedback.**
   - Progress shows in the browser footer ("Resolving 2/5: <title>"). Cancel is available.
   - Exit 4 shows the not-logged-in message from 9a.
   - A missing executable shows LOCATE…
   - The current sound never changes until the resolved preset has loaded.
3. **Python (match-engineer):** `resolve --progress-json` prints one JSON line per capture
   (`{"done", "total", "capture": <json path>, "title"}`), flushed. Plus a pytest with the
   client mocked.

## 4. A/B compare (dsp-engineer), `plugin/src/presets/AbCompare.{h,cpp}`
1. **Slots.** Two slots, A and B, each holding a complete `Preset` (with its parameter
   values). A is active at start, with the current preset.
2. **Switching** (the existing top-bar A/B button, which shows "A" or "B" as its state):
   - Store `processor.currentPreset()` into the active slot.
   - Make the other slot active.
   - If that slot is empty, copy the stored preset into it first (B starts identical).
   - Load the slot's preset with `processor.loadPreset()` (normal loader, 30 ms swap).
3. **Loads.** A preset loaded from the browser or by ‹ › replaces the **active** slot only.
4. **Menu.** A small menu on the button (right-click) offers "Copy A → B", "Copy B → A" and
   "Reset compare".
5. **State.** Not saved in plugin state; a reopened session starts on A.

## 5. UI (dsp-engineer), `plugin/src/presets/PresetBrowser.{h,cpp}` + `PresetInfoPanel.*`
Use the existing skin style; no mockup exists for this page.
1. **Opening.** Clicking the top-bar preset selector opens the browser as an overlay over the
   rig + inspector. It contains "Load file…", which keeps today's file chooser.
2. **Left column:**
   - the banks (Factory: Classic / Styles / Matched; User);
   - the categories (only those present, with counts);
   - a search box on top.
3. **Middle:** the preset list (name, category, bank; double-click loads).
4. **Right: the info panel.**
   - The name, category, bank, file and notes.
   - **Every capture** in path A, path B and the cab (`ir`, `irA` / `irB`, including
     `irMix`). For each one: slot/role, title, `@creator`, licence, TONE3000 URL.
   - A **NON-COMMERCIAL** tag when the licence contains `nc` (case-insensitive).
   - "local file: no attribution recorded" when there is no `source`.
5. **Footer:** SAVE, SAVE AS, RENAME, DELETE (disabled for factory presets), the resolve
   progress and messages.

## Acceptance
- **Core tests:** `category` round-trip, ignored by the render (identical output with and
  without it), and every factory preset parses with a category.
- **Plugin tests:**
  - library scan of a temp factory + user tree (banks, sub-banks, category fallback,
    `.resolved.json` skipped, a broken file listed with its error);
  - search (AND, case, fields);
  - **save/load round-trip:** `currentPreset()` → Save As → load → `currentPreset()` equal,
    including parameter values;
  - rename and delete;
  - factory read-only;
  - **A/B swap:** change a parameter on A, switch to B (B equals the original), change
    another, switch back (A's value is restored), switch again (B's value is restored). Each
    switch is one engine build;
  - the resolve flow with a fake `sawblade-t3k` script (progress, exit 4 message, use of the
    cached resolved file without a child process);
  - an info-panel test where each capture's creator and licence appear, and the NC tag shows;
  - an editor test with the browser open, in the screenshot test.
- **Python:** pytest passes.
- **Gates:** full `ctest` passes, a clang `-Werror` build passes, and pluginval level 10
  passes.
