#include "PedalSwitch.h"

#include <algorithm>
#include <cmath>

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin {
using L = SawbladeLookAndFeel;
namespace { constexpr float kCaptionW = 64.0f; }

PedalSwitch::PedalSwitch(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& name,
                         std::vector<juce::String> texts)
    : paramId_(paramId), name_(name), texts_(std::move(texts)) {
  auto* p = apvts.getParameter(paramId_);
  jassert(p != nullptr);
  const int n = p != nullptr ? static_cast<int>(std::lround(p->convertFrom0to1(1.0f) - p->convertFrom0to1(0.0f))) + 1 : 2;
  if (texts_.empty())
    for (int i = 0; i < n; ++i) texts_.push_back((p != nullptr ? p->getText(p->convertTo0to1(static_cast<float>(i)), 32) : juce::String(i)).toUpperCase());
  for (int i = 0; i < static_cast<int>(texts_.size()); ++i) values_.push_back(static_cast<double>(i));
  init(apvts);
}

PedalSwitch::PedalSwitch(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& name, const FaceSwitchSpec& focus)
    : paramId_(paramId), name_(name), texts_{focus.wideText, focus.narrowText}, values_{focus.wideValue, focus.narrowValue}, focus_(true), spec_(focus) {
  init(apvts);
}

PedalSwitch::~PedalSwitch() = default;

void PedalSwitch::init(juce::AudioProcessorValueTreeState& apvts) {
  setTitle(name_);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(false);
  if (auto* p = apvts.getParameter(paramId_)) {
    attachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float v) {
      value_ = static_cast<double>(v);
      update();
    });
    attachment_->sendInitialUpdate();
  }
  update();
}

int PedalSwitch::position() const {
  if (focus_) return spec_.isNarrow(value_) ? 1 : 0;
  return std::clamp(static_cast<int>(std::lround(value_)), 0, numPositions() - 1);
}

juce::String PedalSwitch::valueText() const { return texts_[static_cast<std::size_t>(position())]; }

void PedalSwitch::update() {
  const juce::String v = valueText();
  setTitle(name_ + ": " + v);
  setTooltip(name_ + ": " + v + " (click to change, mouse wheel to step)");
  repaint();
}

void PedalSwitch::setPosition(int index) {
  index = std::clamp(index, 0, numPositions() - 1);
  if (attachment_ != nullptr) attachment_->setValueAsCompleteGesture(static_cast<float>(values_[static_cast<std::size_t>(index)]));
}

void PedalSwitch::step(int direction, bool wrap) {
  const int n = numPositions();
  int i = position() + direction;
  if (wrap) i = (i % n + n) % n;
  setPosition(std::clamp(i, 0, n - 1));
}

void PedalSwitch::setValueText(TextSide side, int heightPx, float fontPx) {
  side_ = side;
  textH_ = heightPx;
  fontPx_ = fontPx;
  repaint();
}

void PedalSwitch::setCaption(const juce::String& caption) {
  caption_ = caption;
  repaint();
}

void PedalSwitch::mouseDown(const juce::MouseEvent&) { step(+1, true); }

void PedalSwitch::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
  if (w.deltaY > 0.0f) step(+1, false);
  else if (w.deltaY < 0.0f) step(-1, false);
}

juce::Rectangle<float> PedalSwitch::leverBox() const {
  auto b = getLocalBounds().toFloat();
  if (caption_.isNotEmpty()) {  // row layout: caption, lever, value text
    const float s = std::min(b.getHeight(), 30.0f);
    return juce::Rectangle<float>(s, s).withCentre({b.getX() + kCaptionW + s * 0.5f, b.getCentreY()});
  }
  if (textH_ > 0) {
    if (side_ == TextSide::Below) b.removeFromBottom(static_cast<float>(textH_));
    else b.removeFromTop(static_cast<float>(textH_));
  }
  const float s = std::min(b.getWidth(), b.getHeight());
  return juce::Rectangle<float>(s, s).withCentre(b.getCentre());
}

void PedalSwitch::paint(juce::Graphics& g) {
  const auto box = leverBox();
  const auto c = box.getCentre();
  const float r = box.getWidth() * 0.5f;

  // Mounting plate and bezel.
  g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a4640), c.x, box.getY(), juce::Colour(0xff0e0d0c), c.x, box.getBottom(), false));
  g.fillEllipse(box);
  g.setColour(juce::Colour(0xff0a0908));
  g.fillEllipse(box.reduced(r * 0.22f));
  g.setColour(L::chipBorder());
  g.drawEllipse(box.reduced(0.5f), 1.0f);

  // Lever: tilts across the positions (first = left), a bright tip.
  const int n = std::max(2, numPositions());
  const float t = n > 1 ? static_cast<float>(position()) / static_cast<float>(n - 1) : 0.5f;
  const float angle = juce::degreesToRadians((t - 0.5f) * 2.0f * 38.0f);
  const float len = r * 0.95f;
  const juce::Point<float> tip(c.x + std::sin(angle) * len, c.y - std::cos(angle) * len);
  g.setColour(juce::Colours::black.withAlpha(0.6f));
  g.drawLine(c.x + 0.8f, c.y + 0.8f, tip.x + 0.8f, tip.y + 0.8f, std::max(1.6f, r * 0.28f));
  g.setColour(juce::Colour(0xffcfc8b8));
  g.drawLine(c.x, c.y, tip.x, tip.y, std::max(1.4f, r * 0.24f));
  const float tr = std::max(1.5f, r * 0.2f);
  g.setColour(position() == 0 ? juce::Colour(0xffe8e1d2) : L::saw());
  g.fillEllipse(tip.x - tr, tip.y - tr, tr * 2, tr * 2);

  if (caption_.isNotEmpty()) {
    g.setColour(L::dimText());
    g.setFont(L::labelFont(11.0f));
    g.drawText(caption_, 0, 0, static_cast<int>(kCaptionW), getHeight(), juce::Justification::centredLeft);
    g.setColour(L::text());
    g.setFont(L::labelFont(fontPx_));
    const int x = static_cast<int>(kCaptionW + box.getWidth() + 8.0f);
    g.drawFittedText(valueText(), x, 0, getWidth() - x, getHeight(), juce::Justification::centredLeft, 1, 0.7f);
  } else if (textH_ > 0) {
    auto area = getLocalBounds().toFloat();
    area = side_ == TextSide::Below ? area.removeFromBottom(static_cast<float>(textH_)) : area.removeFromTop(static_cast<float>(textH_));
    g.setColour(L::text());
    g.setFont(L::labelFont(fontPx_));
    g.drawFittedText(valueText(), area.toNearestInt(), juce::Justification::centred, 1, 0.7f);
  }
}

}  // namespace sawblade::plugin
