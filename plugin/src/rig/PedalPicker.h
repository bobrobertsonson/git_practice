#pragma once

// The "+ PEDAL" picker of the pedalboard (v0.4 Tasks C and B): a small overlay anchored to a board's + PEDAL slot with a tab strip.
// MODELED lists the five generic modeled pedals (CHAINSAW, MODDED SAW, ONE-KNOB SAW, BIG FUZZ, GREEN OVERDRIVE), no badge. CAPTURES lists
// the pedal captures in the local capture cache first (title, creator, licence tag, the CAPTURE badge and outline) and then a
// "SEARCH TONE3000..." row. More tabs drop in through addTab(). The picker only reports what was picked; the Pedalboard makes the edit.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "rig/CapturePedals.h"
#include "rig/PedalKind.h"
#include "rig/RigWidgets.h"
#include "sawblade/preset.h"

namespace sawblade::plugin::rig {

class PedalPicker : public juce::Component {
 public:
  struct Model {
    std::string type;       // block registry type: "pedal.hm"
    juce::String name;      // generic descriptor, upper case: "CHAINSAW"
    juce::String blurb;     // one line
  };
  static constexpr int kWidth = 300, kHeaderH = 40, kRowH = 46, kHeight = kHeaderH + 5 * kRowH + 12;

  PedalPicker();
  ~PedalPicker() override;

  // The five modeled pedals, in the order the picker lists them.
  static const std::vector<Model>& modeledModels();
  // A new block of that type at its defaults (no id, slot "pedal"); the Pedalboard gives it an id when it inserts it.
  static Block makeModeledBlock(const std::string& type);

  // 0 = SAW, 1 = BODY: which board's + PEDAL opened it (the heading says so).
  void setPath(int path);
  int path() const noexcept { return path_; }

  // Adds a tab (Task B: CAPTURES). `page` fills the area below the tab strip. Returns the tab's index.
  int addTab(const juce::String& title, std::unique_ptr<juce::Component> page);
  int tabCount() const noexcept { return static_cast<int>(pages_.size()); }
  juce::Button& tabButton(int index) { return tabs_.button(index); }
  void selectTab(int index);
  int selectedTab() const noexcept { return tabs_.selected(); }
  juce::TextButton& modelButton(int index);  // the MODELED rows

  std::function<void(const std::string& type)> onPickModeled;
  std::function<void(const CachedPedal& tone)> onPickCapture;  // a cached pedal capture (its first setting is added)
  std::function<void()> onSearch;                              // "SEARCH TONE3000...": the capture browser in insert mode
  // The CAPTURES tab (rebuilt from the cache each time the picker opens, and by reloadCaptures()).
  void reloadCaptures();
  int captureRowCount() const;
  juce::TextButton& captureRow(int i);
  juce::TextButton& searchRow();
  int capturesTab() const noexcept { return capturesTab_; }
  std::function<void()> onClose;  // the x button / Escape

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;

 private:
  class ModelList;
  class CapturesPage;
  int path_ = 0;
  Segmented tabs_;
  std::vector<Segmented::Item> items_;
  std::vector<std::unique_ptr<juce::Component>> pages_;
  ModelList* models_ = nullptr;      // owned by pages_[0]
  CapturesPage* captures_ = nullptr;  // owned by pages_[1]
  int capturesTab_ = -1;
  juce::TextButton close_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalPicker)
};

}  // namespace sawblade::plugin::rig
