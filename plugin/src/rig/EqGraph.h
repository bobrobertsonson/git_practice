#pragma once

// The interactive graphical EQ (spec phase 10, section 6.2). 700 x 380 design px.
//
// Mapping (x, y in the graph's own pixels, w = width, h = height):
//     freq   = 20 * 1000^(x / w)                       x = w * log(freq / 20) / log(1000)   20 Hz .. 20 kHz
//     gain   = 18 - 36 * y / h        (peak / shelf)    y = h * (18 - gain) / 36              +18 .. -18 dB
//     q      = 0.1 * 200^((h - y) / h) (high / low pass: the node sits at the band's Q)       0.1 .. 20
// Interaction: drag = freq + gain (Q for pass filters); shift-drag or mouse wheel = Q; double-click a node =
// enable / disable (structural); right-click a node = type menu / remove (structural); double-click empty space
// = add a peak band there (structural). freq / gain / Q drags are live (no rebuild); a post EQ band that has a
// parameter slot takes its gain from the host-visible parameter (docs/PLUGIN.md "Rig editor").

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "rig/RigController.h"

namespace sawblade::plugin::rig {

class EqGraph : public juce::Component, public juce::SettableTooltipClient {
 public:
  static constexpr int kWidth = 700, kHeight = 380;
  static constexpr float kNodeRadius = 8.0f, kHitRadius = 14.0f;

  explicit EqGraph(RigController& c);

  // --- the documented mapping (static, tested) ---
  static double xToFreq(double x, double w) noexcept;
  static double freqToX(double freq, double w) noexcept;
  static double yToGain(double y, double h) noexcept;
  static double gainToY(double gainDb, double h) noexcept;
  static double yToQ(double y, double h) noexcept;
  static double qToY(double q, double h) noexcept;
  static juce::String bandReadout(const EqBand& b);  // "PEAK · 1.20 kHz · +3.0 dB · Q 1.00"
  static const char* typeName(EqType t) noexcept;

  void setTarget(EqTarget t);
  EqTarget target() const noexcept { return target_; }
  // Takes the bands of the current target from the preset (a no-op while a drag is in progress).
  void refresh(const Preset& p, double sampleRate);

  int numBands() const noexcept { return static_cast<int>(bands_.size()); }
  const EqBand& band(int i) const { return bands_[static_cast<std::size_t>(i)]; }
  int selected() const noexcept { return selected_; }
  void select(int i);
  juce::String readout() const;
  // Centre of a band's node in this component's coordinates.
  juce::Point<float> nodePosition(int band) const;
  std::function<void()> onChanged;  // selection or band values changed (the page updates its readout)

  void paint(juce::Graphics&) override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent&) override;
  void mouseUp(const juce::MouseEvent&) override;
  void mouseDoubleClick(const juce::MouseEvent&) override;
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

 private:
  int hit(juce::Point<float> p) const;
  juce::Colour accent() const;
  void recomputeCurve();
  void applyDrag(const juce::MouseEvent& e);
  void showBandMenu(int band);
  void changed();
  void liveEdit(int band, double freq, double gain, double q);
  void endGesture();

  RigController& controller_;
  EqTarget target_ = EqTarget::Post;
  std::vector<EqBand> bands_;
  double sampleRate_ = 48000.0;
  int selected_ = -1;
  std::vector<float> curveDb_;  // one value per x pixel

  // drag state
  int dragBand_ = -1;
  EqBand dragStart_;
  juce::Point<float> dragNode_;
  int gestureParam_ = -1;
  bool undoGesture_ = false;  // a controller history gesture is open (mouse down .. up on a band)

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqGraph)
};

}  // namespace sawblade::plugin::rig
