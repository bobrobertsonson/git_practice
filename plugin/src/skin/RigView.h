#pragma once

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FootswitchButton.h"
#include "LedIndicator.h"
#include "SkinAssets.h"

namespace sawblade::plugin::skin {

enum class Piece { SawAmp, BodyAmp, Cab, SawPedal, BodyPedal };
constexpr int kNumPieces = 5;

// One selectable render in the rig (amp head, cab or pedal). Pure image + hit target; the drop
// shadow and the selection ring are drawn by RigView around it.
class RigPiece : public juce::Component, public juce::SettableTooltipClient {
 public:
  RigPiece(Piece piece, Panel panel, const juce::String& title, float cornerRadius);
  Piece piece() const noexcept { return piece_; }
  float cornerRadius() const noexcept { return radius_; }
  void paint(juce::Graphics&) override;
  void mouseDown(const juce::MouseEvent&) override;
  std::function<void(Piece)> onSelect;

 private:
  Piece piece_;
  Panel panel_;
  float radius_;
};

// The left part of the main screen (940 x 742 design px): amp heads, cab, cables, pedalboard with
// the two pedals, footswitches + LEDs and the "+ PEDAL" placeholders. Positions follow
// design/mockups/RigReal.dc.html.
class RigView : public juce::Component {
 public:
  static constexpr int kWidth = 940, kHeight = 742;

  RigView();
  ~RigView() override;

  void select(Piece p);
  Piece selected() const noexcept { return selected_; }
  std::function<void(Piece)> onSelect;

  void paint(juce::Graphics&) override;
  void paintOverChildren(juce::Graphics&) override;

  RigPiece& piece(Piece p) { return *pieces_[static_cast<size_t>(p)]; }
  FootswitchButton& footswitch(Piece pedal) { return *switches_[pedal == Piece::SawPedal ? 0 : 1]; }
  LedIndicator& led(Piece pedal) { return *leds_[pedal == Piece::SawPedal ? 0 : 1]; }

 private:
  class Cables;
  void layoutPedal(Piece pedal, juce::Rectangle<int> image, float mmPerImageHeight, juce::Point<float> switchMm, juce::Point<float> ledMm);

  std::array<std::unique_ptr<RigPiece>, kNumPieces> pieces_;
  std::unique_ptr<Cables> cables_;
  std::array<std::unique_ptr<FootswitchButton>, 2> switches_;
  std::array<std::unique_ptr<LedIndicator>, 2> leds_;
  std::array<std::unique_ptr<juce::TextButton>, 2> addPedal_;
  Piece selected_ = Piece::SawPedal;
};

}  // namespace sawblade::plugin::skin
