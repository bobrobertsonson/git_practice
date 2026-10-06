#include "SongInput.h"

#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin::song_input {
using L = SawbladeLookAndFeel;

ChooserSpec chooserSpec(Action a, bool mac) {
  if (a == Action::SongFile) {
    // JUCE matches filters case-insensitively on every platform, so lower-case extensions cover .WAV too.
    // On macOS the filter is "*": JUCE then passes allowedFileTypes = nil and its panel delegate matches every
    // file, so neither AppKit mechanism can disable a .wav; handlePicked() validates the pick instead.
    const juce::String filter = mac ? juce::String("*") : juce::String("*.wav;*.mp3;*.flac;*.m4a;*.aif;*.aiff;*.aac;*.ogg");
    return {"Choose a song file", filter, juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles};
  }
  return {"Choose a folder of separated stems", juce::String(),
          juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories};
}

ChooserSpec importChooserSpec(bool mac) {
  const juce::String filter = mac ? juce::String("*") : juce::String("*.wav;*.aif;*.aiff;*.flac");
  return {"Choose a DI recording to import", filter, juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles};
}

bool handlePicked(PlayAlong& pa, Action a, const juce::File& f, PickNotice& notice) {
  if (f == juce::File()) return false;  // cancelled
  const bool ok = a == Action::SongFile ? (f.existsAsFile() && isSongFileName(f.getFullPathName().toStdString())) : f.isDirectory();
  if (!ok) {
    // Rejected before loadSong, which would treat a non-song name as a folder; the current song stays loaded.
    notice.set(a == Action::SongFile ? "Not a song file: choose an mp3, wav, flac, m4a, aif, aac or ogg file."
                                     : "Not a folder: choose a folder of separated stems.");
    return false;
  }
  notice.clear();
  return pa.loadSong(f.getFullPathName().toStdString(), /*userInitiated=*/true);  // false: refused, nothing loads
}

void launchChooser(std::unique_ptr<juce::FileChooser>& holder, Action a, std::function<void(const juce::File&)> onPicked) {
  launchChooserSpec(holder, chooserSpec(a), std::move(onPicked));
}

void launchChooserSpec(std::unique_ptr<juce::FileChooser>& holder, const ChooserSpec& spec, std::function<void(const juce::File&)> onPicked) {
  holder = std::make_unique<juce::FileChooser>(spec.title, juce::File(), spec.filter);
  holder->launchAsync(spec.flags, [cb = std::move(onPicked)](const juce::FileChooser& fc) { cb(fc.getResult()); });
}

bool isLoadableDrop(const juce::StringArray& files) {
  for (const auto& f : files)
    if (juce::File(f).isDirectory() || isSongFileName(f.toStdString())) return true;
  return false;
}

bool loadDroppedFiles(PlayAlong& pa, const juce::StringArray& files) {
  for (const auto& f : files) {
    if (!juce::File(f).isDirectory() && !isSongFileName(f.toStdString())) continue;
    pa.loadSong(f.toStdString(), /*userInitiated=*/true);
    return true;
  }
  return false;
}

StatusLine statusLine(const PlayAlong::LoadStatus& st, bool standalone, bool hostSync, const juce::String& pickNotice, const juce::String& noneText) {
  using S = PlayAlong::LoadStatus::State;
  StatusLine out;
  juce::String msg;
  juce::Colour col = L::dimText();
  switch (st.state) {
    case S::Separating: {
      msg = "Separating " + juce::String(juce::roundToInt(st.separationFraction * 100.0)) + "%";
      if (st.separationEtaSeconds >= 0.0) msg += "  (about " + juce::String(juce::roundToInt(st.separationEtaSeconds)) + " s left)";
      col = L::warning();
      break;
    }
    case S::NotSeparated: msg = juce::String(st.message); col = L::warning(); break;
    case S::Cancelled: msg = "Separation cancelled."; break;
    case S::Loading: msg = "Loading stems..."; col = L::warning(); break;
    case S::Failed: msg = juce::String(st.message); col = L::error(); break;
    case S::Ready:
      if (!st.warnings.empty()) {
        msg = juce::String(st.warnings.front());
        col = L::warning();
      } else if (st.suggestedLevelDb) {
        msg = "Level set to " + juce::String(*st.suggestedLevelDb, 1) + " dB to match the rig. Adjust to taste.";
      } else if (st.otherMappedToGuitar && (standalone || hostSync)) {
        msg = "4-stem song: 'other' is treated as the guitar.";
      }
      break;
    case S::None: msg = noneText; break;
  }
  if (st.state == S::Ready && msg.isEmpty() && !standalone && !hostSync)
    msg = "Backing is off. Enable SYNC TO HOST to follow the host transport.";
  if (!st.notice.empty()) {
    msg = juce::String(st.notice);
    col = L::error();
  }
  if (pickNotice.isNotEmpty()) {
    msg = pickNotice;
    col = L::error();
  }
  // A missing model: a short line in the status row (full message in the tooltip), then the complete command in a
  // wrapped, selectable field below it, with COPY in the status row.
  out.showFetch = st.state == S::Failed && st.modelMissing && !st.fetchCommand.empty() && pickNotice.isEmpty() && st.notice.empty();
  out.text = out.showFetch ? juce::String("Separation model not installed. Run this from the repository root:") : msg;
  out.tooltip = msg;
  out.colour = col;
  out.separating = st.state == S::Separating;
  out.fetchCommand = juce::String(st.fetchCommand);
  out.fetchTooltip = out.showFetch ? "Copy the install command to the clipboard.\n\n" + juce::String(st.message)
                                   : juce::String("Copy the install command to the clipboard");
  return out;
}

void styleFetchField(juce::TextEditor& field) {
  field.setName("fetchCommand");
  field.setMultiLine(true, true);  // word-wrapped: the whole command is visible
  field.setReturnKeyStartsNewLine(false);
  field.setScrollbarsShown(false);
  field.setIndents(3, 1);
  field.setBorder(juce::BorderSize<int>(1));
  field.setReadOnly(true);
  field.setCaretVisible(false);
  field.setSelectAllWhenFocused(true);
  field.setFont(L::monoFont(9.0f));
  field.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141210));
  field.setColour(juce::TextEditor::textColourId, L::text());
  field.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
  field.setTitle("Install command");
}

void fitFetchFont(juce::TextEditor& field) {
  for (float h = 9.0f; h >= 7.0f; h -= 0.5f) {
    field.applyFontToAllText(L::monoFont(h));  // setFont alone leaves the existing text
    if (field.getTextHeight() <= field.getHeight()) break;
  }
}

}  // namespace sawblade::plugin::song_input
