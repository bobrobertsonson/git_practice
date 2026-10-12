#include "FilmstripKnob.h"

#include <algorithm>
#include <cmath>

namespace sawblade::plugin::skin {

FilmstripKnob::FilmstripKnob(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& displayName,
                             Kind kind, juce::Colour arcColour)
    : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), apvts_(&apvts), paramId_(paramId), kind_(kind), arc_(arcColour) {
  setMouseDragSensitivity(kPixelsForFullRange);
  setScrollWheelEnabled(true);
  setTitle(displayName);
  setTooltip(displayName + " (drag to turn, shift = fine, double-click = reset)");
  attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(*apvts_, paramId_, *this);
  if (auto* p = apvts_->getParameter(paramId_)) setDoubleClickReturnValue(true, p->convertFrom0to1(p->getDefaultValue()));
}

FilmstripKnob::FilmstripKnob(const juce::String& displayName, Kind kind, juce::Colour arcColour, const Range& range)
    : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), range_(range), kind_(kind), arc_(arcColour) {
  setMouseDragSensitivity(kPixelsForFullRange);
  setScrollWheelEnabled(true);
  setRange(range.lo, range.hi, 0.0);
  if (range.skewMidpoint > range.lo && range.skewMidpoint < range.hi) setSkewFactorFromMidPoint(range.skewMidpoint);
  setValue(range.def, juce::dontSendNotification);
  setDoubleClickReturnValue(true, range.def);
  setNumDecimalPlacesToDisplay(range.decimals);
  setTitle(displayName);
  setTooltip(displayName + " (drag to turn, shift = fine, double-click = reset)");
}

FilmstripKnob::~FilmstripKnob() = default;

int FilmstripKnob::frameForProportion(double v, int frames) {
  if (frames <= 1) return 0;
  v = std::clamp(v, 0.0, 1.0);
  return static_cast<int>(std::lround(v * (frames - 1)));
}

const Filmstrip& FilmstripKnob::strip() const {
  auto& a = SkinAssets::get();
  return kind_ == Kind::Amp ? a.knobAmp() : a.knobPedal();
}

int FilmstripKnob::currentFrame() { return frameForProportion(proportion(), strip().frames); }

juce::String FilmstripKnob::getTextFromValue(double v) {
  if (apvts_ == nullptr) {
    juce::String t(v, range_.decimals);
    if (range_.unit.isNotEmpty()) t << " " << range_.unit;
    return t;
  }
  if (auto* p = apvts_->getParameter(paramId_)) {
    juce::String t = p->getText(p->convertTo0to1(static_cast<float>(v)), 32);
    if (p->getLabel().isNotEmpty()) t << " " << p->getLabel();
    return t;
  }
  return juce::Slider::getTextFromValue(v);
}

void FilmstripKnob::mouseDown(const juce::MouseEvent& e) {
  lastY_ = e.position.y;
  mouseHeld_ = true;
  juce::Slider::mouseDown(e);  // starts the host gesture and handles the popup menu / modifiers
}

void FilmstripKnob::mouseUp(const juce::MouseEvent& e) {
  juce::Slider::mouseUp(e);  // ends the gesture (onDragEnd runs while mouseHeld() is still true)
  mouseHeld_ = false;
}

void FilmstripKnob::mouseDrag(const juce::MouseEvent& e) {
  if (!isEnabled()) return;
  // Incremental, so releasing or pressing shift mid-drag never makes the knob jump.
  const double step = static_cast<double>(lastY_ - e.position.y) / kPixelsForFullRange * (e.mods.isShiftDown() ? kFineFactor : 1.0);
  lastY_ = e.position.y;
  const double p = std::clamp(proportion() + step, 0.0, 1.0);
  setValue(proportionOfLengthToValue(p), juce::sendNotificationSync);
}

void FilmstripKnob::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat();
  const float d = std::min(b.getWidth(), b.getHeight());
  const auto c = b.getCentre();
  const double v = proportion();
  const Filmstrip& fs = strip();

  // The sprite frame has a transparent margin (the body is ~63% of the frame for the pedal knob,
  // ~64% of the base for the amp one), so the frame is drawn larger than the component: the body
  // then fills it and the value arc runs just outside the body.
  const float frameScale = kind_ == Kind::Pedal ? 1.38f : 1.12f;
  const float half = d * 0.5f * frameScale;
  const float bodyR = d * (kind_ == Kind::Pedal ? 0.435f : 0.36f);

  // Code-drawn drop shadow: a soft dark disc offset downwards.
  {
    const float sr = bodyR * 1.35f;
    juce::ColourGradient grad(juce::Colours::black.withAlpha(0.65f), c.x, c.y + bodyR * 0.22f, juce::Colours::transparentBlack,
                              c.x, c.y + bodyR * 0.22f + sr, true);
    grad.addColour(0.72, juce::Colours::black.withAlpha(0.35f));
    g.setGradientFill(grad);
    g.fillEllipse(c.x - sr, c.y + bodyR * 0.22f - sr, sr * 2, sr * 2);
  }

  if (fs.valid()) {
    fs.drawFrame(g, frameForProportion(v, fs.frames), juce::Rectangle<float>(0, 0, half * 2, half * 2).withCentre(c));
  } else {  // code-drawn fallback
    g.setColour(juce::Colour(0xff3a3a3a));
    g.fillEllipse(c.x - bodyR, c.y - bodyR, bodyR * 2, bodyR * 2);
    const float a = juce::degreesToRadians(static_cast<float>(-135.0 + 270.0 * v));
    g.setColour(juce::Colours::white);
    g.drawLine(c.x, c.y, c.x + std::sin(a) * bodyR * 0.9f, c.y - std::cos(a) * bodyR * 0.9f, 2.0f);
  }

  // Value arc, -135..+135 degrees, just outside the body.
  const float arcR = d * 0.47f;
  const float thick = std::max(1.5f, d * 0.04f);
  const float a0 = juce::degreesToRadians(-135.0f);
  const float a1 = juce::degreesToRadians(135.0f);
  juce::Path track;
  track.addCentredArc(c.x, c.y, arcR, arcR, 0.0f, a0, a1, true);
  g.setColour(juce::Colour(0xff2a2622));
  g.strokePath(track, juce::PathStrokeType(thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  if (v > 0.0) {
    juce::Path val;
    val.addCentredArc(c.x, c.y, arcR, arcR, 0.0f, a0, a0 + static_cast<float>(v) * (a1 - a0), true);
    g.setColour(isEnabled() ? arc_ : arc_.withAlpha(0.35f));
    g.strokePath(val, juce::PathStrokeType(thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }
}

}  // namespace sawblade::plugin::skin
