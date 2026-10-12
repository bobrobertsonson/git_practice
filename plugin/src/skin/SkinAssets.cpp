#include "SkinAssets.h"

#include <algorithm>

#include "BinaryData.h"

namespace sawblade::plugin::skin {

void Filmstrip::drawFrame(juce::Graphics& g, int frame, juce::Rectangle<float> dest) const {
  if (!valid()) return;
  frame = std::clamp(frame, 0, frames - 1);
  g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
  g.drawImage(image, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(), 0, frame * frameHeight, frameWidth, frameHeight);
}

juce::Image Filmstrip::frameImage(int frame) const {
  if (!valid()) return {};
  frame = std::clamp(frame, 0, frames - 1);
  return image.getClippedImage({0, frame * frameHeight, frameWidth, frameHeight}).createCopy();
}

bool SkinAssets::parseSidecar(const juce::String& json, SidecarInfo& out) {
  const juce::var v = juce::JSON::parse(json);
  auto* o = v.getDynamicObject();
  if (o == nullptr) return false;
  SidecarInfo s;
  s.frames = static_cast<int>(o->getProperty("frames"));
  s.frameWidth = static_cast<int>(o->getProperty("frame_width"));
  s.frameHeight = static_cast<int>(o->getProperty("frame_height"));
  if (o->hasProperty("ring_radius_px")) s.ringRadiusPx = static_cast<double>(o->getProperty("ring_radius_px"));
  if (s.frames <= 0 || s.frameWidth <= 0 || s.frameHeight <= 0) return false;
  out = s;
  return true;
}

bool SkinAssets::loadStrip(const void* png, size_t pngSize, const void* json, size_t jsonSize, Filmstrip& out) {
  if (png == nullptr || json == nullptr) return false;
  SidecarInfo info;
  if (!parseSidecar(juce::String::fromUTF8(static_cast<const char*>(json), static_cast<int>(jsonSize)), info)) return false;
  juce::Image img = juce::ImageFileFormat::loadFrom(png, pngSize);
  if (!img.isValid() || img.getWidth() != info.frameWidth || img.getHeight() != info.frames * info.frameHeight) return false;
  out.image = img.convertedToFormat(juce::Image::ARGB);
  out.frames = info.frames;
  out.frameWidth = info.frameWidth;
  out.frameHeight = info.frameHeight;
  out.ringRadiusPx = info.ringRadiusPx;
  return true;
}

Filmstrip SkinAssets::loadEmbeddedStrip(const juce::String& stem) {
  Filmstrip f;
  int pngSize = 0, jsonSize = 0;
  const char* png = BinaryData::getNamedResource((stem + "_png").toRawUTF8(), pngSize);
  const char* json = BinaryData::getNamedResource((stem + "_json").toRawUTF8(), jsonSize);
  const bool ok = loadStrip(png, static_cast<size_t>(pngSize), json, static_cast<size_t>(jsonSize), f);
  jassert(ok);
  if (!ok) f = {};
  return f;
}

SkinAssets& SkinAssets::get() {
  static SkinAssets assets;
  return assets;
}

SkinAssets::SkinAssets() {
  struct Item { Panel p; const char* data; int size; };
  const Item items[] = {
      {Panel::AmpSaw, BinaryData::amp_saw_png, BinaryData::amp_saw_pngSize},
      {Panel::AmpBody, BinaryData::amp_body_png, BinaryData::amp_body_pngSize},
      {Panel::Cab4x12, BinaryData::cab_4x12_png, BinaryData::cab_4x12_pngSize},
      {Panel::PedalSaw, BinaryData::pedal_saw_png, BinaryData::pedal_saw_pngSize},
      {Panel::PedalBody, BinaryData::pedal_body_png, BinaryData::pedal_body_pngSize},
  };
  for (const auto& it : items) {
    panels_[static_cast<int>(it.p)] = juce::ImageFileFormat::loadFrom(it.data, static_cast<size_t>(it.size));
    jassert(panels_[static_cast<int>(it.p)].isValid());
  }
  knobAmp_ = loadEmbeddedStrip("knob_amp");
  knobPedal_ = loadEmbeddedStrip("knob_pedal");
  footswitch_ = loadEmbeddedStrip("footswitch");
  ledOrange_ = loadEmbeddedStrip("led_orange");
}

const juce::Image& SkinAssets::panel(Panel p) const { return panels_[static_cast<int>(p)]; }

}  // namespace sawblade::plugin::skin
