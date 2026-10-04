#include "RigView.h"

#include <algorithm>

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin::skin {
namespace {
using L = SawbladeLookAndFeel;

// Layout in rig coordinates (mockup: left/top of each element inside the 940 x 742 rig area).
const juce::Rectangle<int> kBoard{120, 742 - 16 - 300, 940 - 120 - 24, 300};  // left 120, right 24, bottom 16, h 300
constexpr int kSawAmpX = 34, kSawAmpY = 46, kBodyAmpX = 34, kBodyAmpY = 210, kAmpW = 330;
constexpr int kCabX = 400, kCabY = 40, kCabW = 330;
constexpr int kSawPedalX = 150, kSawPedalW = 180, kSawPedalBottom = 30;
constexpr int kBodyPedalX = 350, kBodyPedalW = 140, kBodyPedalBottom = 50;

// Camera extents of the pedal ortho renders (design/render): ortho_scale in mm over the long side.
constexpr float kSawPedalMmHeight = 205.0f, kBodyPedalMmHeight = 165.0f;
constexpr float kFootswitchFrameMm = 40.0f, kLedFrameMm = 20.0f;  // ui_sprites.py PX_FOOT / PX_LED

int heightFor(const juce::Image& img, int width, int fallback) {
  return img.isValid() ? juce::roundToInt(static_cast<float>(width) * static_cast<float>(img.getHeight()) / static_cast<float>(img.getWidth())) : fallback;
}

// The dashed "+ PEDAL" placeholder.
class PlaceholderButton : public juce::TextButton {
 public:
  PlaceholderButton() : juce::TextButton("+ PEDAL") {}
  void paintButton(juce::Graphics& g, bool, bool) override {
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(b, 10.0f);
    juce::Path p;
    p.addRoundedRectangle(b, 10.0f);
    juce::Path dashed;
    const float dash[] = {6.0f, 4.0f};
    juce::PathStrokeType(2.0f).createDashedStroke(dashed, p, dash, 2);
    g.setColour(L::rule().brighter(0.2f));
    g.fillPath(dashed);
    g.setColour(L::placeholderText());
    g.setFont(L::labelFont(15.0f));
    g.drawText("+ PEDAL", getLocalBounds(), juce::Justification::centred);
  }
};
}  // namespace

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

// ---------------------------------------------------------------------------------------------
// The two instrument cables, drawn above the amps and below the pedals.
class RigView::Cables : public juce::Component {
 public:
  Cables() { setInterceptsMouseClicks(false, false); }
  void paint(juce::Graphics& g) override {
    auto cable = [&g](juce::Point<float> a, juce::Point<float> c1, juce::Point<float> c2, juce::Point<float> b, juce::Colour glow) {
      juce::Path p;
      p.startNewSubPath(a);
      p.cubicTo(c1, c2, b);
      g.setColour(juce::Colour(0xff151515));
      g.strokePath(p, juce::PathStrokeType(7.0f));
      g.setColour(glow.withAlpha(0.55f));
      g.strokePath(p, juce::PathStrokeType(2.0f));
    };
    cable({190, 600}, {120, 520}, {60, 420}, {70, 160}, L::saw());
    cable({392, 640}, {300, 560}, {230, 470}, {230, 330}, L::body());
  }
};

// ---------------------------------------------------------------------------------------------
RigView::RigView() {
  const auto& a = SkinAssets::get();
  const int sawAmpH = heightFor(a.panel(Panel::AmpSaw), kAmpW, 144);
  const int bodyAmpH = heightFor(a.panel(Panel::AmpBody), kAmpW, 144);
  const int cabH = heightFor(a.panel(Panel::Cab4x12), kCabW, kCabW);
  const int sawPedalH = heightFor(a.panel(Panel::PedalSaw), kSawPedalW, 270);
  const int bodyPedalH = heightFor(a.panel(Panel::PedalBody), kBodyPedalW, 210);

  auto make = [this](Piece p, Panel panel, const char* title, float radius, juce::Rectangle<int> bounds) {
    auto piece = std::make_unique<RigPiece>(p, panel, title, radius);
    piece->setBounds(bounds);
    piece->onSelect = [this](Piece q) { select(q); };
    pieces_[static_cast<size_t>(p)] = std::move(piece);
  };
  make(Piece::SawAmp, Panel::AmpSaw, "SAW amp head", 4.0f, {kSawAmpX, kSawAmpY, kAmpW, sawAmpH});
  make(Piece::BodyAmp, Panel::AmpBody, "BODY amp head", 4.0f, {kBodyAmpX, kBodyAmpY, kAmpW, bodyAmpH});
  make(Piece::Cab, Panel::Cab4x12, "4x12 cab", 4.0f, {kCabX, kCabY, kCabW, cabH});
  const juce::Rectangle<int> sawPedal{kSawPedalX, kHeight - kSawPedalBottom - sawPedalH, kSawPedalW, sawPedalH};
  const juce::Rectangle<int> bodyPedal{kBodyPedalX, kHeight - kBodyPedalBottom - bodyPedalH, kBodyPedalW, bodyPedalH};
  make(Piece::SawPedal, Panel::PedalSaw, "SAW pedal: Stockholm Syndrome", 10.0f, sawPedal);
  make(Piece::BodyPedal, Panel::PedalBody, "BODY pedal: Tighten", 10.0f, bodyPedal);

  for (Piece p : {Piece::SawAmp, Piece::BodyAmp, Piece::Cab}) addAndMakeVisible(*pieces_[static_cast<size_t>(p)]);
  cables_ = std::make_unique<Cables>();
  cables_->setBounds(0, 0, kWidth, kHeight);
  addAndMakeVisible(*cables_);
  for (Piece p : {Piece::SawPedal, Piece::BodyPedal}) addAndMakeVisible(*pieces_[static_cast<size_t>(p)]);

  layoutPedal(Piece::SawPedal, sawPedal, kSawPedalMmHeight, {-30.0f, -80.0f}, {-30.0f, -64.5f});
  layoutPedal(Piece::BodyPedal, bodyPedal, kBodyPedalMmHeight, {0.0f, -50.0f}, {0.0f, -29.0f});

  const int addX[2] = {520, 650};
  for (int i = 0; i < 2; ++i) {
    auto b = std::make_unique<PlaceholderButton>();
    b->setBounds(addX[i], kHeight - 60 - 170, 110, 170);
    b->setEnabled(false);
    b->setTitle("Add pedal");
    b->setTooltip("Add pedal (not available in this prototype)");
    addAndMakeVisible(*b);
    addPedal_[static_cast<size_t>(i)] = std::move(b);
  }
}

RigView::~RigView() = default;

void RigView::layoutPedal(Piece pedal, juce::Rectangle<int> image, float mmPerImageHeight, juce::Point<float> switchMm, juce::Point<float> ledMm) {
  const size_t i = pedal == Piece::SawPedal ? 0 : 1;
  const float ppm = static_cast<float>(image.getHeight()) / mmPerImageHeight;  // design px per mm
  const auto centre = image.toFloat().getCentre();
  // Pedal coordinates: +x right, +y toward the top of the image (the switches sit at negative y).
  auto at = [&](juce::Point<float> mm) { return juce::Point<float>(centre.x + mm.x * ppm, centre.y - mm.y * ppm); };

  const float fsSize = kFootswitchFrameMm * ppm;
  auto sw = std::make_unique<FootswitchButton>(pedal == Piece::SawPedal ? "SAW pedal footswitch" : "BODY pedal footswitch");
  sw->setBounds(juce::Rectangle<float>(0, 0, fsSize, fsSize + FootswitchButton::kPressOffsetPx).withCentre(at(switchMm)).toNearestInt());
  auto led = std::make_unique<LedIndicator>();
  led->setBounds(LedIndicator::boundsFor(at(ledMm), kLedFrameMm * ppm));
  led->setTitle(pedal == Piece::SawPedal ? "SAW pedal LED" : "BODY pedal LED");
  sw->setPairedLed(led.get());
  addAndMakeVisible(*sw);
  addAndMakeVisible(*led);
  switches_[i] = std::move(sw);
  leds_[i] = std::move(led);
}

void RigView::select(Piece p) {
  if (selected_ == p) return;
  selected_ = p;
  repaint();
  if (onSelect) onSelect(p);
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

  // Pedalboard.
  const auto board = kBoard.toFloat();
  juce::DropShadow(juce::Colours::black.withAlpha(0.8f), 22, {0, 14}).drawForRectangle(g, kBoard.reduced(4));
  {
    juce::Path bp;
    bp.addRoundedRectangle(board, 12.0f);
    g.saveState();
    g.reduceClipRegion(bp);
    g.setColour(juce::Colour(0xff0c0c0c));
    g.fillRect(board);
    g.setColour(juce::Colour(0xff151515));
    for (float y = board.getBottom(); y > board.getY(); y -= 17.0f) g.fillRect(board.getX(), y - 14.0f, board.getWidth(), 14.0f);
    g.restoreState();
    g.setColour(juce::Colour(0xff2a2a2a));
    g.drawRoundedRectangle(board.reduced(1.0f), 11.0f, 2.0f);
  }

  // Drop shadows of the renders (drawn here, under the children, so they can spill outside them).
  for (const auto& pc : pieces_) {
    const auto bounds = pc->getBounds();
    juce::DropShadow(juce::Colours::black.withAlpha(0.75f), 18, {0, 14}).drawForRectangle(g, bounds.reduced(2));
  }

  // Captions.
  g.setFont(L::labelFont(12.0f));
  g.setColour(L::dimText());
  g.drawText(juce::String::fromUTF8("CAB \xc2\xb7 4x12 \xc2\xb7 SHARED"), kCabX, 378, 330, 16, juce::Justification::centredLeft);
  g.setColour(L::sawText());
  g.drawText(juce::String::fromUTF8("SAW \xc2\xb7 STOCKHOLM SYNDROME"), kSawPedalX, kHeight - 6 - 16, 240, 16, juce::Justification::centredLeft);
  g.setColour(L::bodyText());
  g.drawText(juce::String::fromUTF8("BODY \xc2\xb7 TIGHTEN"), kBodyPedalX, kHeight - 26 - 16, 200, 16, juce::Justification::centredLeft);
}

void RigView::paintOverChildren(juce::Graphics& g) {
  const auto b = pieces_[static_cast<size_t>(selected_)]->getBounds().toFloat();
  g.setColour(L::saw());
  g.drawRoundedRectangle(b.expanded(1.5f), pieces_[static_cast<size_t>(selected_)]->cornerRadius() + 1.5f, 3.0f);
}

}  // namespace sawblade::plugin::skin
