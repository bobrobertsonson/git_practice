#include "rig/Pedalboard.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "SawbladeLookAndFeel.h"
#include "pedals/CircuitParams.h"
#include "rig/AmpHead.h"
#include "rig/CapturePedals.h"
#include "rig/RigModel.h"
#include "sawblade/auto_trim.h"

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
  void paintButton(juce::Graphics& g, bool over, bool) override {
    const float a = isEnabled() ? 1.0f : 0.45f;  // greyed while the path is full
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(b, 10.0f);
    juce::Path p;
    p.addRoundedRectangle(b, 10.0f);
    juce::Path dashed;
    const float dash[] = {6.0f, 4.0f};
    juce::PathStrokeType(2.0f).createDashedStroke(dashed, p, dash, 2);
    g.setColour((over && isEnabled() ? L::rule().brighter(0.5f) : L::rule().brighter(0.2f)).withMultipliedAlpha(a));
    g.fillPath(dashed);
    g.setColour(L::placeholderText().withMultipliedAlpha(a));
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
BoardTile::BoardTile(int path, int blockIndex, const Block& b, RigController& controller)
    : path_(path),
      index_(blockIndex),
      id_(b.id),
      name_(pedalName(b)),
      kind_(b.type == "nam" ? PedalKind::Capture : PedalKind::Modeled),  // a tile is before its path's amp: a nam block here is a pedal capture
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
  if (kind_ == PedalKind::Capture) {
    const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get());
    title_ = juce::String(blockTitle(b));
    if (nam != nullptr) {
      const Capture& c = nam->model;
      creator_ = c.source ? (c.source->creator.empty() ? juce::String("UNKNOWN CREATOR") : "@" + juce::String(c.source->creator)) : juce::String("LOCAL FILE");
      licence_ = c.source ? juce::String(c.source->license).toUpperCase() : juce::String();
      if (c.source) {
        toneId_ = c.source->id;
        modelId_ = c.source->modelId;
      }
    }
    setTooltip(title_ + " " + creator_ + (licence_.isNotEmpty() ? " " + licence_ : juce::String()) + ": a capture, fixed tone (click to select, footswitch = bypass)");
    const auto rid = id_;
    const int pth = path;
    level_ = std::make_unique<PresetKnob>(
        controller, "LEVEL", skin::FilmstripKnob::Kind::Pedal, L::capture(), skin::FilmstripKnob::Range{kBlockGainMinDb, kBlockGainMaxDb, 0.0, 1, "dB"},
        [rid, pth](Preset& p, double v) {
          PathPreset& pp = pth == 0 ? p.a : p.b;
          for (std::size_t i = 0; i < pp.blocks.size(); ++i)
            if (pp.blocks[i].id == rid) setBlockOutputGainDb(pp, static_cast<int>(i), v);
        },
        /*live=*/true);
    level_->knob().setTitle("Level " + title_);
    level_->knob().setTooltip("Output level of this capture, -24 to +24 dB, changes live (drag, shift = fine, double-click = 0)");
    if (nam != nullptr) level_->setValueFromPreset(nam->outputGainDb);
    addAndMakeVisible(*level_);
    selector_.setTitle("Setting " + title_);
    selector_.setTooltip("Choose another model (setting) of this capture: the capture is swapped, one undo step");
    selector_.onClick = [this] {
      settingsMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&selector_), [safe = juce::Component::SafePointer<BoardTile>(this)](int r) {
        if (safe != nullptr && r > 0 && r <= static_cast<int>(safe->settings_.size())) safe->chooseSetting(safe->settings_[static_cast<std::size_t>(r - 1)].modelId);
      });
    };
    addChildComponent(selector_);
  }
  showBypass(b.bypass);
}

BoardTile::~BoardTile() = default;

void BoardTile::showBypass(bool bypass) {
  if (bypass == bypassed() && led_.isOn() == !bypass) return;
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
  if (kind_ == PedalKind::Capture) return layoutCapture();
  const float ppm = static_cast<float>(getHeight()) / mmHeight_;  // design px per mm
  const auto centre = getLocalBounds().toFloat().getCentre();
  // Pedal coordinates: +x right, +y toward the top of the image (the switches sit at negative y).
  auto at = [&](juce::Point<float> mm) { return juce::Point<float>(centre.x + mm.x * ppm, centre.y - mm.y * ppm); };
  const float fsSize = kFootswitchFrameMm * ppm;
  fs_.setBounds(juce::Rectangle<float>(0.0f, 0.0f, fsSize, fsSize + static_cast<float>(skin::FootswitchButton::kPressOffsetPx)).withCentre(at(switchMm_)).toNearestInt());
  led_.setBounds(skin::LedIndicator::boundsFor(at(ledMm_), kLedFrameMm * ppm));
}

void BoardTile::paint(juce::Graphics& g) {
  if (kind_ == PedalKind::Capture) return paintCapture(g);
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

// --- capture tiles ---------------------------------------------------------------------------------------------------------------
void BoardTile::showLevel(double db) {
  if (level_) level_->setValueFromPreset(db);
}

void BoardTile::setSettings(std::vector<CachedModel> models, const std::string& currentModelId) {
  settings_ = std::move(models);
  modelId_ = currentModelId;
  juce::String current = "SETTING";
  for (const auto& m : settings_)
    if (m.modelId == currentModelId) current = juce::String(m.name).toUpperCase();
  selector_.setButtonText(current + juce::String::fromUTF8(" \xe2\x96\xbe"));
  selector_.setVisible(hasSelector());
  if (kind_ == PedalKind::Capture) layoutCapture();
}

juce::PopupMenu BoardTile::settingsMenu() const {
  juce::PopupMenu m;
  int id = 1;
  for (const auto& s : settings_) m.addItem(id++, juce::String(s.name), true, s.modelId == modelId_);
  return m;
}

void BoardTile::chooseSetting(const std::string& modelId) {
  if (modelId == modelId_) return;
  const auto cb = onSetting;
  if (cb) cb(*this, modelId);
}

void BoardTile::layoutCapture() {
  const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
  const float fsSize = 0.17f * h;
  fs_.setBounds(juce::Rectangle<float>(0.0f, 0.0f, fsSize, fsSize + static_cast<float>(skin::FootswitchButton::kPressOffsetPx)).withCentre({w * 0.5f, h * 0.885f}).toNearestInt());
  led_.setBounds(skin::LedIndicator::boundsFor({w * 0.82f, h * 0.885f}, 0.045f * h));
  selector_.setBounds(juce::Rectangle<float>(w * 0.08f, h * 0.465f, w * 0.84f, h * 0.075f).toNearestInt());
  if (level_) {
    const float top = hasSelector() ? 0.55f : 0.47f;
    const float kh = h * (0.80f - top), kw = std::min(w * 0.84f, kh * 0.9f);
    level_->setBounds(juce::Rectangle<float>(kw, kh).withCentre({w * 0.5f, h * (top + 0.80f) * 0.5f}).toNearestInt());
  }
}

void BoardTile::paintCapture(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat();
  const float w = b.getWidth(), h = b.getHeight();
  const float radius = juce::jlimit(3.0f, 10.0f, w * 10.0f / 180.0f);
  g.setColour(L::panel());
  g.fillRoundedRectangle(b, radius);
  if (bypassed()) {
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRoundedRectangle(b, radius);
  }
  paintKindOutline(g, PedalKind::Capture, b, radius, L::capture());
  paintKindBadge(g, PedalKind::Capture, juce::Rectangle<float>(w * 0.08f, h * 0.045f, w * 0.62f, h * 0.075f));
  g.setColour(L::text());
  g.setFont(L::labelFont(std::max(8.0f, h * 0.062f)));
  g.drawFittedText(title_, juce::Rectangle<float>(w * 0.08f, h * 0.14f, w * 0.84f, h * 0.15f).toNearestInt(), juce::Justification::topLeft, 2, 0.7f);
  g.setColour(L::dimText());  // where a modeled tile shows its circuit name: the creator
  g.setFont(L::bodyFont(std::max(8.0f, h * 0.05f)));
  g.drawFittedText(creator_, juce::Rectangle<float>(w * 0.08f, h * 0.295f, w * 0.84f, h * 0.06f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
  if (licence_.isNotEmpty()) {
    g.setColour(L::sawText());
    g.setFont(L::labelFont(std::max(7.0f, h * 0.042f)));
    g.drawFittedText(licence_, juce::Rectangle<float>(w * 0.08f, h * 0.355f, w * 0.84f, h * 0.05f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
  }
  g.setColour(L::dimText());
  g.setFont(L::labelFont(std::max(6.0f, h * 0.036f)));
  g.drawFittedText(juce::String::fromUTF8("CAPTURE \xc2\xb7 FIXED TONE"), juce::Rectangle<float>(w * 0.08f, h * 0.41f, w * 0.84f, h * 0.045f).toNearestInt(),
                   juce::Justification::centredLeft, 1, 0.6f);
}

void BoardTile::paintOverChildren(juce::Graphics& g) {
  if (!selected_) return;
  g.setColour(path_ == 0 ? L::saw() : L::body());
  g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), juce::jlimit(3.0f, 10.0f, static_cast<float>(getWidth()) * 10.0f / 180.0f), 3.0f);
}

// The handlers copy the callback first: the Pedalboard may rebuild (destroy) this tile from inside it, so nothing of `this` is touched after.
void BoardTile::mouseDown(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) {  // right-click / ctrl-click: the menu, no selection, no drag
    const auto menu = onContextMenu;
    if (menu) menu(*this);
    return;
  }
  const auto select = onSelect;
  const auto g = onGesture;  // both copied first: the first callback may rebuild (destroy) this tile
  if (select) select(*this);
  if (g) g(*this, Gesture::Down, e);
}

void BoardTile::mouseDrag(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  const auto g = onGesture;
  if (g) g(*this, Gesture::Drag, e);
}

void BoardTile::mouseUp(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  const auto g = onGesture;
  if (g) g(*this, Gesture::Up, e);
}

void BoardTile::mouseDoubleClick(const juce::MouseEvent&) {
  if (onDoubleClick) onDoubleClick(*this);
}

// ---------------------------------------------------------------------------------------------
// The strip inside a board's viewport: the tiles and the + PEDAL slot side by side.
class Pedalboard::Strip : public juce::Component {};

// The board's scroller; reports every scroll so the editor can re-place the live pedal face.
class Pedalboard::BoardViewport : public juce::Viewport {
 public:
  std::function<void()> onScrolled;
  void visibleAreaChanged(const juce::Rectangle<int>&) override {
    if (onScrolled) onScrolled();
  }
};

Pedalboard::Pedalboard(RigController& c) : controller_(c) {
  setTitle("Pedalboards");
  setInterceptsMouseClicks(false, true);  // only the tiles take the mouse: a click on a head still selects the head
  for (int path = 0; path < 2; ++path) {
    PathView& v = paths_[static_cast<std::size_t>(path)];
    v.viewport = std::make_unique<BoardViewport>();
    v.strip = std::make_unique<Strip>();
    v.viewport->setViewedComponent(v.strip.get(), /*deleteComponentWhenNoLongerNeeded=*/false);
    v.viewport->setScrollBarsShown(false, true);
    v.viewport->setScrollBarThickness(8);
    v.viewport->setTitle(path == 0 ? "SAW pedalboard" : "BODY pedalboard");
    v.viewport->onScrolled = [this] {
      if (onScrolled) onScrolled();
      repaint();
    };
    addAndMakeVisible(*v.viewport);
    auto add = std::make_unique<PlaceholderButton>();
    add->setTitle(path == 0 ? "Add pedal to the SAW path" : "Add pedal to the BODY path");
    add->setTooltip(path == 0 ? "Add a pedal to the SAW path" : "Add a pedal to the BODY path");
    add->onClick = [this, path] { showPicker(path); };
    v.strip->addAndMakeVisible(*add);
    v.add = std::move(add);
  }
}

Pedalboard::~Pedalboard() { alive_->store(false); }

std::vector<juce::Rectangle<int>> Pedalboard::slotRects(int path, int tiles, bool withAdd) {
  const int slots = std::max(1, tiles + (withAdd ? 1 : 0));
  const auto inner = boardBounds(path).reduced(kBoardPad);
  const int tw = juce::jlimit(kMinTileW, kMaxTileW, (inner.getWidth() - kTileGap * (slots - 1)) / slots);
  const int th = tileHeightFor(tw);
  const int stripH = inner.getHeight() - kScrollBar;
  const int y = (stripH - th) / 2;
  std::vector<juce::Rectangle<int>> out;
  out.reserve(static_cast<std::size_t>(slots));
  for (int i = 0; i < slots; ++i) out.emplace_back(i * (tw + kTileGap), y, tw, th);
  return out;
}

juce::Rectangle<int> Pedalboard::slotBounds(int path, int index) const {
  const PathView& v = paths_[static_cast<std::size_t>(path)];
  const auto rects = slotRects(path, static_cast<int>(v.tiles.size()), !v.off);
  return rects[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(rects.size()) - 1, index))] + boardBounds(path).reduced(kBoardPad).getPosition();
}

juce::Rectangle<int> Pedalboard::tileBounds(const BoardTile& t) const { return getLocalArea(&t, t.getLocalBounds()); }

bool Pedalboard::tileFullyVisible(int path, int blockIndex) {
  BoardTile* t = tile(path, blockIndex);
  return t == nullptr || paths_[static_cast<std::size_t>(path)].viewport->getBounds().contains(tileBounds(*t));
}

BoardTile* Pedalboard::tile(int path, int index) {
  auto& tiles = paths_[static_cast<std::size_t>(path)].tiles;
  return index >= 0 && index < static_cast<int>(tiles.size()) ? tiles[static_cast<std::size_t>(index)].get() : nullptr;
}

BoardTile* Pedalboard::tileForBlock(int path, int blockIndex) { return tile(path, blockIndex); }  // tile i is block i

juce::Button& Pedalboard::addButton(int path) { return *paths_[static_cast<std::size_t>(path)].add; }
juce::Viewport& Pedalboard::viewport(int path) { return *paths_[static_cast<std::size_t>(path)].viewport; }

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

bool Pedalboard::hasTile(int path, const std::string& id) const {
  for (const auto& t : paths_[static_cast<std::size_t>(path)].tiles)
    if (t->blockId() == id) return true;
  return false;
}

void Pedalboard::setMouseDownProbe(std::function<bool()> probe) { mouseDown_ = probe ? std::move(probe) : [] { return juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown(); }; }

void Pedalboard::say(const juce::String& m) {
  if (onMessage) onMessage(m);
}

void Pedalboard::refresh(const Preset& shown) {
  // A press that never got its mouse-up (lost mouse capture) must not freeze the board for good: give up on it after 10 s.
  if (drag_.tile != nullptr && !drag_.active && juce::Time::getMillisecondCounter() - drag_.downMs > 10000u) drag_ = Drag{};
  // An ACTIVE drag whose mouse button is no longer down lost its mouse-up (capture stolen): drop the ghost, restore the tile, carry on.
  if (drag_.tile != nullptr && drag_.active && !mouseDown_()) endDrag(/*drop=*/false);
  if (drag_.tile != nullptr) {  // a tile is pressed or dragged: nothing is rebuilt under the hand; the mouse-up re-reads the preset
    refreshPending_ = true;
    return;
  }
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
      std::string detail;
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get()))
        detail = nam->model.source ? nam->model.source->id + "/" + nam->model.source->modelId : nam->model.file;
      keys.push_back({b.id, b.type, pedalName(b), detail});
    }
    v.blocks = static_cast<int>(pp.blocks.size());
    if (off != v.off || keys != v.keys) {
      rebuild(path, pp, std::move(keys), off);
      rebuilt = true;
    } else {
      for (int i = 0; i < n; ++i) {
        const Block& b = pp.blocks[static_cast<std::size_t>(i)];
        v.tiles[static_cast<std::size_t>(i)]->showBypass(b.bypass);
        if (const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get())) v.tiles[static_cast<std::size_t>(i)]->showLevel(nam->outputGainDb);
      }
    }
    const bool full = pathFull(path);
    if (v.add->isEnabled() == full) {  // + PEDAL greys while the path holds its 8 blocks (the amp counts)
      v.add->setEnabled(!full);
      v.add->setTooltip(full ? "Path full: 8 blocks (the amp counts)" : path == 0 ? "Add a pedal to the SAW path" : "Add a pedal to the BODY path");
    }
    const int after = off ? 0 : blocksAfterAmp(pp);
    if (after != v.afterAmp) {
      v.afterAmp = after;
      repaint();
    }
  }
  if (rebuilt && onTilesChanged) onTilesChanged();
}

void Pedalboard::refreshNow() { refresh(controller_.processor().editBasePreset()); }

void Pedalboard::rebuild(int path, const PathPreset& pp, std::vector<Key> keys, bool off) {
  PathView& v = paths_[static_cast<std::size_t>(path)];
  v.tiles.clear();  // a Component removes itself from its parent when destroyed
  v.off = off;
  v.keys = std::move(keys);
  for (int i = 0; i < static_cast<int>(v.keys.size()); ++i) {
    auto t = std::make_unique<BoardTile>(path, i, pp.blocks[static_cast<std::size_t>(i)], controller_);
    if (t->isCapture() && !t->toneId().empty()) {  // the setting selector: the models of the capture's tone that are in the cache
      auto models = cachedModelsOf(t->toneId());
      if (models.size() >= 2) t->setSettings(std::move(models), t->modelId());
    }
    t->onSetting = [this](BoardTile& tile, const std::string& modelId) {
      if (const auto cap = cachedToneCapture(tile.toneId(), modelId)) swapPedalCapture(tile.path(), tile.blockId(), *cap);
      else say("that setting is not in the capture cache");
    };
    t->onSelect = [this](BoardTile& tile) {
      closePicker();
      if (onSelect) onSelect(tile);
    };
    t->onDoubleClick = [this](BoardTile& tile) {
      if (onTileDoubleClick) onTileDoubleClick(tile);
    };
    t->onBypass = [this](BoardTile& tile, bool bypass) { setPedalBypass(tile.path(), tile.blockId(), bypass); };
    t->onContextMenu = [this](BoardTile& tile) { showTileMenu(tile); };
    t->onGesture = [this](BoardTile& tile, BoardTile::Gesture g, const juce::MouseEvent& e) { gesture(tile, g, e); };
    v.strip->addAndMakeVisible(*t);
    v.tiles.push_back(std::move(t));
  }
  v.add->setVisible(!off);
  layoutPath(path);
  applySelection();
  repaint();
}

void Pedalboard::layoutPath(int path) {
  PathView& v = paths_[static_cast<std::size_t>(path)];
  const auto inner = boardBounds(path).reduced(kBoardPad);
  v.viewport->setBounds(inner);
  v.viewport->setVisible(!v.off);
  const auto rects = slotRects(path, static_cast<int>(v.tiles.size()), !v.off);
  v.strip->setSize(std::max(inner.getWidth(), rects.back().getRight()), inner.getHeight() - kScrollBar);
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

// --- editing: one RigController::edit each ----------------------------------------------------------------------------------------
bool Pedalboard::setPedalBypass(int path, const std::string& id, bool bypass) {
  if (!hasTile(path, id)) return false;
  controller_.edit([path, id, bypass](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    for (std::size_t i = 0; i < pp.blocks.size(); ++i)
      if (pp.blocks[i].id == id) {
        setBypass(pp, static_cast<int>(i), bypass);
        return;
      }
  });
  refreshNow();
  return true;
}

bool Pedalboard::removePedal(int path, const std::string& id) {
  if (!hasTile(path, id)) return false;  // nothing to remove: no needless reload
  controller_.edit([path, id](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    const int tiles = boardBlockCount(pp);
    for (int i = 0; i < tiles; ++i)
      if (pp.blocks[static_cast<std::size_t>(i)].id == id) {
        removeBlock(pp, i);
        return;
      }
  });
  refreshNow();
  return true;
}

bool Pedalboard::movePedal(int path, const std::string& id, int toPath, int toIndex) {
  return moveImpl(path, id, toPath, nullptr, toIndex, nullptr);
}

bool Pedalboard::moveImpl(int path, const std::string& id, int toPath, const Drop* anchor, int toIndex, bool* cancelled) {
  const PathView& dst = paths_[static_cast<std::size_t>(toPath)];
  if (dst.off) return false;  // a board that is off is not a drop target
  if (anchor == nullptr && !hasTile(path, id)) return false;  // an id that is not on the board moves nothing
  if (path != toPath && pathFull(toPath)) {
    say(juce::String(toPath == 0 ? "SAW" : "BODY") + " path full: 8 blocks");
    return false;
  }
  // Everything below is decided on the preset as it is NOW, by id: the tiles the drag started from may be older than the rig.
  bool edited = false, lost = false;
  controller_.edit([&](Preset& p) {
    PathPreset& src = path == 0 ? p.a : p.b;
    PathPreset& dstPath = toPath == 0 ? p.a : p.b;
    const int srcTiles = boardBlockCount(src);
    int from = -1;
    for (int i = 0; i < srcTiles; ++i)
      if (src.blocks[static_cast<std::size_t>(i)].id == id) from = i;
    if (from < 0) {
      lost = anchor != nullptr;  // the dragged pedal is gone: cancel
      return;
    }
    // The final index among the target's tiles once the dragged pedal is out of the way.
    const int dstTiles = boardBlockCount(dstPath);
    int index = toIndex;
    if (anchor != nullptr) {
      std::vector<const Block*> others;
      for (int i = 0; i < dstTiles; ++i) {
        const Block& b = dstPath.blocks[static_cast<std::size_t>(i)];
        if (!(path == toPath && b.id == id)) others.push_back(&b);
      }
      auto find = [&others](const std::string& x) {
        for (std::size_t i = 0; i < others.size(); ++i)
          if (others[i]->id == x) return static_cast<int>(i);
        return -1;
      };
      index = -1;
      if (!anchor->beforeId.empty()) index = find(anchor->beforeId);
      if (index < 0 && !anchor->afterId.empty() && (anchor->beforeId.empty() || find(anchor->afterId) >= 0)) {
        const int after = find(anchor->afterId);
        if (after >= 0) index = after + 1;
      }
      if (index < 0 && anchor->beforeId.empty() && anchor->afterId.empty() && others.empty()) index = 0;  // an empty board
      if (index < 0) {  // the neighbours the user aimed between are gone: not a place we can name
        lost = true;
        return;
      }
    }
    if (path == toPath) {
      const int to = std::clamp(index, 0, srcTiles - 1);
      if (to == from) return;  // nothing moves
      moveBlock(src, from, to);
      edited = true;
      return;
    }
    if (static_cast<int>(dstPath.blocks.size()) >= kMaxBlocksPerPath) return;
    Block b = src.blocks[static_cast<std::size_t>(from)];
    removeBlock(src, from);
    b.id = newBlockId(p, toPath == 0 ? 'a' : 'b');  // a fresh id; everything else (params, bypass, capture, make-up) is kept
    if (b.slot.empty() && b.type == "nam") b.slot = "pedal";  // stays a pedal whatever the target's blocks
    addBlock(dstPath, std::clamp(index, 0, boardBlockCount(dstPath)), std::move(b));
    edited = true;
  });
  if (cancelled != nullptr) *cancelled = lost;
  if (edited || lost) refreshNow();
  return edited;
}

bool Pedalboard::addModeledPedal(int path, const std::string& type) {
  if (paths_[static_cast<std::size_t>(path)].off) return false;
  if (pathFull(path)) {
    say(juce::String(path == 0 ? "SAW" : "BODY") + " path full: 8 blocks");
    return false;
  }
  Block proto;
  try {
    proto = PedalPicker::makeModeledBlock(type);
  } catch (const std::exception& ex) {
    say(juce::String("cannot add this pedal: ") + ex.what());
    return false;
  }
  controller_.edit([path, proto](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    if (static_cast<int>(pp.blocks.size()) >= kMaxBlocksPerPath) return;
    Block b = proto;
    b.id = newBlockId(p, path == 0 ? 'a' : 'b');
    addBlock(pp, boardBlockCount(pp), std::move(b));  // before the amp, at the end of the board
  });
  refreshNow();
  return true;
}

namespace {
// Sets the make-up of the nam block `id` wherever it is in `p` (no-op when it is not there / not a nam block).
void setMakeupById(Preset& p, const std::string& id, double makeupDb) {
  for (int path = 0; path < 2; ++path) {
    const PathPreset& pp = path == 0 ? p.a : p.b;
    for (std::size_t i = 0; i < pp.blocks.size(); ++i)
      if (pp.blocks[i].id == id) p = withSlotMakeup(p, path, static_cast<int>(i), makeupDb);
  }
}
}  // namespace

bool Pedalboard::addCapturePedal(int path, const Capture& capture) {
  if (paths_[static_cast<std::size_t>(path)].off) return false;
  if (pathFull(path)) {
    say(juce::String(path == 0 ? "SAW" : "BODY") + " path full: 8 blocks");
    return false;
  }
  SawbladeProcessor& proc = controller_.processor();
  const Preset before = proc.editBasePreset();
  std::string newId;
  controller_.edit([path, capture, &newId](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    if (static_cast<int>(pp.blocks.size()) >= kMaxBlocksPerPath) return;
    Block b;
    b.id = newBlockId(p, path == 0 ? 'a' : 'b');
    b.type = "nam";
    b.slot = "pedal";
    auto np = std::make_shared<NamBlockParams>();
    np->model = capture;
    b.params = std::move(np);
    newId = b.id;
    addBlock(pp, boardBlockCount(pp), std::move(b));
  });
  refreshNow();
  if (newId.empty() || !proc.levelMatchEnabled()) return !newId.empty();
  // LEVEL MATCH: the pedal drops in at matched loudness. The add is the undo step; the make-up (the path alone, before vs after) lands when
  // the background measurement is done and adds none: it goes into the rig and into every stored snapshot that has the block.
  const Preset after = proc.editBasePreset();
  say("LEVEL MATCHING...");
  proc.computeSlotMakeup(before, after, path, [this, alive = alive_, newId](const LevelWorker::MakeupResult& r) {
    juce::MessageManager::callAsync([this, alive, newId, mk = r.makeupDb] {
      if (!alive->load()) return;
      say(juce::String());
      if (!mk) return;
      SawbladeProcessor& pr = controller_.processor();
      Preset cur = pr.editBasePreset();
      bool there = false;
      for (const PathPreset* pp : {&cur.a, &cur.b})
        for (const Block& b : pp->blocks) there = there || b.id == newId;
      if (!there) return;  // removed meanwhile
      pr.patchHistory([&newId, mk](Preset& snap) { setMakeupById(snap, newId, *mk); });
      setMakeupById(cur, newId, *mk);
      pr.loadPreset(std::move(cur), /*keepMonitor=*/true);  // not a user edit: records no undo step
      refreshNow();
    });
  });
  return true;
}

bool Pedalboard::swapPedalCapture(int path, const std::string& id, const Capture& capture) {
  SawbladeProcessor& proc = controller_.processor();
  const Preset cur = proc.editBasePreset();
  const PathPreset& pp = path == 0 ? cur.a : cur.b;
  int idx = -1;
  for (int i = 0; i < boardBlockCount(pp); ++i)
    if (pp.blocks[static_cast<std::size_t>(i)].id == id && pp.blocks[static_cast<std::size_t>(i)].type == "nam") idx = i;
  if (idx < 0) return false;
  const auto replace = [path, id, capture](Preset& p, std::optional<double> makeup) {
    PathPreset& q = path == 0 ? p.a : p.b;
    for (std::size_t i = 0; i < q.blocks.size(); ++i)
      if (q.blocks[i].id == id)
        if (const auto* nam = dynamic_cast<const NamBlockParams*>(q.blocks[i].params.get())) {
          auto np = std::make_shared<NamBlockParams>(*nam);
          np->model = capture;
          if (makeup) np->makeupDb = *makeup;
          q.blocks[i].params = std::move(np);
        }
  };
  const std::uint64_t seq = ++swapSeq_;
  if (!proc.levelMatchEnabled()) {
    controller_.edit([replace](Preset& p) { replace(p, std::nullopt); });
    refreshNow();
    return true;
  }
  // LEVEL MATCH: measure the path with the new capture (make-up 0) against the old one, then swap with the make-up in ONE step.
  Preset swapped = cur;
  replace(swapped, 0.0);
  say("LEVEL MATCHING...");
  proc.computeSlotMakeup(cur, swapped, path, [this, alive = alive_, seq, replace](const LevelWorker::MakeupResult& r) {
    juce::MessageManager::callAsync([this, alive, seq, replace, mk = r.makeupDb] {
      if (!alive->load() || seq != swapSeq_) return;
      say(juce::String());
      controller_.edit([replace, mk](Preset& p) { replace(p, mk ? mk : std::optional<double>()); });
      refreshNow();
    });
  });
  return true;
}

bool Pedalboard::setPedalLevel(int path, const std::string& id, double db) {
  if (!hasTile(path, id)) return false;
  controller_.edit([path, id, db](Preset& p) {
    PathPreset& pp = path == 0 ? p.a : p.b;
    for (std::size_t i = 0; i < pp.blocks.size(); ++i)
      if (pp.blocks[i].id == id) setBlockOutputGainDb(pp, static_cast<int>(i), db);
  });
  refreshNow();
  return true;
}

juce::PopupMenu Pedalboard::menuFor(const BoardTile& t) const {
  juce::PopupMenu m;
  m.addItem(kMenuBypass, "BYPASS", true, t.bypassed());
  m.addItem(kMenuRemove, "REMOVE");
  return m;
}

void Pedalboard::applyMenuChoice(int path, const std::string& id, int choice) {
  if (choice == kMenuRemove) {
    removePedal(path, id);
    return;
  }
  if (choice != kMenuBypass) return;
  for (auto& t : paths_[static_cast<std::size_t>(path)].tiles)
    if (t->blockId() == id) {
      setPedalBypass(path, id, !t->bypassed());
      return;
    }
}

void Pedalboard::showTileMenu(BoardTile& t) {
  closePicker();
  menuFor(t).showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&t),
                           [safe = juce::Component::SafePointer<Pedalboard>(this), path = t.path(), id = t.blockId()](int result) {
                             if (safe != nullptr && result != 0) safe->applyMenuChoice(path, id, result);
                           });
}

// --- the picker ---------------------------------------------------------------------------------------------------------------------
void Pedalboard::showPicker(int path) {
  juce::Component* parent = getParentComponent();
  if (parent == nullptr || paths_[static_cast<std::size_t>(path)].off || pathFull(path)) return;
  if (!picker_) {
    picker_ = std::make_unique<PedalPicker>();
    picker_->onPickModeled = [this](const std::string& type) {
      const int p = picker_->path();
      closePicker();
      addModeledPedal(p, type);
    };
    picker_->onPickCapture = [this](const CachedPedal& tone) {
      const int p = picker_->path();
      closePicker();
      const auto cap = tone.models.empty() ? std::nullopt : cachedToneCapture(tone.toneId, tone.models.front().modelId);
      if (cap) addCapturePedal(p, *cap);
      else say("that capture is not in the capture cache any more");
    };
    picker_->onSearch = [this] {
      const int p = picker_->path();
      closePicker();
      if (onSearchRequest) onSearchRequest(p, tileCount(p));
    };
    picker_->onClose = [this] { closePicker(); };
  }
  if (picker_->getParentComponent() != parent) parent->addChildComponent(*picker_);
  picker_->setPath(path);
  const auto board = boardBounds(path);
  const int x = std::clamp(slotBounds(path, tileCount(path)).getX(), board.getX() + 8, board.getRight() - PedalPicker::kWidth - 8);
  picker_->setBounds(parent->getLocalArea(this, juce::Rectangle<int>(x, board.getY() + 8, PedalPicker::kWidth, PedalPicker::kHeight)));
  picker_->setVisible(true);
  picker_->toFront(true);
}

void Pedalboard::closePicker() {
  if (picker_ != nullptr) picker_->setVisible(false);
}

// --- dragging -----------------------------------------------------------------------------------------------------------------------
void Pedalboard::gesture(BoardTile& t, BoardTile::Gesture g, const juce::MouseEvent& e) {
  const juce::Point<float> p = getLocalPoint(&t, e.position);
  switch (g) {
    case BoardTile::Gesture::Down:
      closePicker();
      abortDrag();  // a previous drag that never ended: its tile is shown normally again
      drag_ = Drag{};
      drag_.tile = &t;
      drag_.down = drag_.pos = p;
      drag_.downMs = juce::Time::getMillisecondCounter();
      break;
    case BoardTile::Gesture::Drag:
      if (drag_.tile != &t) return;
      drag_.pos = p;
      if (!drag_.active && p.getDistanceFrom(drag_.down) >= static_cast<float>(kDragThreshold)) startDrag(t);
      if (drag_.active) {
        for (auto& v : paths_) {  // near a scrolling board's edge: scroll it
          if (!v.viewport->isVisible()) continue;
          const auto r = v.viewport->getBounds();
          if (p.y < static_cast<float>(r.getY()) || p.y > static_cast<float>(r.getBottom())) continue;
          if (p.x < static_cast<float>(r.getX() + 24)) v.viewport->setViewPosition(std::max(0, v.viewport->getViewPositionX() - 14), 0);
          else if (p.x > static_cast<float>(r.getRight() - 24)) v.viewport->setViewPosition(v.viewport->getViewPositionX() + 14, 0);
        }
        drag_.drop = computeDrop(p);
        repaint();
      }
      break;
    case BoardTile::Gesture::Up:
      if (drag_.tile != &t) return;
      endDrag(true);
      break;
  }
}

void Pedalboard::abortDrag() {
  if (drag_.tile != nullptr && drag_.active) drag_.tile->setAlpha(1.0f);
  drag_ = Drag{};
  repaint();
}

void Pedalboard::startDrag(BoardTile& t) {
  drag_.active = true;
  drag_.grab = drag_.down - tileBounds(t).getPosition().toFloat();
  drag_.ghost = t.createComponentSnapshot(t.getLocalBounds(), true, 1.0f);
  t.setAlpha(0.35f);
}

void Pedalboard::endDrag(bool drop) {
  Drag d = std::move(drag_);
  drag_ = Drag{};
  const bool pending = refreshPending_;
  refreshPending_ = false;
  repaint();
  if (d.tile == nullptr) return;
  d.tile->setAlpha(1.0f);  // the tile was not rebuilt during the drag
  const std::string id = d.tile->blockId();
  const int path = d.tile->path();
  bool edited = false, cancelled = false;
  if (drop && d.active) {
    switch (d.drop.kind) {
      case Drop::Kind::Remove: edited = removePedal(path, id); break;
      case Drop::Kind::Insert: edited = moveImpl(path, id, d.drop.path, &d.drop, d.drop.index, &cancelled); break;
      case Drop::Kind::Refused: say(juce::String(d.drop.path == 0 ? "SAW" : "BODY") + " path full: 8 blocks"); break;
      case Drop::Kind::Cancel: break;
    }
  }
  if (!edited && (pending || cancelled)) refreshNow();
}

Pedalboard::Drop Pedalboard::computeDrop(juce::Point<float> p) const {
  Drop d;
  const BoardTile* dragged = drag_.tile;
  for (int path = 0; path < 2; ++path) {
    if (!boardBounds(path).contains(p.toInt())) continue;
    const PathView& v = paths_[static_cast<std::size_t>(path)];
    d.path = path;
    if (v.off) return d;  // a board that is off is not a drop target: cancel
    if (dragged != nullptr && path != dragged->path() && pathFull(path)) {
      d.kind = Drop::Kind::Refused;
      return d;
    }
    std::vector<const BoardTile*> others;
    for (const auto& t : v.tiles)
      if (t.get() != dragged) others.push_back(t.get());
    int idx = 0;
    for (const BoardTile* o : others)
      if (static_cast<float>(tileBounds(*o).getCentreX()) < p.x) ++idx;
    float x = static_cast<float>(v.viewport->getX()) + 2.0f;
    if (!others.empty()) x = idx < static_cast<int>(others.size()) ? static_cast<float>(tileBounds(*others[static_cast<std::size_t>(idx)]).getX() - kTileGap / 2)
                                                                   : static_cast<float>(tileBounds(*others.back()).getRight() + kTileGap / 2);
    const auto vr = v.viewport->getBounds();
    d.kind = Drop::Kind::Insert;
    d.index = idx;
    if (idx < static_cast<int>(others.size())) d.beforeId = others[static_cast<std::size_t>(idx)]->blockId();
    if (idx > 0) d.afterId = others[static_cast<std::size_t>(idx - 1)]->blockId();
    d.bar = juce::Rectangle<float>(x - 1.5f, static_cast<float>(vr.getY() + 6), 3.0f, static_cast<float>(vr.getHeight() - 12 - kScrollBar));
    return d;
  }
  d.kind = Drop::Kind::Remove;  // outside both boards
  return d;
}

Pedalboard::DragInfo Pedalboard::dragInfo() const {
  DragInfo i;
  i.tile = drag_.tile;
  i.pressed = drag_.tile != nullptr;
  i.active = drag_.active;
  if (!drag_.active) return i;
  i.removing = drag_.drop.kind == Drop::Kind::Remove;
  i.refused = drag_.drop.kind == Drop::Kind::Refused;
  if (drag_.drop.kind == Drop::Kind::Insert || i.refused) i.targetPath = drag_.drop.path;
  if (drag_.drop.kind == Drop::Kind::Insert) i.insertIndex = drag_.drop.index;
  return i;
}

// --- painting -----------------------------------------------------------------------------------------------------------------------
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

// The drag: the target board's outline, the insertion bar, the ghost (and what dropping it does).
void Pedalboard::paintOverChildren(juce::Graphics& g) {
  if (!drag_.active) return;
  const Drop& d = drag_.drop;
  if (d.kind == Drop::Kind::Insert || d.kind == Drop::Kind::Refused) {
    g.setColour((d.kind == Drop::Kind::Refused ? L::error() : d.path == 0 ? L::saw() : L::body()).withAlpha(0.9f));
    g.drawRoundedRectangle(boardBounds(d.path).toFloat().reduced(1.0f), 11.0f, 3.0f);
  }
  if (d.kind == Drop::Kind::Insert) {
    g.setColour(L::text());
    g.fillRoundedRectangle(d.bar, 1.5f);
  }
  const juce::Point<float> at = drag_.pos - drag_.grab;
  if (drag_.ghost.isValid()) {
    g.setOpacity(0.6f);
    g.drawImageAt(drag_.ghost, juce::roundToInt(at.x), juce::roundToInt(at.y));
  }
  const bool remove = d.kind == Drop::Kind::Remove, refused = d.kind == Drop::Kind::Refused;
  if (remove || refused) {
    const juce::Rectangle<float> pill(at.x, at.y - 24.0f, 150.0f, 20.0f);
    g.setColour(juce::Colour(0xff121110).withAlpha(0.9f));
    g.fillRoundedRectangle(pill, 4.0f);
    g.setColour(L::error());
    g.setFont(L::labelFont(12.0f));
    g.drawText(remove ? "REMOVE" : "PATH FULL: 8 BLOCKS", pill.toNearestInt(), juce::Justification::centred);
  }
}

}  // namespace sawblade::plugin::rig
