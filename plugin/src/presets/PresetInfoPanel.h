#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PresetLibrary.h"

namespace sawblade::plugin {

// The right-hand panel of the preset browser: name, category, bank, file and notes, then every capture of path A, path B
// and the cab (`ir`, or `irA` / `irB` for perPath and irMix): slot/role, title, @creator, licence, TONE3000 URL, a
// NON-COMMERCIAL tag when the licence contains "nc", and "local file: no attribution recorded" when there is no `source`.
class PresetInfoPanel : public juce::Component {
 public:
  PresetInfoPanel();
  void setEntry(const PresetEntry* e);  // copies; nullptr clears
  void paint(juce::Graphics&) override;

  // Everything the panel shows, one row per line (for tests and accessibility).
  juce::StringArray rows() const;
  juce::String plainText() const { return rows().joinIntoString("\n"); }
  int nonCommercialTags() const;

 private:
  struct Row {
    enum class Kind { Name, Meta, File, Notes, Error, Heading, Where, Title, Attribution, Url, Missing, Tag } kind;
    juce::String text;
  };
  std::vector<Row> rows_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetInfoPanel)
};

}  // namespace sawblade::plugin
