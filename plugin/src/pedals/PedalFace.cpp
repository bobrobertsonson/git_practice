#include "PedalFace.h"

#include <algorithm>
#include <cmath>

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

constexpr float kOledCentreY = 56.0f, kOledW = 68.0f, kOledH = 25.0f;
constexpr float kKnobLabelDy = -18.0f, kSwitchLabelY = -58.5f;

juce::String upper(const juce::String& s) { return s.toUpperCase(); }
}  // namespace

juce::Point<float> PedalFace::knobMm(int i) {
  static constexpr float xs[3] = {-36.0f, 0.0f, 36.0f};
  return {xs[i % 3], i < 3 ? 26.0f : -15.0f};
}
juce::Point<float> PedalFace::switchMm(int i) {
  static constexpr float xs[3] = {-36.0f, 0.0f, 36.0f};
  return {xs[i % 3], -47.5f};
}

PedalFace::PedalFace(SawbladeProcessor& p) : proc_(p) {
  setInterceptsMouseClicks(false, true);
  setTitle("Pedal face");
  auto& apvts = proc_.parameters();

  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    for (int k = 0; k < 6; ++k) {
      const FaceKnob& fk = f.knobs[static_cast<size_t>(k)];
      if (fk.param < 0) continue;
      const ParamSpec& s = paramSpec(fk.param);
      auto knob = std::make_unique<skin::FilmstripKnob>(apvts, s.id, s.name, skin::FilmstripKnob::Kind::Pedal, L::saw());
      addChildComponent(*knob);
      knobs_[static_cast<size_t>(c)][static_cast<size_t>(k)] = std::move(knob);
    }
    if (f.clipParam >= 0) {
      std::vector<juce::String> clipTexts;
      for (int i = 0; i < kNumClipTypes; ++i) clipTexts.push_back(clipShortName(i));
      clip_[static_cast<size_t>(c)] = std::make_unique<PedalSwitch>(apvts, paramSpec(f.clipParam).id, std::string(f.oledName) + " clip", clipTexts);
    }
    focus_[static_cast<size_t>(c)] = std::make_unique<PedalSwitch>(apvts, paramSpec(f.focus.param).id, std::string(f.oledName) + " " + f.focus.label, f.focus);
    for (PedalSwitch* sw : {clip_[static_cast<size_t>(c)].get(), focus_[static_cast<size_t>(c)].get()}) {
      if (sw == nullptr) continue;
      sw->setValueText(PedalSwitch::TextSide::Above, 0, 6.5f);
      addChildComponent(*sw);
    }
  }
  circuit_ = std::make_unique<PedalSwitch>(apvts, paramSpec(kSawCircuit).id, "Circuit");
  circuit_->setValueText(PedalSwitch::TextSide::Above, 0, 6.5f);
  addChildComponent(*circuit_);
  setVisible(false);
}

PedalFace::~PedalFace() = default;

float PedalFace::pxPerMm() const { return static_cast<float>(getHeight()) / kPedalMmHeight; }

juce::Point<float> PedalFace::toPx(juce::Point<float> mm) const {
  const float ppm = pxPerMm();
  const auto c = getLocalBounds().toFloat().getCentre();
  return {c.x + mm.x * ppm, c.y - mm.y * ppm};  // +y is up in pedal millimetres
}

void PedalFace::resized() {
  const float ppm = pxPerMm();
  const float kd = kKnobMm * ppm;
  for (int c = 0; c < kNumCircuits; ++c)
    for (int k = 0; k < 6; ++k)
      if (auto* knob = knobs_[static_cast<size_t>(c)][static_cast<size_t>(k)].get())
        knob->setBounds(juce::Rectangle<float>(kd, kd).withCentre(toPx(knobMm(k))).toNearestInt());
  const float lever = kSwitchMm * ppm;
  const int textH = 0;
  auto place = [&](PedalSwitch& sw, int position) {
    const auto centre = toPx(switchMm(position));
    const float w = 30.0f * ppm;
    sw.setBounds(juce::Rectangle<float>(centre.x - w / 2, centre.y - lever / 2 - static_cast<float>(textH), w, lever + static_cast<float>(textH)).toNearestInt());
  };
  place(*circuit_, 0);
  for (int c = 0; c < kNumCircuits; ++c) {
    if (clip_[static_cast<size_t>(c)]) place(*clip_[static_cast<size_t>(c)], 1);
    place(*focus_[static_cast<size_t>(c)], 2);
  }
}

void PedalFace::showActive() {
  for (int c = 0; c < kNumCircuits; ++c) {
    const bool on = active_ && static_cast<int>(*active_) == c;
    for (auto& k : knobs_[static_cast<size_t>(c)])
      if (k) k->setVisible(on);
    if (clip_[static_cast<size_t>(c)]) clip_[static_cast<size_t>(c)]->setVisible(on);
    focus_[static_cast<size_t>(c)]->setVisible(on);
  }
  circuit_->setVisible(active_.has_value());
}

void PedalFace::refresh() {
  const auto slot = proc_.circuitSlot();
  const std::optional<Circuit> next = slot ? std::optional<Circuit>(slot->circuit) : std::nullopt;
  const juce::String name = juce::String(proc_.status().presetName);
  const bool changed = next != active_ || name != presetName_ || isVisible() != next.has_value();
  active_ = next;
  presetName_ = name;
  if (changed) {
    showActive();
    setVisible(active_.has_value());
  }
  repaint();  // the OLED follows the parameters
}

juce::String PedalFace::oledLine1() const { return upper(presetName_); }

juce::String PedalFace::oledLine2() const {
  const Circuit c = active_.value_or(Circuit::Chainsaw);
  const CircuitFace& f = circuitFace(c);
  auto& apvts = proc_.parameters();
  const auto raw = [&](int param) { return static_cast<double>(apvts.getRawParameterValue(paramSpec(param).id)->load()); };
  const juce::String focus = f.focus.isNarrow(raw(f.focus.param)) ? f.focus.narrowText : f.focus.wideText;
  if (f.clipParam < 0) return juce::String(f.oledName) + kDot + focus;  // no CLIP switch: no clip field
  const int clip = static_cast<int>(std::lround(raw(f.clipParam)));
  return juce::String(f.oledName) + kDot + clipShortName(clip) + kDot + focus;
}

void PedalFace::paint(juce::Graphics& g) {
  if (!active_) return;
  const float ppm = pxPerMm();

  // Dark sockets behind the knobs: they hide the baked knobs and rings underneath.
  for (int k = 0; k < 6; ++k) {
    const auto c = toPx(knobMm(k));
    const float r = 17.5f * ppm;
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1c1a18), c.x, c.y, juce::Colour(0xff0a0908), c.x + r, c.y, true));
    g.fillEllipse(c.x - r, c.y - r, r * 2, r * 2);
    g.setColour(juce::Colour(0xff050505));
    g.drawEllipse(c.x - r, c.y - r, r * 2, r * 2, 1.0f);
  }
  // Dark plates behind the three switches.
  for (int k = 0; k < 3; ++k) {
    const auto c = toPx(switchMm(k));
    const float r = 7.5f * ppm;
    g.setColour(juce::Colour(0xff0e0d0c));
    g.fillEllipse(c.x - r, c.y - r, r * 2, r * 2);
  }

  // Label chips over the baked captions.
  const CircuitFace& f = circuitFace(*active_);
  const float chipH = kChipMmHeight * ppm;
  auto chip = [&](juce::Point<float> centre, const juce::String& text) {
    g.setFont(L::labelFont(chipH * 0.92f));
    const float w = std::max(24.0f * ppm * 0.75f + 4.0f, juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 8.0f);
    const auto r = juce::Rectangle<float>(w, chipH).withCentre(centre);
    g.setColour(juce::Colour(0xff121110));
    g.fillRoundedRectangle(r, 2.0f);
    g.setColour(L::chipBorder());
    g.drawRoundedRectangle(r, 2.0f, 0.75f);
    g.setColour(L::text());
    g.drawText(text, r, juce::Justification::centred);
  };
  for (int k = 0; k < 6; ++k) {
    const FaceKnob& fk = f.knobs[static_cast<size_t>(k)];
    auto m = knobMm(k);
    chip(toPx({m.x, m.y + kKnobLabelDy}), fk.param >= 0 ? fk.label : "");
  }
  const char* switchLabels[3] = {"CIRCUIT", f.clipParam >= 0 ? "CLIP" : "", f.focus.label};
  for (int k = 0; k < 3; ++k) chip(toPx({switchMm(k).x, kSwitchLabelY}), switchLabels[k]);

  // OLED overlay.
  const auto oled = juce::Rectangle<float>(kOledW * ppm, kOledH * ppm).withCentre(toPx({0.0f, kOledCentreY}));
  g.setColour(juce::Colour(0xff060606));
  g.fillRoundedRectangle(oled, 3.0f);
  g.setColour(juce::Colour(0xff2a2622));
  g.drawRoundedRectangle(oled, 3.0f, 1.0f);
  g.setColour(L::text());
  const float lineH = oled.getHeight() * 0.36f;
  g.setFont(L::monoFont(lineH * 0.95f).boldened());
  auto inner = oled.reduced(4.0f, 2.0f);
  g.drawFittedText(oledLine1(), inner.removeFromTop(inner.getHeight() * 0.5f).toNearestInt(), juce::Justification::centred, 1, 0.5f);
  g.setColour(L::sawText());
  g.drawFittedText(oledLine2(), inner.toNearestInt(), juce::Justification::centred, 1, 0.5f);
}

}  // namespace sawblade::plugin
