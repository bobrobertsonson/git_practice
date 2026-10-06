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

juce::String AmpHead::bodyDownloadingText(const juce::String& name) {
  if (name.isEmpty()) return juce::String::fromUTF8("CHOOSING A BODY AMP\xE2\x80\xA6");
  return juce::String::fromUTF8("BODY AMP DOWNLOADING\xE2\x80\xA6") + " (" + name + ")";
}

juce::String AmpHead::bodyMissingText() { return juce::String::fromUTF8("BODY AMP MISSING \xE2\x80\x94 touch BLEND"); }

juce::String AmpHead::bodyFailedText(FillReason r) {
  const juce::String dash = juce::String::fromUTF8(" \xE2\x80\x94 ");
  switch (r) {
    case FillReason::NotLoggedIn: return "NOT LOGGED IN" + dash + "log in via Settings, then touch BLEND";
    case FillReason::Network: return "NO NETWORK" + dash + "check the connection, then touch BLEND";
    case FillReason::NoCapture: return "NO BODY AMP FOUND" + dash + "pick one with BROWSE CAPTURES";
    case FillReason::NoTool: return "TONE3000 TOOL NOT FOUND" + dash + "set its path in Settings";
    case FillReason::License: return "AMP NOT ALLOWED" + dash + "pick another with BROWSE CAPTURES";
    case FillReason::NetworkOff: return "NETWORK TOOLS DISABLED" + dash + "restart the host without SAWBLADE_NO_NETWORK";
    default: return "BODY AMP FAILED" + dash + "pick one with BROWSE CAPTURES";
  }
}

juce::String AmpHead::bodyOffWithBlocksText() { return juce::String::fromUTF8("BODY PATH OFF \xE2\x80\x94 turn up BLEND"); }

juce::String AmpHead::gainReadout(double gain, const SawbladeProcessor::LadderInfo& l) {
  const juce::String g = "GAIN " + juce::String(gain, 1);
  if (!l.has) return g;
  if (l.pending) return g + dot() + "drive only (fetching " + juce::String(l.targetName.empty() ? l.targetModelId : l.targetName) + ")";
  return g + dot() + "capture: " + juce::String(l.activeName.empty() ? l.activeModelId : l.activeName);
}

juce::String AmpHead::stepsText(int steps) {
  if (steps >= 2) return "STEPS " + juce::String(steps);
  if (steps == 0) return juce::String::fromUTF8("STEPS \xE2\x80\x94");
  return {};
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

void AmpHead::refresh(const Preset& preset, const SawbladeProcessor::LadderInfo& ladder, const FillStatus& fill) {
  const PathPreset& pp = path_ == 0 ? preset.a : preset.b;
  juce::String text, tag;
  bool on = true, reason = false;
  if (path_ == 1 && !pp.enabled) {
    text = pp.blocks.empty() ? bodyOffText() : bodyOffWithBlocksText();
    on = false;
    reason = true;
  } else if (ampIndex(pp) < 0 && path_ == 1 && fill.kind == FillStatus::Kind::Downloading) {
    text = bodyDownloadingText(juce::String::fromUTF8(fill.name.c_str()));
    on = false;  // reason_ stays false: not a fault, the normal text style
  } else if (ampIndex(pp) < 0 && path_ == 1 && fill.kind == FillStatus::Kind::Failed) {
    text = bodyFailedText(fill.reason);
    on = false;
    reason = true;
  } else if (ampIndex(pp) < 0 && path_ == 1) {
    text = bodyMissingText();
    on = false;
    reason = true;
  } else if (ampIndex(pp) < 0) {
    text = noAmpText();
    on = false;
    reason = true;
  } else {
    text = gainReadout(knob(kAmpGain).getValue(), ladder);
    // The steps tag: from the preset as shown (so it follows an undo / redo and a ladder that just arrived), and what this session has checked.
    if (const auto* nam = dynamic_cast<const NamBlockParams*>(pp.blocks[static_cast<std::size_t>(ampIndex(pp))].params.get())) {
      if (nam->model.ladder.size() >= 2) tag = stepsText(static_cast<int>(nam->model.ladder.size()));
      else if (nam->model.source && nam->model.source->provider == "tone3000" && proc_.ladderCheckedNone(nam->model.source->id)) tag = stepsText(0);
    }
  }
  if (on != enabled_) {
    enabled_ = on;
    for (auto& k : knobs_) k->setEnabled(on);
  }
  if (text != readout_ || reason != reason_ || tag != stepsTag_) {
    readout_ = text;
    stepsTag_ = tag;
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
  auto textArea = pill.toNearestInt().reduced(6, 0);
  if (stepsTag_.isNotEmpty()) {  // the tag takes the right end of the pill; the read-out keeps the rest
    const auto tagArea = textArea.removeFromRight(52);
    g.setColour((stepsTag_.endsWithChar('4') || stepsTag_.getLastCharacter() != 0 ? L::dimText() : L::dimText()).withAlpha(1.0f));
    g.setFont(L::monoFont(9.0f));
    g.drawText(stepsTag_, tagArea, juce::Justification::centredRight);
    g.setColour((reason_ ? L::warning() : L::text()).withAlpha(0.95f));
    g.setFont(L::monoFont(11.0f));
  }
  g.drawFittedText(readout_, textArea, juce::Justification::centred, 1, 0.75f);
}

}  // namespace sawblade::plugin::rig
