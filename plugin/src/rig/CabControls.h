#pragma once

// The cab controls, one class for both places that show them (v0.4 Task D): the CAB tab of the rig editor and the CAB page
// (CabScreen). Mode SHARED / PER PATH, CAB ON, one IR card per target (SHARED IR, or PATH A IR (SAW) + PATH B IR (BODY)) with
// CHOOSE... (a .wav file) and, on the CAB page, BROWSE IR (the capture browser for the cab), the LIVE / STUDIO notice, and on
// the CAB page a MIC POSITIONS button (the mic page). Every change goes through RigController::edit (one undo step, prepared
// off the audio thread); the cab has no level control (no field in CabPreset).

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "rig/RigController.h"
#include "rig/RigModel.h"
#include "rig/RigWidgets.h"

namespace sawblade::plugin::rig {

class CabControls : public juce::Component {
 public:
  // withActions: the CAB page's extra buttons (BROWSE IR on every IR card, MIC POSITIONS). The rig editor's tab has none.
  CabControls(RigController& c, bool withActions);
  ~CabControls() override;

  // Pulls the preset into the controls (cheap; called while the page is visible).
  void refresh(const Preset& p);

  // CAB page only: BROWSE IR (any card) and MIC POSITIONS.
  std::function<void()> onBrowseIr;
  std::function<void()> onMicPositions;

  // The one-line LIVE / STUDIO notice under the cards, and the colour rule it shares with the mode chip.
  juce::String noticeText() const { return notice.getText(); }
  bool cardVisible(CabSlot s) const;
  juce::TextButton& chooseButton(CabSlot s);
  juce::TextButton& browseButton(CabSlot s);  // exists on every card, visible only with actions
  juce::TextButton& micButton() { return mic; }
  juce::Button& cabOnButton() { return *on; }
  juce::String cardTitle(CabSlot s) const;  // the IR the card shows

  void resized() override;

  Segmented mode;

 private:
  struct IrCard;
  IrCard& card(CabSlot s);
  const IrCard& card(CabSlot s) const;
  void choose(CabSlot slot);

  RigController& controller;
  bool withActions_;
  std::unique_ptr<LedToggle> on;
  std::unique_ptr<IrCard> shared, cardA, cardB;
  juce::Label notice;
  juce::TextButton mic;
  std::unique_ptr<juce::FileChooser> chooser;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabControls)
};

}  // namespace sawblade::plugin::rig
