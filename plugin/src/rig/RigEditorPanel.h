#pragma once

// The rig editor overlay (spec phase 10, section 6): 940 x 742 design px exactly over the rig area, opened by the
// RIG toggle in the top bar. Topology selector and tab strip (CHAIN / EQ / BLEND / CAB / GATE / COMP) on top, one
// page per tab below. It edits the preset only through the RigController (structural edits via the loader, live
// edits and parameters directly); open / closed and the active tab are UI state and are never saved.

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "rig/EqGraph.h"
#include "rig/RigController.h"

namespace sawblade::plugin::rig {

class RigEditorPanel : public juce::Component {
 public:
  static constexpr int kWidth = 940, kHeight = 742;
  enum class Tab { Chain = 0, Eq, Blend, Cab, Gate, Comp };
  static constexpr int kNumTabs = 6;

  RigEditorPanel(SawbladeProcessor& p, RigController& c);
  ~RigEditorPanel() override;

  // Pulls the preset / status into the controls. Cheap when nothing changed (called from the editor's timer
  // while the panel is visible).
  void refresh();

  void setTab(Tab t);
  Tab tab() const noexcept;
  juce::Button& tabButton(Tab t);
  juce::Button& topologyButton(Topology t);
  juce::Button& eqTargetButton(EqTarget t);
  juce::Button& cabModeButton(CabMode m);
  void setEqTarget(EqTarget t);
  EqGraph& eqGraph();
  juce::String statusText() const;
  juce::String eqReadoutText() const;
  RigController& controller() noexcept;

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigEditorPanel)
};

}  // namespace sawblade::plugin::rig
