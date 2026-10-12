#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sawblade::plugin::skin {

// What a JSON sidecar says about a filmstrip (design/render/ui_sprites.py output).
struct SidecarInfo {
  int frames = 0, frameWidth = 0, frameHeight = 0;
  double ringRadiusPx = 0.0;  // optional "ring_radius_px" (rotary strips), in frame pixels; 0 = absent
};

// A vertical filmstrip: `frames` frames of frameWidth x frameHeight stacked top to bottom.
struct Filmstrip {
  juce::Image image;
  int frames = 0, frameWidth = 0, frameHeight = 0;
  double ringRadiusPx = 0.0;

  bool valid() const noexcept { return frames > 0 && image.isValid(); }
  // Frame `frame` (clamped) scaled into `dest` with high-quality resampling.
  void drawFrame(juce::Graphics& g, int frame, juce::Rectangle<float> dest) const;
  // A copy of one frame at native size.
  juce::Image frameImage(int frame) const;
};

enum class Panel { AmpSaw, AmpBody, Cab4x12, PedalSaw, PedalBody };

// Every UI render the editor uses, decoded once from the embedded BinaryData (message thread).
// A missing or garbled image or sidecar never crashes: the entry stays invalid (jassert in debug)
// and the controls fall back to code-drawn art.
class SkinAssets {
 public:
  static SkinAssets& get();

  const juce::Image& panel(Panel p) const;
  const Filmstrip& knobAmp() const { return knobAmp_; }
  const Filmstrip& knobPedal() const { return knobPedal_; }
  const Filmstrip& footswitch() const { return footswitch_; }
  const Filmstrip& ledOrange() const { return ledOrange_; }

  // Pure helpers, public for tests.
  static bool parseSidecar(const juce::String& json, SidecarInfo& out);
  // Builds a strip from PNG + sidecar bytes; false (and `out` untouched) if either is bad or the
  // PNG size does not match the sidecar.
  static bool loadStrip(const void* png, size_t pngSize, const void* json, size_t jsonSize, Filmstrip& out);
  // Decodes one of the embedded strips again (tests check each against its sidecar).
  static Filmstrip loadEmbeddedStrip(const juce::String& stem);

 private:
  SkinAssets();
  juce::Image panels_[5];
  Filmstrip knobAmp_, knobPedal_, footswitch_, ledOrange_;
};

}  // namespace sawblade::plugin::skin
