#include "AdvancedDrawer.h"

#include <algorithm>

#include "../SawbladeLookAndFeel.h"
#include "CircuitFaces.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
constexpr int kPad = 16, kTitleH = 26, kRowGap = 96;
}  // namespace

AdvancedDrawer::AdvancedDrawer(SawbladeProcessor& p) : proc_(p) {
  setTitle("Advanced");
  setWantsKeyboardFocus(true);
  setOpaque(false);
  animator_.addChangeListener(this);
  auto& apvts = proc_.parameters();

  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    for (const FaceKnob& dk : f.drawerKnobs) {
      const ParamSpec& s = paramSpec(dk.param);
      Cell cell;
      cell.knob = std::make_unique<skin::FilmstripKnob>(apvts, s.id, s.name, skin::FilmstripKnob::Kind::Pedal, L::saw());
      cell.name = std::make_unique<juce::Label>();
      cell.read = std::make_unique<juce::Label>();
      cell.name->setText(dk.label, juce::dontSendNotification);
      cell.name->setFont(L::labelFont(11.0f));
      cell.name->setColour(juce::Label::textColourId, L::text());
      cell.read->setFont(L::monoFont(11.0f));
      cell.read->setColour(juce::Label::textColourId, L::dimText());
      for (juce::Label* l : {cell.name.get(), cell.read.get()}) {
        l->setJustificationType(juce::Justification::centred);
        l->setInterceptsMouseClicks(false, false);
        addChildComponent(*l);
      }
      addChildComponent(*cell.knob);
      cell.knob->onValueChange = [this] { updateReadouts(); };
      cells_[static_cast<size_t>(c)].push_back(std::move(cell));
    }
    for (const FaceKnob& ds : f.drawerSwitches) {
      auto sw = std::make_unique<PedalSwitch>(apvts, paramSpec(ds.param).id, juce::String(ds.label), std::vector<juce::String>{});
      sw->setCaption(ds.label);
      sw->setValueText(PedalSwitch::TextSide::Below, 0, 11.0f);
      addChildComponent(*sw);
      switches_[static_cast<size_t>(c)].push_back(std::move(sw));
    }
  }

  close_.setButtonText(juce::String::fromUTF8("\xc3\x97"));
  close_.setTitle("Close advanced drawer");
  close_.setTooltip("Close the advanced controls (double-click the pedal or press Escape)");
  close_.onClick = [this] { setOpen(false); };
  addAndMakeVisible(close_);
  setVisible(false);
}

AdvancedDrawer::~AdvancedDrawer() { animator_.removeChangeListener(this); }

void AdvancedDrawer::setAnchor(juce::Rectangle<int> pedal, juce::Rectangle<int> limit) {
  const int x = pedal.getRight() + kGap;
  const int right = std::max(0, limit.getRight() - x);
  const int left = std::max(0, pedal.getX() - kGap - limit.getX());
  openLeft_ = right < kMinOpenWidth && left > right;
  if (openLeft_) {
    const int w = std::min(kPreferredWidth, left);
    open_ = juce::Rectangle<int>(pedal.getX() - kGap - w, pedal.getY(), w, pedal.getHeight());
  } else {
    open_ = juce::Rectangle<int>(x, pedal.getY(), right, pedal.getHeight());
  }
  finishAnimation();
  if (wantOpen_) setBounds(open_);
}

juce::Rectangle<int> AdvancedDrawer::closedBounds() const {
  return openLeft_ ? juce::Rectangle<int>(open_.getRight() - 8, open_.getY(), 8, open_.getHeight()) : open_.withWidth(8);
}

juce::String AdvancedDrawer::title() const {
  return "ADVANCED" + kDot + (active_ ? juce::String(circuitFace(*active_).oledName) : juce::String("NO CIRCUIT"));
}

void AdvancedDrawer::setOpen(bool open, bool animate) {
  if (open == wantOpen_ && (open ? isVisible() : true)) return;
  wantOpen_ = open;
  const auto closed = closedBounds();
  if (open) {
    refresh();
    if (!isVisible()) {
      setBounds(closed);
      setAlpha(0.0f);
      setVisible(true);
      toFront(false);
    }
    if (animate) animator_.animateComponent(this, open_, 1.0f, kAnimationMs, false, 1.0, 1.0);
    else { animator_.cancelAnimation(this, false); setBounds(open_); setAlpha(1.0f); }
    grabKeyboardFocus();
  } else {
    if (animate && isVisible()) animator_.animateComponent(this, closed, 0.0f, kAnimationMs, false, 1.0, 1.0);
    else { animator_.cancelAnimation(this, false); setVisible(false); }
  }
}

void AdvancedDrawer::finishAnimation() {
  animator_.cancelAllAnimations(true);
  if (wantOpen_) {
    setBounds(open_);
    setAlpha(1.0f);
  } else {
    setVisible(false);
  }
}

void AdvancedDrawer::changeListenerCallback(juce::ChangeBroadcaster*) {
  if (!wantOpen_ && !animator_.isAnimating(this)) setVisible(false);
}

bool AdvancedDrawer::keyPressed(const juce::KeyPress& k) {
  if (k == juce::KeyPress::escapeKey && wantOpen_) {
    setOpen(false);
    return true;
  }
  return false;
}

void AdvancedDrawer::refresh() {
  const auto slot = proc_.circuitSlot();
  const std::optional<Circuit> next = slot ? std::optional<Circuit>(slot->circuit) : std::nullopt;
  if (next != active_) {
    active_ = next;
    showActive();
    resized();
  }
  updateReadouts();
  repaint();
}

void AdvancedDrawer::showActive() {
  for (int c = 0; c < kNumCircuits; ++c) {
    const bool on = active_ && static_cast<int>(*active_) == c;
    for (Cell& cell : cells_[static_cast<size_t>(c)])
      for (juce::Component* w : {static_cast<juce::Component*>(cell.knob.get()), static_cast<juce::Component*>(cell.name.get()),
                                 static_cast<juce::Component*>(cell.read.get())})
        w->setVisible(on);
    for (auto& sw : switches_[static_cast<size_t>(c)]) sw->setVisible(on);
  }
}

void AdvancedDrawer::updateReadouts() {
  for (auto& circuit : cells_)
    for (Cell& cell : circuit)
      if (cell.knob->isVisible() || !active_) cell.read->setText(cell.knob->getTextFromValue(cell.knob->getValue()), juce::dontSendNotification);
}

void AdvancedDrawer::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat();
  g.setColour(juce::Colour(0xff121110));
  g.fillRoundedRectangle(b, 10.0f);
  g.setColour(L::chipBorder());
  g.drawRoundedRectangle(b.reduced(0.5f), 10.0f, 1.0f);
  g.setColour(L::saw());
  g.fillRect(juce::Rectangle<float>(b.getX() + kPad, b.getY() + kTitleH + 4.0f, 28.0f, 2.0f));
  g.setColour(L::text());
  g.setFont(L::titleFont(14.0f));
  g.drawText(title(), kPad, 6, 400, kTitleH - 4, juce::Justification::centredLeft);
  if (!active_ || (cells_[static_cast<size_t>(*active_)].empty() && switches_[static_cast<size_t>(*active_)].empty())) {
    g.setColour(L::dimText());
    g.setFont(L::bodyFont(13.0f));
    g.drawText(active_ ? "This circuit has no advanced controls." : "This preset has no pedal circuit block.", getLocalBounds().reduced(kPad),
               juce::Justification::centred);
  }
}

void AdvancedDrawer::resized() {
  // The layout is that of the open drawer whatever the current width: during the slide the contents are
  // revealed from the left rather than squeezed.
  const int w = open_.getWidth() > 0 ? open_.getWidth() : getWidth();
  close_.setBounds(w - 36 - 6, 6, 30, 26);
  const int cellW = (w - 2 * kPad) / 5;
  for (auto& circuit : cells_)
    for (size_t i = 0; i < circuit.size(); ++i) {
      const int row = static_cast<int>(i) / 5, col = static_cast<int>(i) % 5;
      const int cx = kPad + col * cellW + cellW / 2;
      const int y = kTitleH + 12 + row * kRowGap;
      Cell& c = circuit[i];
      c.knob->setBounds(cx - kKnobPx / 2, y, kKnobPx, kKnobPx);
      c.name->setBounds(cx - cellW / 2, y + kKnobPx + 1, cellW, 14);
      c.read->setBounds(cx - cellW / 2, y + kKnobPx + 15, cellW, 14);
    }
  for (auto& circuit : switches_)
    for (size_t i = 0; i < circuit.size(); ++i) {
      const int sw = 200;
      circuit[i]->setBounds(kPad + static_cast<int>(i) * (sw + 16), open_.getHeight() > 0 ? open_.getHeight() - 40 : getHeight() - 40, sw, 36);
    }
}

}  // namespace sawblade::plugin
