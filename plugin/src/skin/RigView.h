#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FootswitchButton.h"
#include "LedIndicator.h"
#include "SkinAssets.h"

namespace sawblade::plugin::skin {

// What the inspector and BROWSE CAPTURES act on. SawPedal / BodyPedal: a pedal tile of path A / path B (which one:
// RigView::selectedBlockId(); empty = the first tile of that path). The cab is not a piece of the main page any more: it has
// its own page (v0.4 Task D), opened by the CAB button and the cab chip.
enum class Piece { SawAmp, BodyAmp, SawPedal, BodyPedal };

// The geometry of the rig area (940 x 742 design px) in rig coordinates (v0.4 Task D, docs/specs/v0_4-pedals.md "Lead decisions").
// Two columns, SAW left and BODY right; the heads sit at the same y and have the same size, centred in their column; below each head a
// caption and the path's pedalboard; the cab chip is centred at the bottom. Nothing here depends on whether path B is on.
struct RigLayout {
  static constexpr int kWidth = 940, kHeight = 742;
  static constexpr int kSawColumnX = 16, kBodyColumnX = 478, kColumnW = 446;
  static constexpr int kHeadW = 330, kHeadY = 44;
  static constexpr int kHeadToBoard = 40;  // head bottom -> board top (the column caption sits in between)
  static constexpr int kBoardBottom = 672;
  static constexpr int kChipW = 320, kChipH = 36, kChipY = 690;

  static int headHeight();  // from the amp art's aspect at kHeadW; the same for both heads
  static juce::Rectangle<int> column(int path) { return {path == 0 ? kSawColumnX : kBodyColumnX, 0, kColumnW, kHeight}; }
  static juce::Rectangle<int> head(int path);     // path 0 = SAW, 1 = BODY
  static juce::Rectangle<int> caption(int path);  // "SAW · n PEDALS", between the head and the board
  static juce::Rectangle<int> board(int path);    // head bottom + kHeadToBoard .. kBoardBottom, the column's width
  static juce::Rectangle<int> chip() { return juce::Rectangle<int>(kChipW, kChipH).withCentre({kWidth / 2, kChipY + kChipH / 2}); }
};

// One selectable render in the rig (an amp head). Pure image + hit target; the drop shadow and the selection ring are drawn by
// RigView around it.
class RigPiece : public juce::Component, public juce::SettableTooltipClient {
 public:
  RigPiece(Piece piece, Panel panel, const juce::String& title, float cornerRadius);
  Piece piece() const noexcept { return piece_; }
  float cornerRadius() const noexcept { return radius_; }
  void paint(juce::Graphics&) override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDoubleClick(const juce::MouseEvent&) override;
  std::function<void(Piece)> onSelect;
  std::function<void(Piece)> onDoubleClick;

 private:
  Piece piece_;
  Panel panel_;
  float radius_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigPiece)
};

// The cab chip at the bottom of the rig area, where both paths meet: "CAB · <IR title> · ● LIVE" / "● STUDIO", or "CAB OFF" when
// the cab is disabled. A click opens the CAB page.
class CabChip : public juce::Component, public juce::SettableTooltipClient {
 public:
  enum class Mode { Off, Live, Studio };
  CabChip();

  // `name`: the cab / IR title ("" when the preset has none).
  void set(const juce::String& name, Mode mode);
  Mode mode() const noexcept { return mode_; }
  // The chip as one string: "CAB · V30 4x12 · ● LIVE", "CAB OFF".
  juce::String text() const;

  std::function<void()> onClick;
  void paint(juce::Graphics&) override;
  void mouseUp(const juce::MouseEvent&) override;

 private:
  juce::String name_;
  Mode mode_ = Mode::Off;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CabChip)
};

// The left part of the main screen (940 x 742 design px): the room, the two amp heads side by side, the cables (board -> head per
// column, both heads -> the cab chip) and the cab chip. The pedalboards (rig::Pedalboard) are a sibling the editor lays over this
// view; the amp controls (rig::AmpHead) lie over the heads' art.
class RigView : public juce::Component {
 public:
  static constexpr int kWidth = RigLayout::kWidth, kHeight = RigLayout::kHeight;

  RigView();
  ~RigView() override;

  // Selects an amp head, or a pedal tile of path A / B (`blockId`: which tile; "" = the first of that path).
  void select(Piece p, const std::string& blockId = {});
  Piece selected() const noexcept { return selected_; }
  const std::string& selectedBlockId() const noexcept { return selectedId_; }
  std::function<void(Piece)> onSelect;

  // Path B off: the BODY head and its cable are drawn dimmed (alpha kOffAlpha).
  static constexpr float kOffAlpha = 0.35f;
  void setBodyOff(bool off);
  bool bodyOff() const noexcept { return bodyOff_; }

  void paint(juce::Graphics&) override;
  void paintOverChildren(juce::Graphics&) override;

  RigPiece& head(int path) { return *heads_[static_cast<size_t>(path)]; }
  RigPiece& piece(Piece p) { return head(p == Piece::BodyAmp ? 1 : 0); }  // amp heads only
  CabChip& cabChip() { return *chip_; }

 private:
  class Cables;

  std::array<std::unique_ptr<RigPiece>, 2> heads_;
  std::unique_ptr<Cables> cables_;
  std::unique_ptr<CabChip> chip_;
  Piece selected_ = Piece::SawPedal;
  std::string selectedId_;
  bool bodyOff_ = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigView)
};

}  // namespace sawblade::plugin::skin
