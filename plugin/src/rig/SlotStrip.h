#pragma once

// One path's modular slot strip (spec phase 10, section 6.1): left-to-right cards for the blocks in preset
// order (max 8, cards shrink to fit), then "+ ADD". Every edit goes through the RigController.

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "rig/RigController.h"
#include "rig/RigWidgets.h"

namespace sawblade::plugin::rig {

class SlotStrip : public juce::Component {
 public:
  static constexpr int kCardGap = 8, kAddWidth = 84, kMinCard = 92, kMaxCard = 168;

  // path: 0 = A, 1 = B.
  SlotStrip(RigController& c, int path, const juce::String& title, juce::Colour accent);
  ~SlotStrip() override;

  // Rebuilds the cards if the path's blocks changed (cheap compare otherwise). `emptyPedalSlot` shows the
  // placeholder of SINGLE + 2 PEDALS when the path has no second pedal.
  void refresh(const Preset& p, bool emptyPedalSlot);

  int numCards() const noexcept { return static_cast<int>(cards_.size()); }
  juce::Component& card(int i);
  juce::Button& addButton() { return add_; }
  // Adds a block of `type` (a registered block type; for "nam" the file chooser opens first).
  void addType(const juce::String& type);
  std::function<void(const juce::String&)> onMessage;  // errors for the status line

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  class Card;
  void showAddMenu();
  void addBlockOfType(const std::string& type, const juce::File& namFile);
  PathPreset& pathOf(Preset& p) const { return path_ == 0 ? p.a : p.b; }

  RigController& controller_;
  int path_;
  juce::String title_;
  juce::Colour accent_;
  std::vector<Block> shown_;
  bool shownEmpty_ = false;
  std::vector<std::unique_ptr<Card>> cards_;
  juce::Label heading_;
  juce::TextButton add_, emptySlot_;
  std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace sawblade::plugin::rig
