#pragma once

// Export notes (v0.4 Task E): every enabled stage of the preset that is NOT in the trained NAM model, in signal order, with
// its settings in hardware units, so the user can rebuild it around a loader pedal. JUCE-free.
//
// This is a C++ port of match/sawblade_match/export/notes.py (v0.4M, NOTES_VERSION 1: build_export_notes and
// format_notes_txt): same stages, order, settings keys and numbers, `hardware` sentences, `loaderOrder`, `message`, and the
// same text. The Python tool is the source of truth once v0.4M is merged; the parity fixtures in
// plugin/tests/fixtures/export_notes/ were produced by running that Python and the C++ output must equal them. If v0.4M
// changes the format, bump kExportNotesVersion, regenerate the fixtures and follow in the port. Before training the panel
// shows these notes; after training it shows the run report's `exportNotes` (when its version is kExportNotesVersion).

#include <string>

#include <nlohmann/json.hpp>

#include "sawblade/preset.h"

namespace sawblade::plugin {

constexpr int kExportNotesVersion = 1;

// notes.py DISCLAIMER: the closing line of the text when no licence note is given.
extern const char* const kExportNotesDisclaimer;

// The notes for `preset` exported in `mode` ("nocab" | "withcab"). `dropComp` = the bus comp is left out of the preset that
// is trained (the plan's "bypassed" list has "busComp"); in a no-cab export the comp is always listed. `namName` / `irName`
// are the exported file names (empty = not known yet, as before training).
nlohmann::json buildExportNotes(const Preset& preset, const std::string& mode, bool dropComp, const std::string& namName = {},
                                const std::string& irName = {});
// The same from a preset JSON dict (schema sawblade.preset), exactly as notes.py reads it (the parity tests use this).
nlohmann::json buildExportNotesFromJson(const nlohmann::json& preset, const std::string& mode, bool dropComp,
                                        const std::string& namName = {}, const std::string& irName = {});

// True when `notes` is an object of the version this port implements with a stages array: safe to show and copy.
bool exportNotesUsable(const nlohmann::json& notes);

// The text of <name>.export_notes.txt (also what COPY puts on the clipboard). Tolerant of foreign JSON (a report written
// by a newer tool): missing or mistyped members are skipped, it never throws. An empty licenceNote = the disclaimer.
std::string formatNotesTxt(const nlohmann::json& notes, const std::string& presetName = {}, const std::string& licenceNote = {});

// ---- device profiles (v0.6 Task C, docs/specs/v0_6-a2_everywhere.md decision 14) ----------------------------------------------
// The report's `exportNotes.deviceProfiles.anagram` maps each stage to a block of the device (a Neural Amp / Neural Pedal block
// for the model, the IR block for the cab, the compressor, the gate ...), where it goes in the chain and its settings in
// hardware terms. The exact keys belong to the exporter (match/); this side reads them DEFENSIVELY and renders them generically:
// per stage the block, the position and every settings key / value pair, plus any `hardware` / `text` sentence.
//   profile.stages | profile.blocks : array of objects; each: block (or anagramBlock / name), position (or chainPosition /
//     where), stage (the generic stage it comes from), settings (object of key / value, or array of {key,value}), hardware
//     (or text / note): string; profile.loaderOrder (or chain): string; profile.message: string; profile.file (or notesFile):
//     the `<name>.anagram_notes.txt` name.
// The profile object of `notes`, or null when there is none (or it is not an object).
nlohmann::json anagramProfileOf(const nlohmann::json& notes);
// The file name the profile names for its text file ("" = it names none).
std::string anagramNotesFileName(const nlohmann::json& profile);
// The text of the Anagram view of the notes box (also what COPY copies). Never throws; foreign / missing members are skipped.
// `trainingNote` = exportNotes.trainingNote (the exporter's training sentence; "" = none), placed before the licence line like the
// Python text.
std::string formatAnagramNotesTxt(const nlohmann::json& profile, const std::string& presetName = {}, const std::string& licenceNote = {},
                                  const std::string& trainingNote = {});

}  // namespace sawblade::plugin
