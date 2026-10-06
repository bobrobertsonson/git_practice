#include "rig/EqGraph.h"

#include <algorithm>
#include <cmath>

#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin::rig {
namespace {
using L = SawbladeLookAndFeel;

constexpr double kGainSpan = 36.0;  // +18 .. -18 dB

juce::Colour typeColour(EqType t) {
  switch (t) {
    case EqType::Peak: return L::saw();
    case EqType::LowShelf: return L::live();
    case EqType::HighShelf: return L::studio();
    case EqType::HighPass: return L::body();
    case EqType::LowPass: return L::bodyText();
  }
  return L::saw();
}

juce::String freqText(double f) {
  if (f >= 1000.0) return juce::String(f / 1000.0, 2) + " kHz";
  return juce::String(juce::roundToInt(f)) + " Hz";
}

}  // namespace

double EqGraph::xToFreq(double x, double w) noexcept { return 20.0 * std::pow(1000.0, x / w); }
double EqGraph::freqToX(double f, double w) noexcept { return w * std::log(f / 20.0) / std::log(1000.0); }
double EqGraph::yToGain(double y, double h) noexcept { return 18.0 - kGainSpan * y / h; }
double EqGraph::gainToY(double g, double h) noexcept { return h * (18.0 - g) / kGainSpan; }
double EqGraph::yToQ(double y, double h) noexcept { return 0.1 * std::pow(200.0, (h - y) / h); }
double EqGraph::qToY(double q, double h) noexcept { return h - h * std::log(q / 0.1) / std::log(200.0); }

const char* EqGraph::typeName(EqType t) noexcept {
  switch (t) {
    case EqType::Peak: return "PEAK";
    case EqType::LowShelf: return "LOW SHELF";
    case EqType::HighShelf: return "HIGH SHELF";
    case EqType::HighPass: return "HIGH-PASS";
    case EqType::LowPass: return "LOW-PASS";
  }
  return "";
}

juce::String EqGraph::bandReadout(const EqBand& b) {
  const juce::String dot = juce::String::fromUTF8(" \xC2\xB7 ");
  juce::String s = juce::String(typeName(b.type)) + dot + freqText(b.freq);
  if (hasGain(b.type)) s << dot << (b.gainDb >= 0 ? "+" : "") << juce::String(b.gainDb, 1) << " dB";
  s << dot << "Q " << juce::String(b.q, 2);
  if (!b.enabled) s << dot << "OFF";
  return s;
}

EqGraph::EqGraph(RigController& c) : controller_(c) {
  setTitle("EQ graph");
  setTooltip("Drag a node: frequency and gain (Q for high/low-pass). Shift-drag or wheel: Q. Double-click a node: on/off. "
             "Double-click empty space: add a band. Right-click a node: type / remove.");
  setWantsKeyboardFocus(false);
  recomputeCurve();
}

void EqGraph::setTarget(EqTarget t) {
  if (t == target_) return;
  endGesture();
  target_ = t;
  bands_.clear();
  selected_ = -1;
  recomputeCurve();
  repaint();
  changed();
}

void EqGraph::refresh(const Preset& p, double sampleRate) {
  if (dragBand_ >= 0) return;
  const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
  const auto& src = eqBands(p, target_);
  if (src == bands_ && sr == sampleRate_) return;
  const bool selChanged = selected_ >= static_cast<int>(src.size());
  bands_ = src;
  sampleRate_ = sr;
  if (selChanged || (selected_ < 0 && !bands_.empty())) selected_ = bands_.empty() ? -1 : std::clamp(selected_, 0, static_cast<int>(bands_.size()) - 1);
  recomputeCurve();
  repaint();
  changed();
}

void EqGraph::select(int i) {
  i = bands_.empty() ? -1 : std::clamp(i, 0, static_cast<int>(bands_.size()) - 1);
  if (i == selected_) return;
  selected_ = i;
  repaint();
  changed();
}

juce::String EqGraph::readout() const {
  if (selected_ < 0 || selected_ >= numBands()) return bands_.empty() ? "NO BANDS: double-click the graph or use + BAND" : "NO BAND SELECTED";
  return bandReadout(bands_[static_cast<std::size_t>(selected_)]);
}

void EqGraph::changed() {
  if (onChanged) onChanged();
}

juce::Point<float> EqGraph::nodePosition(int i) const {
  const auto& b = bands_[static_cast<std::size_t>(i)];
  const double w = getWidth(), h = getHeight();
  const double x = freqToX(std::clamp(b.freq, 20.0, 20000.0), w);
  const double y = hasGain(b.type) ? gainToY(std::clamp(b.gainDb, -18.0, 18.0), h) : qToY(std::clamp(b.q, 0.1, 20.0), h);
  return {static_cast<float>(x), static_cast<float>(y)};
}

int EqGraph::hit(juce::Point<float> p) const {
  int best = -1;
  float bestD = kHitRadius;
  for (int i = 0; i < numBands(); ++i) {
    const float d = nodePosition(i).getDistanceFrom(p);
    if (d <= bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

juce::Colour EqGraph::accent() const {
  switch (target_) {
    case EqTarget::PreA:
    case EqTarget::EqA: return L::saw();
    case EqTarget::PreB:
    case EqTarget::EqB: return L::body();
    case EqTarget::Post: break;
  }
  return L::text();
}

void EqGraph::recomputeCurve() {
  const int w = std::max(1, getWidth() > 0 ? getWidth() : kWidth);
  curveDb_.assign(static_cast<std::size_t>(w), 0.0f);
  std::vector<BiquadCoeffs> cs;
  for (const EqBand& b : bands_) {
    if (!b.enabled) continue;
    try {
      cs.push_back(designBiquad(b, sampleRate_));
    } catch (const std::exception&) {  // not valid at this rate: not drawn
    }
  }
  for (int x = 0; x < w; ++x) {
    const double f = xToFreq(x + 0.5, w);
    double db = 0.0;
    for (const auto& c : cs) db += biquadMagnitudeDb(c, f, sampleRate_);
    curveDb_[static_cast<std::size_t>(x)] = static_cast<float>(db);
  }
}

void EqGraph::paint(juce::Graphics& g) {
  const auto r = getLocalBounds().toFloat();
  const float w = r.getWidth(), h = r.getHeight();
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(r, 6.0f);

  // grid: decades / octave-ish marks and 6 dB steps
  g.setFont(L::monoFont(10.0f));
  for (int db = -18; db <= 18; db += 6) {
    const float y = static_cast<float>(gainToY(db, h));
    g.setColour(db == 0 ? L::chipBorder() : L::rule());
    g.drawHorizontalLine(juce::roundToInt(y), 0.0f, w);
    g.setColour(L::placeholderText());
    g.drawText((db > 0 ? "+" : "") + juce::String(db), juce::roundToInt(w) - 44, juce::roundToInt(y) - 12, 40, 12, juce::Justification::centredRight);
  }
  const double marks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  for (const double f : marks) {
    const float x = static_cast<float>(freqToX(f, w));
    g.setColour(f == 100 || f == 1000 || f == 10000 ? L::chipBorder() : L::rule());
    g.drawVerticalLine(juce::roundToInt(x), 0.0f, h);
    g.setColour(L::placeholderText());
    const juce::String t = f >= 1000 ? juce::String(f / 1000.0, 0) + "k" : juce::String(static_cast<int>(f));
    g.drawText(t, juce::roundToInt(x) + 3, juce::roundToInt(h) - 14, 36, 12, juce::Justification::centredLeft);
  }

  // combined response
  const juce::Colour col = accent();
  juce::Path line, fill;
  const float y0 = static_cast<float>(gainToY(0.0, h));
  fill.startNewSubPath(0.0f, y0);
  for (int x = 0; x < static_cast<int>(curveDb_.size()); ++x) {
    const float y = static_cast<float>(std::clamp(gainToY(curveDb_[static_cast<std::size_t>(x)], h), -4.0, static_cast<double>(h) + 4.0));
    if (x == 0) line.startNewSubPath(0.0f, y);
    else line.lineTo(static_cast<float>(x), y);
    fill.lineTo(static_cast<float>(x), y);
  }
  fill.lineTo(static_cast<float>(curveDb_.size()), y0);
  fill.closeSubPath();
  g.setColour(col.withAlpha(0.14f));
  g.fillPath(fill);
  g.setColour(col);
  g.strokePath(line, juce::PathStrokeType(2.0f));

  // nodes
  for (int i = 0; i < numBands(); ++i) {
    const EqBand& b = bands_[static_cast<std::size_t>(i)];
    const auto c = nodePosition(i);
    const juce::Colour tc = typeColour(b.type);
    const auto rc = juce::Rectangle<float>(kNodeRadius * 2, kNodeRadius * 2).withCentre(c);
    if (b.enabled) {
      g.setColour(tc);
      g.fillEllipse(rc);
      g.setColour(L::background());
    } else {
      g.setColour(L::panelDeep());
      g.fillEllipse(rc);
      g.setColour(tc);
      g.drawEllipse(rc, 1.5f);
    }
    g.setFont(L::labelFont(10.0f));
    g.drawText(juce::String(i + 1), rc, juce::Justification::centred);
    if (i == selected_) {
      g.setColour(juce::Colours::white);
      g.drawEllipse(rc.expanded(4.0f), 1.5f);
    }
  }
}

// --- interaction -----------------------------------------------------------------------------------
void EqGraph::endGesture() {
  if (gestureParam_ >= 0) controller_.endParam(gestureParam_);
  gestureParam_ = -1;
  if (undoGesture_) {  // one undo step per drag
    undoGesture_ = false;
    controller_.endGesture();
  }
}

void EqGraph::liveEdit(int band, double freq, double gain, double q) {
  controller_.eqLive(target_, band, freq, gain, q);
}

void EqGraph::mouseDown(const juce::MouseEvent& e) {
  const int i = hit(e.position);
  if (e.mods.isPopupMenu()) {
    if (i >= 0) {
      select(i);
      showBandMenu(i);
    }
    return;
  }
  dragBand_ = -1;
  if (i < 0) return;
  select(i);
  dragBand_ = i;
  if (!undoGesture_) {
    undoGesture_ = true;
    controller_.beginGesture();
  }
  dragStart_ = bands_[static_cast<std::size_t>(i)];
  dragNode_ = nodePosition(i);
  if (target_ == EqTarget::Post) {
    const int slot = controller_.postSlotOfBand(i);
    if (slot >= 0) {
      gestureParam_ = kPostEqFirst + slot;
      controller_.beginParam(gestureParam_);
    }
  }
}

void EqGraph::mouseDrag(const juce::MouseEvent& e) {
  if (dragBand_ < 0 || dragBand_ >= numBands()) return;
  applyDrag(e);
}

void EqGraph::applyDrag(const juce::MouseEvent& e) {
  const double w = getWidth(), h = getHeight();
  const auto delta = e.position - e.mouseDownPosition;
  EqBand b = dragStart_;
  if (e.mods.isShiftDown()) {
    // Q only: 60 px of vertical travel is one e-fold
    b.q = std::clamp(dragStart_.q * std::exp(-static_cast<double>(delta.y) / 60.0), kEqQMin, kEqQMax);
  } else {
    if (delta.x != 0.0f) b.freq = std::clamp(xToFreq(std::clamp(static_cast<double>(dragNode_.x + delta.x), 0.0, w), w), kEqFreqMin, kEqFreqMax);
    if (delta.y != 0.0f) {
      const double y = std::clamp(static_cast<double>(dragNode_.y + delta.y), 0.0, h);
      if (hasGain(b.type)) b.gainDb = std::clamp(yToGain(y, h), -kEqGainMax, kEqGainMax);
      else b.q = std::clamp(yToQ(y, h), kEqQMin, kEqQMax);
    }
  }
  bands_[static_cast<std::size_t>(dragBand_)] = b;
  liveEdit(dragBand_, b.freq, hasGain(b.type) ? b.gainDb : 0.0, b.q);
  recomputeCurve();
  repaint();
  changed();
}

void EqGraph::mouseUp(const juce::MouseEvent&) {
  endGesture();
  dragBand_ = -1;
}

void EqGraph::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
  int i = hit(e.position);
  if (i < 0) i = selected_;
  if (i < 0 || i >= numBands() || wheel.deltaY == 0.0f) return;
  select(i);
  EqBand b = bands_[static_cast<std::size_t>(i)];
  b.q = std::clamp(b.q * std::pow(1.12, wheel.deltaY > 0 ? 1.0 : -1.0), kEqQMin, kEqQMax);
  bands_[static_cast<std::size_t>(i)] = b;
  liveEdit(i, b.freq, hasGain(b.type) ? b.gainDb : 0.0, b.q);
  recomputeCurve();
  repaint();
  changed();
}

void EqGraph::mouseDoubleClick(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  const int i = hit(e.position);
  const EqTarget t = target_;
  if (i >= 0) {
    const bool on = !bands_[static_cast<std::size_t>(i)].enabled;
    controller_.edit([t, i, on](Preset& p) { setBandEnabled(p, t, i, on); });
    return;
  }
  const double w = getWidth(), h = getHeight();
  EqBand nb;
  nb.type = EqType::Peak;
  nb.freq = std::clamp(xToFreq(e.position.x, w), kEqFreqMin, kEqFreqMax);
  nb.gainDb = std::clamp(yToGain(e.position.y, h), -kEqGainMax, kEqGainMax);
  nb.q = 1.0;
  if (numBands() >= ParametricEq::kMaxBands) return;
  selected_ = numBands();  // the new band is appended: select it
  controller_.edit([t, nb](Preset& p) { addBand(p, t, nb); });
}

void EqGraph::showBandMenu(int band) {
  const EqBand b = bands_[static_cast<std::size_t>(band)];
  juce::PopupMenu m;
  const EqType types[] = {EqType::Peak, EqType::LowShelf, EqType::HighShelf, EqType::HighPass, EqType::LowPass};
  for (int k = 0; k < 5; ++k) m.addItem(1 + k, typeName(types[k]), true, b.type == types[k]);
  m.addSeparator();
  m.addItem(10, b.enabled ? "Disable band" : "Enable band");
  m.addItem(11, "Remove band");
  const EqTarget t = target_;
  m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(juce::Rectangle<int>(1, 1).withCentre(nodePosition(band).toInt()))),
                  [this, t, band, types](int r) {
                    if (r >= 1 && r <= 5) controller_.edit([=](Preset& p) { setBandType(p, t, band, types[r - 1]); });
                    else if (r == 10) controller_.edit([=](Preset& p) {
                      if (band >= 0 && band < static_cast<int>(eqBands(p, t).size()))
                        setBandEnabled(p, t, band, !eqBands(p, t)[static_cast<std::size_t>(band)].enabled);
                    });
                    else if (r == 11) controller_.edit([=](Preset& p) { removeBand(p, t, band); });
                  });
}

}  // namespace sawblade::plugin::rig
