#include "rig/Pedalboard.h"

#include <algorithm>
#include <utility>

#include "SawbladeLookAndFeel.h"
#include "pedals/CircuitParams.h"
#include "rig/AmpHead.h"
#include "rig/RigModel.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

// Camera extents of the pedal ortho renders (design/render): ortho_scale in mm over the long side.
constexpr float kSawPedalMmHeight = 205.0f, kBodyPedalMmHeight = 165.0f;
constexpr float kFootswitchFrameMm = 40.0f, kLedFrameMm = 20.0f;  // ui_sprites.py PX_FOOT / PX_LED
constexpr float kTileAspect = 1.5f;                                 // both pedal renders are 2 : 3

int tileHeightFor(int width) { return juce::roundToInt(static_cast<float>(width) * kTileAspect); }

juce::String bypassTitle(int path, const juce::String& name) { return juce::String(path == 0 ? "SAW" : "BODY") + " pedal " + name; }

// The dashed "+ PEDAL" placeholder (disabled until Task C).
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
    g.setFont(L::labelFont(juce::jlimit(9.0f, 15.0f, static_cast<float>(getWidth()) * 0.085f)));
    g.drawText("+ PEDAL", getLocalBounds(), juce::Justification::centred);
  }
};
}  // namespace

int boardBlockCount(const PathPreset& p) {
  const int amp = ampIndex(p);
  return amp < 0 ? static_cast<int>(p.blocks.size()) : amp;
}

int blocksAfterAmp(const PathPreset& p) {
  const int amp = ampIndex(p);
  return amp < 0 ? 0 : static_cast<int>(p.blocks.size()) - amp - 1;
}

juce::String pedalName(const Block& b) {
  if (const auto c = circuitForBlockType(b.type)) return juce::String(circuitInfo(*c).choiceName).toUpperCase();
  if (b.type == "pedal.ts") return "GREEN OVERDRIVE";
  if (b.type == "nam" || b.type == "eq") return juce::String(blockTitle(b)).toUpperCase();
  return juce::String(b.type).toUpperCase();
}

// ---------------------------------------------------------------------------------------------
BoardTile::BoardTile(int path, int blockIndex, const Block& b)
    : path_(path),
      index_(blockIndex),
      id_(b.id),
      name_(pedalName(b)),
      circuit_(circuitForBlockType(b.type).has_value()),
      panel_(circuit_ ? skin::Panel::PedalSaw : skin::Panel::PedalBody),
      mmHeight_(circuit_ ? kSawPedalMmHeight : kBodyPedalMmHeight),
      switchMm_(circuit_ ? juce::Point<float>(-30.0f, -80.0f) : juce::Point<float>(0.0f, -50.0f)),
      ledMm_(circuit_ ? juce::Point<float>(-30.0f, -64.5f) : juce::Point<float>(0.0f, -29.0f)),
      fs_(bypassTitle(path, name_) + " footswitch") {
  setTitle(bypassTitle(path, name_));
  setTooltip(name_ + " (click to select, footswitch = bypass)");
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(false);
  fs_.setTooltip("Bypass " + name_ + " (the LED is lit while the pedal is on)");
  led_.setTitle(bypassTitle(path, name_) + " LED");
  fs_.setPairedLed(&led_);
  fs_.onClick = [this] {
    if (onBypass) onBypass(*this, !fs_.getToggleState());
  };
  addAndMakeVisible(fs_);
  addAndMakeVisible(led_);
  showBypass(b.bypass);
}

BoardTile::~BoardTile() = default;

void BoardTile::showBypass(bool bypass) {
  fs_.setToggleState(!bypass, juce::dontSendNotification);
  led_.setOn(!bypass);
  repaint();
}

void BoardTile::setSelected(bool s) {
  if (s == selected_) return;
  selected_ = s;
  repaint();
}

void BoardTile::resized() {
  const float ppm = static_cast<float>(getHeight()) / mmHeight_;  // design px per mm
  const auto centre = getLocalBounds().toFloat().getCentre();
  // Pedal coordinates: +x right, +y toward the top of the image (the switches sit at negative y).
  auto at = [&](juce::Point<float> mm) { return juce::Point<float>(centre.x + mm.x * ppm, centre.y - mm.y * ppm); };
  const float fsSize = kFootswitchFrameMm * ppm;
  fs_.setBounds(juce::Rectangle<float>(0.0f, 0.0f, fsSize, fsSize + static_cast<float>(skin::FootswitchButton::kPressOffsetPx)).withCentre(at(switchMm_)).toNearestInt());
  led_.setBounds(skin::LedIndicator::boundsFor(at(ledMm_), kLedFrameMm * ppm));
}

void BoardTile::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat();
  const float radius = juce::jlimit(3.0f, 10.0f, b.getWidth() * 10.0f / 180.0f);
  juce::Path clip;
  clip.addRoundedRectangle(b, radius);
  const juce::Image& img = skin::SkinAssets::get().panel(panel_);
  g.saveState();
  g.reduceClipRegion(clip);
  if (img.isValid()) {
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(img, b);
  } else {  // fallback: a plain panel
    g.setColour(L::panel());
    g.fillPath(clip);
  }
  if (bypassed()) {
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillPath(clip);
  }
  g.restoreState();

  // The name chip over the baked caption of the render.
  const auto chip = juce::Rectangle<float>(b.getWidth() * 0.84f, b.getHeight() * 0.135f).withCentre({b.getCentreX(), b.getHeight() * 0.108f});
  g.setColour(juce::Colour(0xff121110));
  g.fillRoundedRectangle(chip, 2.0f);
  g.setColour(L::chipBorder());
  g.drawRoundedRectangle(chip, 2.0f, 0.75f);
  g.setColour(L::text());
  g.setFont(L::labelFont(std::max(7.0f, chip.getHeight() * 0.5f)));
  g.drawFittedText(name_, chip.toNearestInt().reduced(3, 1), juce::Justification::centred, 1, 0.5f);
}

void BoardTile::paintOverChildren(juce::Graphics& g) {
  if (!selected_) return;
  g.setColour(path_ == 0 ? L::saw() : L::body());
  g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), juce::jlimit(3.0f, 10.0f, static_cast<float>(getWidth()) * 10.0f / 180.0f), 3.0f);
}

void BoardTile::mouseDown(const juce::MouseEvent&) {
  if (onSelect) onSelect(*this);
}

void BoardTile::mouseDoubleClick(const juce::MouseEvent&) {
  if (onDoubleClick) onDoubleClick(*this);
}

// ---------------------------------------------------------------------------------------------
Pedalboard::Pedalboard(RigController& c) : controller_(c) {
  setTitle("Pedalboards");
  setInterceptsMouseClicks(false, true);  // only the tiles take the mouse: a click on a head still selects the head
  for (int path = 0; path < 2; ++path) {
    auto add = std::make_unique<PlaceholderButton>();
    add->setEnabled(false);
    add->setTitle(path == 0 ? "Add pedal to the SAW path" : "Add pedal to the BODY path");
    add->setTooltip("Add pedal (not available in this prototype)");
    addAndMakeVisible(*add);
    paths_[static_cast<std::size_t>(path)].add = std::move(add);
  }
}

Pedalboard::~Pedalboard() = default;

std::vector<juce::Rectangle<int>> Pedalboard::slotRects(int path, int tiles, bool withAdd) {
  const int slots = std::max(1, tiles + (withAdd ? 1 : 0));
  const auto inner = boardBounds(path).reduced(kBoardPad);
  const int tw = juce::jlimit(kMinTileW, kMaxTileW, (inner.getWidth() - kTileGap * (slots - 1)) / slots);
  const int th = tileHeightFor(tw);
  const int y = inner.getCentreY() - th / 2;
  std::vector<juce::Rectangle<int>> out;
  out.reserve(static_cast<std::size_t>(slots));
  for (int i = 0; i < slots; ++i) out.emplace_back(inner.getX() + i * (tw + kTileGap), y, tw, th);
  return out;
}

juce::Rectangle<int> Pedalboard::slotBounds(int path, int index) const {
  const PathView& v = paths_[static_cast<std::size_t>(path)];
  const auto rects = slotRects(path, static_cast<int>(v.tiles.size()), !v.off);
  return rects[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(rects.size()) - 1, index))];
}

BoardTile* Pedalboard::tile(int path, int index) {
  auto& tiles = paths_[static_cast<std::size_t>(path)].tiles;
  return index >= 0 && index < static_cast<int>(tiles.size()) ? tiles[static_cast<std::size_t>(index)].get() : nullptr;
}

BoardTile* Pedalboard::tileForBlock(int path, int blockIndex) { return tile(path, blockIndex); }  // tile i is block i

juce::Button& Pedalboard::addButton(int path) { return *paths_[static_cast<std::size_t>(path)].add; }

juce::String Pedalboard::captionText(int path) const {
  const PathView& v = paths_[static_cast<std::size_t>(path)];
  const juce::String name = path == 0 ? "SAW" : "BODY";
  if (v.off) return name + kDot + "OFF";
  const int n = static_cast<int>(v.tiles.size());
  return name + kDot + juce::String(n) + (n == 1 ? " PEDAL" : " PEDALS");
}

juce::String Pedalboard::offText(int path) const { return paths_[static_cast<std::size_t>(path)].off ? AmpHead::bodyOffWithBlocksText() : juce::String(); }

juce::String Pedalboard::afterAmpText(int path) const {
  const int n = paths_[static_cast<std::size_t>(path)].afterAmp;
  return n > 0 ? "+" + juce::String(n) + " AFTER AMP (rig editor)" : juce::String();
}

void Pedalboard::refresh(const Preset& shown) {
  bool rebuilt = false;
  for (int path = 0; path < 2; ++path) {
    const PathPreset& pp = path == 0 ? shown.a : shown.b;
    PathView& v = paths_[static_cast<std::size_t>(path)];
    const bool off = path == 1 && !pp.enabled;
    const int n = off ? 0 : boardBlockCount(pp);
    std::vector<Key> keys;
    keys.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      const Block& b = pp.blocks[static_cast<std::size_t>(i)];
      keys.push_back({b.id, b.type, pedalName(b)});
    }
    if (off != v.off || keys != v.keys) {
      rebuild(path, pp, std::move(keys), off);
      rebuilt = true;
    } else {
      for (int i = 0; i < n; ++i) v.tiles[static_cast<std::size_t>(i)]->showBypass(pp.blocks[static_cast<std::size_t>(i)].bypass);
    }
    const int after = off ? 0 : blocksAfterAmp(pp);
    if (after != v.afterAmp) {
      v.afterAmp = after;
      repaint();
    }
  }
  if (rebuilt && onTilesChanged) onTilesChanged();
}

void Pedalboard::rebuild(int path, const PathPreset& pp, std::vector<Key> keys, bool off) {
  PathView& v = paths_[static_cast<std::size_t>(path)];
  v.tiles.clear();  // a Component removes itself from its parent when destroyed
  v.off = off;
  v.keys = std::move(keys);
  for (int i = 0; i < static_cast<int>(v.keys.size()); ++i) {
    auto t = std::make_unique<BoardTile>(path, i, pp.blocks[static_cast<std::size_t>(i)]);
    t->onSelect = [this](BoardTile& tile) {
      if (onSelect) onSelect(tile);
    };
    t->onDoubleClick = [this](BoardTile& tile) {
      if (onTileDoubleClick) onTileDoubleClick(tile);
    };
    t->onBypass = [this](BoardTile& tile, bool bypass) { editBypass(tile.path(), tile.blockId(), bypass); };
    addAndMakeVisible(*t);
    v.tiles.push_back(std::move(t));
  }
  v.add->setVisible(!off);
  layoutPath(path);
  applySelection();
  repaint();
}

void Pedalboard::layoutPath(int path) {
  PathView& v = paths_[static_cast<std::size_t>(path)];
  const auto rects = slotRects(path, static_cast<int>(v.tiles.size()), !v.off);
  for (std::size_t i = 0; i < v.tiles.size(); ++i) v.tiles[i]->setBounds(rects[i]);
  if (!v.off) v.add->setBounds(rects[v.tiles.size()]);
}

void Pedalboard::resized() {
  layoutPath(0);
  layoutPath(1);
}

void Pedalboard::setSelected(int path, const std::string& blockId) {
  if (path == selPath_ && blockId == selId_) return;
  selPath_ = path;
  selId_ = blockId;
  applySelection();
}

BoardTile* Pedalboard::selectedTile() {
  if (selPath_ < 0) return nullptr;
  auto& tiles = paths_[static_cast<std::size_t>(selPath_)].tiles;
  if (selId_.empty()) return tiles.empty() ? nullptr : tiles.front().get();
  for (auto& t : tiles)
    if (t->blockId() == selId_) return t.get();
  return nullptr;
}

void Pedalboard::applySelection() {
  BoardTile* sel = selectedTile();
  for (auto& pv : paths_)
    for (auto& t : pv.tiles) t->setSelected(t.get() == sel);
}

void Pedalboard::editBypass(int path, const std::string& id, bool bypass) {
  controller_.edit([path, id, bypass](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    for (std::size_t i = 0; i < pp.blocks.size(); ++i)
      if (pp.blocks[i].id == id) {
        setBypass(pp, static_cast<int>(i), bypass);
        return;
      }
  });
}

void Pedalboard::paint(juce::Graphics& g) {
  for (int path = 0; path < 2; ++path) {
    const PathView& v = paths_[static_cast<std::size_t>(path)];
    const auto bounds = boardBounds(path);
    const auto area = bounds.toFloat();
    const float a = v.off ? 0.4f : 0.78f;
    juce::DropShadow(juce::Colours::black.withAlpha(0.8f * a), 22, {0, 14}).drawForRectangle(g, bounds.reduced(4));
    {
      juce::Path bp;
      bp.addRoundedRectangle(area, 12.0f);
      g.saveState();
      g.reduceClipRegion(bp);
      g.setColour(juce::Colour(0xff0c0c0c).withAlpha(a));
      g.fillRect(area);
      g.setColour(juce::Colour(0xff151515).withAlpha(a));
      for (float y = area.getBottom(); y > area.getY(); y -= 17.0f) g.fillRect(area.getX(), y - 14.0f, area.getWidth(), 14.0f);
      g.restoreState();
      g.setColour(juce::Colour(0xff2a2a2a).withAlpha(v.off ? 0.5f : 1.0f));
      g.drawRoundedRectangle(area.reduced(1.0f), 11.0f, 2.0f);
    }
    g.setFont(L::labelFont(12.0f));
    g.setColour((path == 0 ? L::sawText() : L::bodyText()).withAlpha(v.off ? 0.5f : 1.0f));
    g.drawText(captionText(path), skin::RigLayout::caption(path), juce::Justification::centredLeft);
    if (v.off) {
      g.setColour(L::dimText());
      g.setFont(L::titleFont(15.0f));
      g.drawText(offText(path), bounds.reduced(16), juce::Justification::centred);
    }
    const juce::String after = afterAmpText(path);
    if (after.isNotEmpty()) {
      g.setColour(L::dimText());
      g.setFont(L::monoFont(10.0f));
      g.drawText(after, bounds.reduced(14, 4).removeFromBottom(14), juce::Justification::centredRight);
    }
  }
}

}  // namespace sawblade::plugin::rig
