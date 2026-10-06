#include "RigView.h"

#include <algorithm>

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin::skin {
namespace {
using L = SawbladeLookAndFeel;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

int heightFor(const juce::Image& img, int width, int fallback) {
  return img.isValid() ? juce::roundToInt(static_cast<float>(width) * static_cast<float>(img.getHeight()) / static_cast<float>(img.getWidth())) : fallback;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
int RigLayout::headHeight() { return heightFor(SkinAssets::get().panel(Panel::AmpSaw), kHeadW, 145); }

juce::Rectangle<int> RigLayout::head(int path) {
  return {column(path).getX() + (kColumnW - kHeadW) / 2, kHeadY, kHeadW, headHeight()};
}

juce::Rectangle<int> RigLayout::caption(int path) {
  return {column(path).getX() + 8, head(path).getBottom() + 12, kColumnW - 16, 18};
}

juce::Rectangle<int> RigLayout::board(int path) {
  const int top = head(path).getBottom() + kHeadToBoard;
  return {column(path).getX(), top, kColumnW, kBoardBottom - top};
}

// ---------------------------------------------------------------------------------------------
RigPiece::RigPiece(Piece piece, Panel panel, const juce::String& title, float cornerRadius)
    : piece_(piece), panel_(panel), radius_(cornerRadius) {
  setTitle(title);
  setTooltip(title + " (click to select)");
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(false);
}

void RigPiece::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat();
  juce::Path clip;
  clip.addRoundedRectangle(b, radius_);
  const juce::Image& img = SkinAssets::get().panel(panel_);
  if (img.isValid()) {
    g.saveState();
    g.reduceClipRegion(clip);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(img, b);
    g.restoreState();
  } else {  // fallback: a plain labelled block
    g.setColour(L::panel());
    g.fillPath(clip);
    g.setColour(L::dimText());
    g.setFont(L::labelFont(13.0f));
    g.drawText(getTitle(), getLocalBounds(), juce::Justification::centred);
  }
}

void RigPiece::mouseDown(const juce::MouseEvent&) {
  if (onSelect) onSelect(piece_);
}

void RigPiece::mouseDoubleClick(const juce::MouseEvent&) {
  if (onDoubleClick) onDoubleClick(piece_);
}

// ---------------------------------------------------------------------------------------------
CabChip::CabChip() {
  setTitle("Cab");
  setTooltip("The cab: its IR and LIVE / STUDIO (click to open the CAB page)");
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(false);
}

void CabChip::set(const juce::String& name, Mode mode) {
  if (name == name_ && mode == mode_) return;
  name_ = name;
  mode_ = mode;
  repaint();
}

juce::String CabChip::text() const {
  if (mode_ == Mode::Off) return "CAB OFF";
  return "CAB" + kDot + (name_.isEmpty() ? juce::String("NO IR") : name_) + kDot + juce::String::fromUTF8("\xe2\x97\x8f ") +
         (mode_ == Mode::Live ? "LIVE" : "STUDIO");
}

void CabChip::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat().reduced(1.0f);
  g.setColour(L::panel());
  g.fillRoundedRectangle(b, 8.0f);
  g.setColour(L::chipBorder());
  g.drawRoundedRectangle(b, 8.0f, 1.2f);
  auto area = getLocalBounds().reduced(12, 0);
  if (mode_ != Mode::Off) {
    const auto right = area.removeFromRight(86);
    g.setColour(mode_ == Mode::Live ? L::live() : L::studio());
    g.setFont(L::labelFont(12.0f));
    g.drawText(juce::String::fromUTF8("\xe2\x97\x8f ") + (mode_ == Mode::Live ? "LIVE" : "STUDIO"), right, juce::Justification::centredRight);
  }
  g.setColour(mode_ == Mode::Off ? L::dimText() : L::text());
  g.setFont(L::labelFont(13.0f));
  const juce::String left = mode_ == Mode::Off ? juce::String("CAB OFF") : "CAB" + kDot + (name_.isEmpty() ? juce::String("NO IR") : name_);
  g.drawFittedText(left, area, juce::Justification::centredLeft, 1, 0.75f);
}

void CabChip::mouseUp(const juce::MouseEvent& e) {
  if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
}

// ---------------------------------------------------------------------------------------------
// The cables, drawn above the room and below the pedalboards: one short cable per column from the board's top edge up to its head,
// and one from each head down to the cab chip, where both paths meet.
class RigView::Cables : public juce::Component {
 public:
  Cables() { setInterceptsMouseClicks(false, false); }
  void setBodyOff(bool off) {
    if (off == bodyOff_) return;
    bodyOff_ = off;
    repaint();
  }
  void paint(juce::Graphics& g) override {
    const auto chip = RigLayout::chip();
    for (int path = 0; path < 2; ++path) {
      const bool off = path == 1 && bodyOff_;
      const float alpha = off ? kOffAlpha : 1.0f;
      const juce::Colour glow = path == 0 ? L::saw() : L::body();
      const auto head = RigLayout::head(path);
      const float cx = static_cast<float>(head.getCentreX());
      const float headBottom = static_cast<float>(head.getBottom());
      const float boardTop = static_cast<float>(RigLayout::board(path).getY());
      cable(g, {cx, boardTop}, {cx, boardTop - 14.0f}, {cx, headBottom + 14.0f}, {cx, headBottom}, glow, alpha);
      // head -> chip: leaves the head's inner half, reaches the chip's top edge on the same side
      const float side = path == 0 ? 1.0f : -1.0f;
      const juce::Point<float> from(cx + side * 110.0f, headBottom);
      const juce::Point<float> to(static_cast<float>(chip.getCentreX()) - side * 90.0f, static_cast<float>(chip.getY()));
      cable(g, from, {from.x, from.y + 150.0f}, {to.x, to.y - 150.0f}, to, glow, alpha);
    }
  }

 private:
  static void cable(juce::Graphics& g, juce::Point<float> a, juce::Point<float> c1, juce::Point<float> c2, juce::Point<float> b, juce::Colour glow,
                    float alpha) {
    juce::Path p;
    p.startNewSubPath(a);
    p.cubicTo(c1, c2, b);
    g.setColour(juce::Colour(0xff151515).withAlpha(alpha));
    g.strokePath(p, juce::PathStrokeType(7.0f));
    g.setColour(glow.withAlpha(0.55f * alpha));
    g.strokePath(p, juce::PathStrokeType(2.0f));
  }
  bool bodyOff_ = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Cables)
};

// ---------------------------------------------------------------------------------------------
RigView::RigView() {
  cables_ = std::make_unique<Cables>();
  cables_->setBounds(0, 0, kWidth, kHeight);
  addAndMakeVisible(*cables_);

  const auto make = [this](int path, Piece p, Panel panel, const char* title) {
    auto piece = std::make_unique<RigPiece>(p, panel, title, 4.0f);
    piece->setBounds(RigLayout::head(path));
    piece->onSelect = [this](Piece q) { select(q); };
    addAndMakeVisible(*piece);
    heads_[static_cast<size_t>(path)] = std::move(piece);
  };
  make(0, Piece::SawAmp, Panel::AmpSaw, "SAW amp head");
  make(1, Piece::BodyAmp, Panel::AmpBody, "BODY amp head");

  chip_ = std::make_unique<CabChip>();
  chip_->setBounds(RigLayout::chip());
  addAndMakeVisible(*chip_);
}

RigView::~RigView() = default;

void RigView::select(Piece p, const std::string& blockId) {
  const bool pedal = p == Piece::SawPedal || p == Piece::BodyPedal;
  const std::string id = pedal ? blockId : std::string();
  if (selected_ == p && selectedId_ == id) return;
  selected_ = p;
  selectedId_ = id;
  repaint();
  if (onSelect) onSelect(p);
}

void RigView::setBodyOff(bool off) {
  if (off == bodyOff_) return;
  bodyOff_ = off;
  head(1).setAlpha(off ? kOffAlpha : 1.0f);
  cables_->setBodyOff(off);
  repaint();
}

void RigView::paint(juce::Graphics& g) {
  const auto area = getLocalBounds().toFloat();
  // Room: a vertical wall-to-floor gradient plus a soft light pool.
  juce::ColourGradient wall(juce::Colour(0xff171513), 0, 0, juce::Colour(0xff0a0908), 0, area.getHeight() * 0.57f, false);
  wall.addColour(0.56 / 0.57, juce::Colour(0xff100f0d));
  g.setGradientFill(wall);
  g.fillAll();
  g.setGradientFill(juce::ColourGradient(juce::Colour(0xff24201b).withAlpha(0.9f), area.getWidth() * 0.45f, area.getHeight() * 0.30f,
                                         juce::Colour(0xff121110).withAlpha(0.0f), area.getWidth() * 0.45f + 560.0f, area.getHeight() * 0.30f, true));
  g.fillAll();
  const float floorY = area.getHeight() * 0.56f;
  g.setGradientFill(juce::ColourGradient(juce::Colour(0xff0e0c0a), 0, floorY, juce::Colour(0xff1a1612), 0, area.getHeight(), false));
  g.fillRect(0.0f, floorY, area.getWidth(), area.getHeight() - floorY);

  // Drop shadows of the heads (drawn here, under the children, so they can spill outside them).
  for (int path = 0; path < 2; ++path) {
    const float alpha = path == 1 && bodyOff_ ? kOffAlpha : 1.0f;
    juce::DropShadow(juce::Colours::black.withAlpha(0.75f * alpha), 18, {0, 14}).drawForRectangle(g, heads_[static_cast<size_t>(path)]->getBounds().reduced(2));
  }
}

void RigView::paintOverChildren(juce::Graphics& g) {
  if (selected_ != Piece::SawAmp && selected_ != Piece::BodyAmp) return;  // a selected pedal tile draws its own ring
  const RigPiece& pc = piece(selected_);
  g.setColour(L::saw());
  g.drawRoundedRectangle(pc.getBounds().toFloat().expanded(1.5f), pc.cornerRadius() + 1.5f, 3.0f);
}

}  // namespace sawblade::plugin::skin
