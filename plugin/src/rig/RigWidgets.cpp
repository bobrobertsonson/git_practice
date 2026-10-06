#include "rig/RigWidgets.h"

#include <cmath>

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;
}

// --- Segmented -------------------------------------------------------------------------------------
void Segmented::setItems(const std::vector<Item>& items, juce::Colour onColour) {
  buttons_.clear();
  weights_.clear();
  removeAllChildren();
  for (std::size_t i = 0; i < items.size(); ++i) {
    auto b = std::make_unique<juce::TextButton>(items[i].text);
    b->setButtonText(items[i].text);
    b->setTitle(items[i].title.isNotEmpty() ? items[i].title : items[i].text);
    b->setTooltip(items[i].tooltip);
    b->setColour(juce::TextButton::buttonOnColourId, onColour);
    const int idx = static_cast<int>(i);
    b->onClick = [this, idx] { setSelected(idx, true); };
    addAndMakeVisible(*b);
    weights_.push_back(items[i].weight);
    buttons_.push_back(std::move(b));
  }
  selected_ = -1;
  resized();
}

void Segmented::setSelected(int i, bool notify) {
  selected_ = i;
  for (std::size_t k = 0; k < buttons_.size(); ++k) buttons_[k]->setToggleState(static_cast<int>(k) == i, juce::dontSendNotification);
  if (notify && onChange) onChange(i);
}

void Segmented::resized() {
  float total = 0.0f;
  for (const float w : weights_) total += w;
  if (total <= 0.0f) return;
  const auto r = getLocalBounds();
  float x = 0.0f;
  const float gap = 3.0f;
  const float avail = static_cast<float>(r.getWidth()) - gap * static_cast<float>(buttons_.size() - 1);
  for (std::size_t i = 0; i < buttons_.size(); ++i) {
    const float w = avail * weights_[i] / total;
    buttons_[i]->setBounds(juce::roundToInt(x), 0, juce::roundToInt(x + w) - juce::roundToInt(x), r.getHeight());
    x += w + gap;
  }
}

// --- LedToggle -------------------------------------------------------------------------------------
LedToggle::LedToggle(const juce::String& text, const juce::String& tooltip, juce::Colour onColour) : juce::Button(text), on_(onColour) {
  setClickingTogglesState(true);
  setTitle(text);
  setTooltip(tooltip);
}

void LedToggle::paintButton(juce::Graphics& g, bool over, bool down) {
  auto r = getLocalBounds().toFloat().reduced(0.5f);
  const float a = isEnabled() ? 1.0f : 0.5f;
  g.setColour((down ? L::panelDeep().brighter(0.15f) : over ? L::panelDeep().brighter(0.06f) : L::panelDeep()).withMultipliedAlpha(a));
  g.fillRoundedRectangle(r, 4.0f);
  g.setColour((getToggleState() ? on_.withAlpha(0.7f) : L::chipBorder()).withMultipliedAlpha(a));
  g.drawRoundedRectangle(r, 4.0f, 1.0f);
  const float d = juce::jmin(10.0f, r.getHeight() * 0.4f);
  const juce::Rectangle<float> led(r.getX() + 8.0f, r.getCentreY() - d * 0.5f, d, d);
  if (getToggleState()) {
    g.setGradientFill(juce::ColourGradient(on_.withAlpha(0.55f), led.getCentre(), on_.withAlpha(0.0f), led.getCentre().translated(d * 1.2f, 0.0f), true));
    g.fillEllipse(led.expanded(d * 0.8f));
  }
  g.setColour((getToggleState() ? on_ : juce::Colour(0xff3a2a20)).withMultipliedAlpha(a));
  g.fillEllipse(led);
  g.setColour(L::background());
  g.drawEllipse(led, 1.0f);
  g.setColour((getToggleState() ? L::text() : L::dimText()).withMultipliedAlpha(a));
  g.setFont(L::labelFont(juce::jmin(12.0f, r.getHeight() * 0.5f)));
  g.drawText(getButtonText(), r.withTrimmedLeft(8.0f + d + 6.0f).withTrimmedRight(4.0f), juce::Justification::centredLeft, true);
}

// --- PresetKnob ------------------------------------------------------------------------------------
PresetKnob::PresetKnob(RigController& c, const juce::String& caption, skin::FilmstripKnob::Kind kind, juce::Colour arc,
                       const skin::FilmstripKnob::Range& range, Apply apply, bool live, Format format)
    : controller_(c), knob_(caption, kind, arc, range), apply_(std::move(apply)), format_(std::move(format)), live_(live), shown_(range.def) {
  addAndMakeVisible(knob_);
  for (juce::Label* l : {&caption_, &value_}) {
    l->setInterceptsMouseClicks(false, false);
    l->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*l);
  }
  caption_.setText(caption, juce::dontSendNotification);
  caption_.setFont(L::labelFont(11.0f));
  caption_.setColour(juce::Label::textColourId, L::text());
  caption_.setMinimumHorizontalScale(0.7f);
  value_.setFont(L::monoFont(11.0f));
  value_.setColour(juce::Label::textColourId, L::dimText());
  value_.setMinimumHorizontalScale(0.7f);
  // One gesture = mouse down .. mouse up (a double-click is one too; the wheel and typed values are one-event gestures).
  knob_.onDragStart = [this] {
    dragging_ = true;
    if (onGestureBegin) onGestureBegin();
  };
  knob_.onDragEnd = [this] {
    dragging_ = false;
    finishGesture();
    if (onGestureEnd) onGestureEnd();
  };
  knob_.onValueChange = [this] {
    updateText();
    if (updating_) return;
    if (live_) {
      submit(false);
    } else if (dragging_) {
      submit(true);  // during the drag: a rebuild only once the hand pauses (latest value wins)
    } else {         // wheel / typed value
      if (onGestureBegin) onGestureBegin();
      submit(true);
      if (onGestureEnd) onGestureEnd();
    }
  };
  updateText();
}

void PresetKnob::updateText() {
  const double v = knob_.getValue();
  value_.setText(format_ ? format_(v) : knob_.getTextFromValue(v), juce::dontSendNotification);
}

void PresetKnob::submit(bool debounced) {
  const double v = knob_.getValue();
  if (live_) {
    shown_ = v;
    controller_.live([a = apply_, v](Preset& p) { a(p, v); });
    return;
  }
  if (v == shown_) return;  // a click without a change submits nothing
  shown_ = v;
  auto f = [a = apply_, v](Preset& p) { a(p, v); };
  if (debounced) controller_.editDebounced(f);
  else controller_.edit(f);
}

// Mouse up: the final value goes to the controller now, one rebuild (it also flushes this drag's debounced edit). When the last
// debounced value already is the final one, flushing it is all there is to do.
void PresetKnob::finishGesture() {
  if (live_) return;
  if (knob_.getValue() != shown_) submit(false);
  else controller_.flushPending();
}

void PresetKnob::setValueFromPreset(double v) {
  if (dragging_ || knob_.isMouseButtonDown()) return;
  if (!live_ && controller_.hasPending()) return;
  if (std::fabs(knob_.getValue() - v) < 1e-12 && shown_ == v) return;
  updating_ = true;
  knob_.setValue(v, juce::dontSendNotification);
  updating_ = false;
  shown_ = knob_.getValue();
  updateText();
}

void PresetKnob::resized() {
  auto r = getLocalBounds();
  const int textH = 14;
  value_.setBounds(r.removeFromBottom(textH));
  caption_.setBounds(r.removeFromBottom(textH));
  const int d = juce::jmin(r.getWidth(), r.getHeight());
  knob_.setBounds(juce::Rectangle<int>(d, d).withCentre(r.getCentre()));
}

}  // namespace sawblade::plugin::rig
