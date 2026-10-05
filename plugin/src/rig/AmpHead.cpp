#include "rig/AmpHead.h"

#include "SawbladeLookAndFeel.h"
#include "rig/RigModel.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

// The art's knob order (left to right) and centres (design px inside the 330-wide head; the art is 659 px wide, x0.5).
constexpr int kOrder[kAmpKnobCount] = {kAmpGain, kAmpBass, kAmpMid, kAmpTreble, kAmpLevel, kAmpPresence};
constexpr float kCx[kAmpKnobCount] = {65.5f, 106.5f, 140.5f, 181.5f, 219.0f, 258.0f};
constexpr float kCy = 26.5f, kCaptionY = 41.0f;
constexpr float kPillY = 108.0f, kPillH = 22.0f;

juce::String dot() { return juce::String::fromUTF8(" \xC2\xB7 "); }
}  // namespace

const char* AmpHead::caption(int k) {
  static const char* const c[kAmpKnobCount] = {"GAIN", "BASS", "MID", "TREBLE", "PRESENCE", "LEVEL"};
  return c[k];
}

juce::String AmpHead::noAmpText() { return "NO AMP IN THIS PATH"; }
juce::String AmpHead::bodyOffText() { return juce::String::fromUTF8("BODY PATH OFF \xE2\x80\x94 turn up BLEND to add one"); }

juce::String AmpHead::gainReadout(double gain, const SawbladeProcessor::LadderInfo& l) {
  const juce::String g = "GAIN " + juce::String(gain, 1);
  if (!l.has) return g;
  if (l.pending) return g + dot() + "drive only (fetching " + juce::String(l.targetName.empty() ? l.targetModelId : l.targetName) + ")";
  return g + dot() + "capture: " + juce::String(l.activeName.empty() ? l.activeModelId : l.activeName);
}

AmpHead::AmpHead(SawbladeProcessor& p, int path) : proc_(p), path_(path) {
  setTitle(path == 0 ? "SAW amp controls" : "BODY amp controls");
  setInterceptsMouseClicks(false, true);  // only the knobs take the mouse: a click elsewhere on the head still selects it
  const juce::Colour arc = path == 0 ? L::saw() : L::body();
  for (int k = 0; k < kAmpKnobCount; ++k) {
    const ParamSpec& s = paramSpec(ampParam(path, k));
    auto kn = std::make_unique<skin::FilmstripKnob>(p.parameters(), s.id, s.name, skin::FilmstripKnob::Kind::Amp, arc);
    kn->setTooltip(juce::String(path == 0 ? "SAW" : "BODY") + " amp " + caption(k) + " (0-10, 5 = neutral; host automatable)");
    addAndMakeVisible(*kn);
    knobs_[static_cast<std::size_t>(k)] = std::move(kn);
  }
  readout_ = gainReadout(5.0, {});
}

AmpHead::~AmpHead() = default;

void AmpHead::resized() {
  for (int i = 0; i < kAmpKnobCount; ++i)
    knobs_[static_cast<std::size_t>(kOrder[i])]->setBounds(juce::Rectangle<float>(kKnob, kKnob).withCentre({kCx[i], kCy}).toNearestInt());
}

void AmpHead::refresh() {
  const Preset p = proc_.editBasePreset();
  refresh(p, proc_.ladderInfo(path_));
}

void AmpHead::refresh(const Preset& preset, const SawbladeProcessor::LadderInfo& ladder) {
  const PathPreset& pp = path_ == 0 ? preset.a : preset.b;
  juce::String text;
  bool on = true, reason = false;
  if (path_ == 1 && !pp.enabled) {
    text = bodyOffText();
    on = false;
    reason = true;
  } else if (ampIndex(pp) < 0) {
    text = noAmpText();
    on = false;
    reason = true;
  } else {
    text = gainReadout(knob(kAmpGain).getValue(), ladder);
  }
  if (on != enabled_) {
    enabled_ = on;
    for (auto& k : knobs_) k->setEnabled(on);
  }
  if (text != readout_ || reason != reason_) {
    readout_ = text;
    reason_ = reason;
    repaint();
  }
}

void AmpHead::paint(juce::Graphics& g) {
  // Captions over the baked ones (LOW / HIGH ...): the art's names differ from the parameters'.
  g.setFont(L::labelFont(7.0f));
  for (int i = 0; i < kAmpKnobCount; ++i) {
    const juce::Rectangle<float> r(kCx[i] - 20.0f, kCaptionY, 40.0f, 11.0f);
    g.setColour(juce::Colour(0xff0d0c0b));
    g.fillRect(r);
    g.setColour(enabled_ ? juce::Colour(0xffd9d2c3) : juce::Colour(0xff6a655c));
    g.drawText(caption(kOrder[i]), r.toNearestInt(), juce::Justification::centred);
  }
  // The read-out pill.
  const auto pill = juce::Rectangle<float>(12.0f, kPillY, static_cast<float>(getWidth()) - 24.0f, kPillH);
  g.setColour(juce::Colour(0xe00d0c0b));
  g.fillRoundedRectangle(pill, 4.0f);
  g.setColour((reason_ ? L::warning() : L::text()).withAlpha(0.95f));
  g.setFont(L::monoFont(11.0f));
  g.drawFittedText(readout_, pill.toNearestInt().reduced(6, 0), juce::Justification::centred, 1, 0.75f);
}

}  // namespace sawblade::plugin::rig
