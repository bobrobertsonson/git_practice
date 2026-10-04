// Tests of the skinned editor (docs/specs/phase2_5_skin.md section 5). Run under a display
// (xvfb-run -a on a headless machine; ctest does this when xvfb-run is installed).

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "PlayAlongPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/CircuitFaces.h"
#include "pedals/PedalFace.h"
#include "pedals/PedalSwitch.h"
#include "skin/FilmstripKnob.h"
#include "skin/FootswitchButton.h"
#include "skin/LedIndicator.h"
#include "skin/RigView.h"
#include "skin/SkinAssets.h"
#include "sawblade/wav_io.h"

using namespace sawblade;
using namespace sawblade::plugin;
using Catch::Matchers::WithinAbs;

namespace {

template <class T>
void collect(juce::Component& c, std::vector<T*>& out) {
  for (auto* child : c.getChildren()) {
    if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
    collect(*child, out);
  }
}
template <class T>
std::vector<T*> all(juce::Component& root) {
  std::vector<T*> v;
  collect<T>(root, v);
  return v;
}

// True if every capture file the preset references (paths.*.blocks[].model.file, cab.ir/irA/irB .file;
// relative to the preset's directory or absolute) exists.
bool presetCapturesPresent(const std::filesystem::path& presetFile) {
  const juce::var root = juce::JSON::parse(juce::File(presetFile.string()));
  if (!root.isObject()) return false;
  const auto dir = presetFile.parent_path();
  auto exists = [&](const juce::var& ref) {
    if (!ref.isObject()) return true;  // nothing referenced
    const juce::String f = ref.getProperty("file", juce::var()).toString();
    if (f.isEmpty()) return true;
    std::filesystem::path q(f.toStdString());
    return std::filesystem::exists(q.is_absolute() ? q : dir / q);
  };
  bool ok = true;
  if (auto* paths = root.getProperty("paths", juce::var()).getDynamicObject())
    for (const auto& kv : paths->getProperties()) {
      const auto* blocks = kv.value.getProperty("blocks", juce::var()).getArray();
      if (blocks != nullptr)
        for (const auto& b : *blocks) ok = ok && exists(b.getProperty("model", juce::var()));
    }
  const juce::var cab = root.getProperty("cab", juce::var());
  for (const char* k : {"ir", "irA", "irB"}) ok = ok && exists(cab.getProperty(k, juce::var()));
  return ok;
}

// A processor with its editor. The barbaric preset is loaded only if all its captures exist (they are
// not committed); otherwise the processor stays on Init. Either way the tests never fail on it.
struct Rig {
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  Rig() {
    const auto preset = std::filesystem::path(SAWBLADE_PRESETS_DIR) / "matched" / "barbaric_v4.json";
    if (std::filesystem::exists(preset) && presetCapturesPresent(preset)) proc.loadPresetFile(preset);
    proc.waitForLoader(std::chrono::milliseconds(60000));
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
  }
  ~Rig() { base.reset(); }

  // Loads a preset and waits for the engine (the parameters then hold its values).
  void load(const std::filesystem::path& file) {
    REQUIRE(proc.loadPresetFile(file));
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(proc.status().error.empty());
    // The loader thread wrote the parameters: let the attachments deliver to the controls.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  }
  void loadInit() {
    proc.loadPreset(makeInitPreset());
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
  }
};

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool shift, int clicks = 1) {
  auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | (shift ? juce::ModifierKeys::shiftModifier : 0));
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos,
                          now, clicks, true);
}

float luminance(juce::Colour c) { return 0.299f * c.getFloatRed() + 0.587f * c.getFloatGreen() + 0.114f * c.getFloatBlue(); }

void savePng(const juce::Image& img, const juce::String& name) {
  const juce::File dir(SAWBLADE_SCREENSHOT_DIR);
  REQUIRE(dir.createDirectory().wasOk());
  const juce::File f = dir.getChildFile(name);
  f.deleteFile();
  juce::FileOutputStream out(f);
  REQUIRE(out.openedOk());
  juce::PNGImageFormat png;
  REQUIRE(png.writeImageToStream(img, out));
}

// Fraction of pixels of `r` that differ visibly from the plain editor background.
double nonBackgroundFraction(const juce::Image& img, juce::Rectangle<int> r) {
  const juce::Colour bg = SawbladeLookAndFeel::background();
  r = r.getIntersection(img.getBounds());
  int n = 0, diff = 0;
  for (int y = r.getY(); y < r.getBottom(); ++y)
    for (int x = r.getX(); x < r.getRight(); ++x) {
      const auto p = img.getPixelAt(x, y);
      ++n;
      if (std::abs(p.getRed() - bg.getRed()) + std::abs(p.getGreen() - bg.getGreen()) + std::abs(p.getBlue() - bg.getBlue()) > 40) ++diff;
    }
  return n > 0 ? static_cast<double>(diff) / n : 0.0;
}

void checkSnapshot(Rig& rig, float scale, const char* file) {
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  const juce::Image img = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, scale);
  REQUIRE(img.getWidth() == juce::roundToInt(1280 * scale));
  REQUIRE(img.getHeight() == juce::roundToInt(800 * scale));

  // not blank: luminance standard deviation
  double sum = 0, sum2 = 0;
  const int n = img.getWidth() * img.getHeight();
  for (int y = 0; y < img.getHeight(); ++y)
    for (int x = 0; x < img.getWidth(); ++x) {
      const double l = luminance(img.getPixelAt(x, y));
      sum += l;
      sum2 += l * l;
    }
  const double mean = sum / n, sd = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
  INFO("luminance sd " << sd);
  CHECK(sd > 0.03);

  // the amp image region is not the background colour
  auto rig_ = all<skin::RigPiece>(*rig.ed);
  REQUIRE(rig_.size() == 5);
  for (auto* piece : rig_) {
    const auto area = rig.ed->getLocalArea(piece, piece->getLocalBounds());
    const juce::Rectangle<int> scaled(juce::roundToInt(area.getX() * scale), juce::roundToInt(area.getY() * scale),
                                      juce::roundToInt(area.getWidth() * scale), juce::roundToInt(area.getHeight() * scale));
    INFO(piece->getTitle());
    CHECK(nonBackgroundFraction(img, scaled) > 0.2);  // the renders are dark, but never plain background
  }
  savePng(img, file);
}

}  // namespace

TEST_CASE("snapshots 1x and 2x", "[editor]") {
  Rig rig;
  checkSnapshot(rig, 1.0f, "sawblade_skin_1x.png");
  checkSnapshot(rig, 2.0f, "sawblade_skin_2x.png");
}

TEST_CASE("resizing keeps the aspect and scales the content", "[editor]") {
  Rig rig;
  rig.ed->setSize(640, 400);
  CHECK_THAT(rig.ed->contentScale(), WithinAbs(0.5, 1e-9));
  rig.ed->setSize(2560, 1600);
  CHECK_THAT(rig.ed->contentScale(), WithinAbs(2.0, 1e-9));
  rig.ed->setSize(1280, 800);
  CHECK_THAT(rig.ed->contentScale(), WithinAbs(1.0, 1e-9));

  // a 1000 x 800 request is pulled onto the 1.6 aspect, within the limits
  rig.ed->getConstrainer()->setBoundsForComponent(rig.ed, {0, 0, 1000, 800}, false, false, false, false);
  const double aspect = static_cast<double>(rig.ed->getWidth()) / rig.ed->getHeight();
  INFO("constrained to " << rig.ed->getWidth() << " x " << rig.ed->getHeight());
  CHECK_THAT(aspect, WithinAbs(1.6, 0.01));
  CHECK(rig.ed->getWidth() >= 640);
  CHECK(rig.ed->getWidth() <= 2560);
  // the limits hold for absurd requests too
  rig.ed->getConstrainer()->setBoundsForComponent(rig.ed, {0, 0, 100, 100}, false, false, false, false);
  CHECK(rig.ed->getWidth() >= 640);
  CHECK(rig.ed->getHeight() >= 400);
}

TEST_CASE("every parameter has exactly one bound control", "[editor]") {
  Rig rig;
  auto knobs = all<skin::FilmstripKnob>(*rig.ed);
  auto switches = all<PedalSwitch>(*rig.ed);
  std::map<std::string, int> nKnobs, nSwitches;
  for (auto* k : knobs) ++nKnobs[k->paramId().toStdString()];
  for (auto* sw : switches) ++nSwitches[sw->paramId().toStdString()];
  std::set<std::string> focusParams;
  for (int c = 0; c < kNumCircuits; ++c) focusParams.insert(paramSpec(circuitFace(static_cast<Circuit>(c)).focus.param).id);
  CHECK(static_cast<int>(nKnobs.size() + nSwitches.size()) >= kNumParams);

  for (int i = 0; i < kNumParams; ++i) {
    const ParamSpec& s = paramSpec(i);
    INFO(s.id);
    // One knob or one switch; a FOCUS parameter has one knob (the drawer) and one switch (the face).
    if (focusParams.count(s.id)) {
      CHECK(nKnobs[s.id] == 1);
      CHECK(nSwitches[s.id] == 1);
    } else {
      CHECK(nKnobs[s.id] + nSwitches[s.id] == 1);
    }
    if (!s.choices.empty()) CHECK(nKnobs[s.id] == 0);  // choices are switches
    if (nKnobs[s.id] == 0) continue;
    auto it = std::find_if(knobs.begin(), knobs.end(), [&](auto* k) { return k->paramId().toStdString() == s.id; });
    REQUIRE(it != knobs.end());
    skin::FilmstripKnob& knob = **it;
    auto* param = rig.proc.parameters().getParameter(s.id);
    REQUIRE(param != nullptr);

    // knob -> parameter
    knob.setValue(knob.proportionOfLengthToValue(0.8), juce::sendNotificationSync);
    CHECK_THAT(static_cast<double>(param->getValue()), WithinAbs(0.8, 1e-3));
    // parameter -> knob
    param->setValueNotifyingHost(0.25f);
    CHECK_THAT(knob.proportion(), WithinAbs(0.25, 1e-3));
  }
  // No parameter id appears twice among the knobs of one component set, and no knob is unbound.
  for (auto& kv : nKnobs) CHECK(kv.second <= 1);
  for (auto& kv : nSwitches) CHECK(kv.second <= 1);
}

TEST_CASE("filmstrip mapping and embedded sidecars", "[editor]") {
  using skin::FilmstripKnob;
  CHECK(FilmstripKnob::frameForProportion(0.0, 128) == 0);
  CHECK(FilmstripKnob::frameForProportion(1.0, 128) == 127);
  CHECK(FilmstripKnob::frameForProportion(0.5, 128) == 64);
  CHECK(FilmstripKnob::frameForProportion(-3.0, 128) == 0);
  CHECK(FilmstripKnob::frameForProportion(7.0, 128) == 127);

  juce::ScopedJuceInitialiser_GUI gui;
  struct Expect { const char* stem; int frames; };
  for (const Expect e : {Expect{"knob_amp", 128}, Expect{"knob_pedal", 128}, Expect{"footswitch", 2}, Expect{"led_orange", 2}}) {
    INFO(e.stem);
    const auto f = skin::SkinAssets::loadEmbeddedStrip(e.stem);
    REQUIRE(f.valid());
    CHECK(f.frames == e.frames);
    CHECK(f.image.getHeight() == f.frames * f.frameHeight);
    CHECK(f.image.getWidth() == f.frameWidth);
  }
  // the lazily decoded copies the controls use
  const auto& a = skin::SkinAssets::get();
  CHECK(a.knobAmp().frames == 128);
  CHECK(a.knobPedal().frames == 128);
  CHECK(a.footswitch().frames == 2);
  CHECK(a.ledOrange().frames == 2);
  for (auto p : {skin::Panel::AmpSaw, skin::Panel::AmpBody, skin::Panel::Cab4x12, skin::Panel::PedalSaw, skin::Panel::PedalBody})
    CHECK(a.panel(p).isValid());

  // garbled input never crashes and is rejected
  skin::SidecarInfo info;
  CHECK_FALSE(skin::SkinAssets::parseSidecar("not json", info));
  CHECK_FALSE(skin::SkinAssets::parseSidecar("{\"frames\": 0}", info));
  skin::Filmstrip f;
  const char junk[] = "junk";
  CHECK_FALSE(skin::SkinAssets::loadStrip(junk, sizeof junk, junk, sizeof junk, f));
  CHECK_FALSE(skin::SkinAssets::loadStrip(nullptr, 0, nullptr, 0, f));
}

TEST_CASE("knob interaction: drag, shift, horizontal, double-click", "[editor]") {
  Rig rig;
  auto knobs = all<skin::FilmstripKnob>(*rig.ed);
  auto it = std::find_if(knobs.begin(), knobs.end(), [](auto* k) { return k->paramId() == "blend"; });
  REQUIRE(it != knobs.end());
  skin::FilmstripKnob& k = **it;
  const juce::Point<float> start(40.0f, 60.0f);

  auto dragBy = [&](float dx, float dy, bool shift) {
    const auto down = mouse(k, start, start, shift);
    k.mouseDown(down);
    k.mouseDrag(mouse(k, start + juce::Point<float>(dx, dy), start, shift));
    k.mouseUp(mouse(k, start + juce::Point<float>(dx, dy), start, shift));
  };

  k.setValue(k.proportionOfLengthToValue(0.3), juce::sendNotificationSync);
  dragBy(0.0f, -100.0f, false);  // 100 px up
  CHECK_THAT(k.proportion(), WithinAbs(0.7, 0.02));

  k.setValue(k.proportionOfLengthToValue(0.3), juce::sendNotificationSync);
  dragBy(0.0f, -100.0f, true);  // shift = fine
  CHECK_THAT(k.proportion(), WithinAbs(0.34, 0.005));

  k.setValue(k.proportionOfLengthToValue(0.3), juce::sendNotificationSync);
  dragBy(120.0f, 0.0f, false);  // horizontal: no change
  CHECK_THAT(k.proportion(), WithinAbs(0.3, 1e-6));

  // dragging down lowers it, and a long drag clamps at the ends
  k.setValue(k.proportionOfLengthToValue(0.3), juce::sendNotificationSync);
  dragBy(0.0f, 50.0f, false);
  CHECK_THAT(k.proportion(), WithinAbs(0.1, 0.02));
  dragBy(0.0f, -1000.0f, false);
  CHECK_THAT(k.proportion(), WithinAbs(1.0, 1e-9));

  // double-click returns to the parameter default
  k.setValue(k.proportionOfLengthToValue(0.9), juce::sendNotificationSync);
  k.mouseDoubleClick(mouse(k, start, start, false, 2));
  auto* param = rig.proc.parameters().getParameter("blend");
  CHECK_THAT(static_cast<double>(param->getValue()), WithinAbs(static_cast<double>(param->getDefaultValue()), 1e-6));
  CHECK_THAT(k.proportion(), WithinAbs(static_cast<double>(param->getDefaultValue()), 1e-6));
}

TEST_CASE("footswitch press look and paired LED", "[editor]") {
  Rig rig;
  auto switches = all<skin::FootswitchButton>(*rig.ed);
  auto leds = all<skin::LedIndicator>(*rig.ed);
  REQUIRE(switches.size() == 2);
  REQUIRE(leds.size() == 2);
  skin::FootswitchButton& fs = *switches[0];

  const auto up = fs.createComponentSnapshot(fs.getLocalBounds(), true, 1.0f);
  juce::Component& fsc = fs;  // Button's mouse handlers are protected; Component's are public and virtual
  const auto centre = fs.getLocalBounds().toFloat().getCentre();
  fsc.mouseDown(mouse(fs, centre, centre, false));
  CHECK(fs.isDown());
  const auto down = fs.createComponentSnapshot(fs.getLocalBounds(), true, 1.0f);
  fsc.mouseUp(mouse(fs, centre, centre, false));  // completes a click: toggles, then toggled back below
  fs.setToggleState(true, juce::dontSendNotification);
  fs.pairedLed()->setOn(true);
  int differing = 0;
  for (int y = 0; y < up.getHeight(); ++y)
    for (int x = 0; x < up.getWidth(); ++x)
      if (up.getPixelAt(x, y) != down.getPixelAt(x, y)) ++differing;
  CHECK(differing > 100);

  // clicking toggles the paired LED
  skin::LedIndicator* led = fs.pairedLed();
  REQUIRE(led != nullptr);
  CHECK(led != switches[1]->pairedLed());
  const bool on0 = fs.getToggleState();
  CHECK(led->isOn() == on0);
  auto click = [&] {  // a real click: mouse down + up inside the button
    fsc.mouseDown(mouse(fs, centre, centre, false));
    fsc.mouseUp(mouse(fs, centre, centre, false));
  };
  click();
  CHECK(fs.getToggleState() == !on0);
  CHECK(led->isOn() == !on0);
  click();
  CHECK(fs.getToggleState() == on0);
  CHECK(led->isOn() == on0);

  // LED on vs off differs outside the sprite (the code-drawn glow)
  skin::LedIndicator& l0 = *leds[0];
  l0.setOn(true);
  const auto lit = l0.createComponentSnapshot(l0.getLocalBounds(), true, 1.0f);
  l0.setOn(false);
  const auto dark = l0.createComponentSnapshot(l0.getLocalBounds(), true, 1.0f);
  const auto sprite = l0.spriteBounds().toNearestInt();
  int glow = 0;
  for (int y = 0; y < lit.getHeight(); ++y)
    for (int x = 0; x < lit.getWidth(); ++x)
      if (!sprite.contains(x, y) && lit.getPixelAt(x, y) != dark.getPixelAt(x, y)) ++glow;
  CHECK(glow > 200);
}

TEST_CASE("accessibility: titles and tooltips on every control", "[editor]") {
  Rig rig;
  int checked = 0;
  auto check = [&](juce::Component& c, const juce::String& tip) {
    INFO("component: " << c.getTitle() << " / " << tip);
    CHECK(c.getTitle().isNotEmpty());
    CHECK(tip.isNotEmpty());
    ++checked;
  };
  for (auto* s : all<juce::Slider>(*rig.ed)) {
    check(*s, s->getTooltip());
    CHECK(s->getTextFromValue(s->getValue()).isNotEmpty());
  }
  for (auto* b : all<juce::Button>(*rig.ed)) check(*b, b->getTooltip());
  for (auto* p : all<skin::RigPiece>(*rig.ed)) check(*p, p->getTooltip());
  CHECK(checked >= 12 + 8 + 5);

  // value text carries the unit
  for (auto* s : all<skin::FilmstripKnob>(*rig.ed))
    if (s->paramId() == "inputGain") CHECK(s->getTextFromValue(3.0).contains("dB"));
}

TEST_CASE("clicking a rig piece selects it", "[editor]") {
  Rig rig;
  CHECK(rig.ed->selectedPiece() == skin::Piece::SawPedal);
  for (auto* p : all<skin::RigPiece>(*rig.ed)) {
    p->mouseDown(mouse(*p, {5.0f, 5.0f}, {5.0f, 5.0f}, false));
    CHECK(rig.ed->selectedPiece() == p->piece());
  }
}

// ---- play-along panel (docs/specs/phase5_2_playalong_plugin.md) ------------------------------------------
namespace {

// Synthetic song (never committed): a folder of four stems, `seconds` long at 48 kHz.
std::filesystem::path writeSyntheticSong(const std::filesystem::path& root, const std::string& name, double seconds) {
  namespace fs = std::filesystem;
  const fs::path dir = root / name;
  fs::create_directories(dir);
  const auto n = static_cast<std::size_t>(seconds * 48000.0);
  auto make = [&](double hz, double amp, double pulseHz) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / 48000.0;
      const double env = pulseHz > 0.0 ? std::exp(-8.0 * std::fmod(t * pulseHz, 1.0)) : 1.0;
      x[i] = static_cast<float>(amp * env * std::sin(6.283185307179586 * hz * t));
    }
    return x;
  };
  const auto drums = make(180.0, 0.4, 2.0), bass = make(55.0, 0.3, 0.0), vocals = make(440.0, 0.15, 0.5), other = make(220.0, 0.3, 4.0);
  sawblade::writeWavFloat32Stereo(dir / "drums.wav", 48000.0, drums, drums);
  sawblade::writeWavFloat32Stereo(dir / "bass.wav", 48000.0, bass, bass);
  sawblade::writeWavFloat32Stereo(dir / "vocals.wav", 48000.0, vocals, vocals);
  sawblade::writeWavFloat32Stereo(dir / "other.wav", 48000.0, other, other);
  return dir;
}

struct TempFolder {
  std::filesystem::path dir;
  TempFolder() {
    dir = std::filesystem::temp_directory_path() / ("sawblade_editor_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt()));
    std::filesystem::create_directories(dir);
  }
  ~TempFolder() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

void processBlocks(SawbladeProcessor& p, int blocks) {
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  for (int i = 0; i < blocks; ++i) {
    buf.clear();
    p.processBlock(buf, midi);
  }
}

// A real click (mouse down + up inside the button); Button::triggerClick() is asynchronous.
void click(juce::Button& b) {
  juce::Component& c = b;
  const auto centre = b.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(b, centre, centre, false));
  c.mouseUp(mouse(b, centre, centre, false));
}

juce::Button* buttonTitled(juce::Component& root, const juce::String& title) {
  for (auto* b : all<juce::Button>(root))
    if (b->getTitle() == title) return b;
  return nullptr;
}
juce::Slider* sliderTitled(juce::Component& root, const juce::String& title) {
  for (auto* s : all<juce::Slider>(root))
    if (s->getTitle() == title) return s;
  return nullptr;
}
bool anyLabelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}

}  // namespace

TEST_CASE("play-along: the panel exists, is closed by default and opens from the top bar", "[editor][playalong]") {
  Rig rig;
  auto panels = all<PlayAlongPanel>(*rig.ed);
  REQUIRE(panels.size() == 1);
  PlayAlongPanel& panel = *panels[0];
  CHECK_FALSE(panel.isVisible());
  CHECK_FALSE(rig.ed->playAlongOpen());
  juce::Button* toggle = buttonTitled(*rig.ed, "PLAY ALONG");
  REQUIRE(toggle != nullptr);
  CHECK(toggle->isEnabled());
  CHECK(toggle->getTooltip().isNotEmpty());
  // It is on the top bar and the panel is docked along the bottom of the 1280 x 800 design.
  const auto tb = rig.ed->getLocalArea(toggle, toggle->getLocalBounds());
  CHECK(tb.getBottom() <= 58);
  const auto pb = rig.ed->getLocalArea(&panel, panel.getLocalBounds());
  CHECK(pb.getBottom() == SawbladeEditor::kDesignHeight);
  CHECK(pb.getWidth() == SawbladeEditor::kDesignWidth);
  CHECK(pb.getY() > 400);

  click(*toggle);
  CHECK(panel.isVisible());
  CHECK(rig.ed->playAlongOpen());
  CHECK(toggle->getToggleState());
  click(*toggle);
  CHECK_FALSE(panel.isVisible());

  // The open / closed state is UI state: it is not in the plugin state.
  rig.ed->setPlayAlongOpen(true);
  juce::MemoryBlock state;
  rig.proc.getStateInformation(state);
  CHECK(juce::String::fromUTF8(static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())).contains("playAlong") == false);
  rig.ed->setPlayAlongOpen(false);

  // Existing top-bar controls still fit: nothing overlaps the new button.
  for (auto* b : all<juce::Button>(*rig.ed)) {
    if (b == toggle || !b->isShowing() || !b->getParentComponent() || b->getParentComponent() != toggle->getParentComponent()) continue;
    if (b->getY() > 58) continue;
    INFO(b->getTitle());
    CHECK_FALSE(b->getBounds().intersects(toggle->getBounds()));
  }
}

TEST_CASE("play-along: the controls are bound to the processor", "[editor][playalong]") {
  Rig rig;
  TempFolder tmp;
  rig.ed->setPlayAlongOpen(true);
  auto& pa = rig.proc.playAlong();
  auto* panel = all<PlayAlongPanel>(*rig.ed).at(0);

  // Every expected control exists, with a title and a tooltip.
  for (const char* title : {"LOAD SONG", "KEEP KEYS", "PLAY", "SET A", "SET B", "LOOP", "COUNT-IN", "MUTE", "GHOST", "FULL", "SYNC TO HOST"}) {
    INFO(title);
    auto* b = buttonTitled(*panel, title);
    REQUIRE(b != nullptr);
    CHECK(b->getTooltip().isNotEmpty());
  }
  for (const char* title : {"Seek", "Count-in BPM", "Backing level", "Backing offset"}) {
    INFO(title);
    auto* s = sliderTitled(*panel, title);
    REQUIRE(s != nullptr);
    CHECK(s->getTooltip().isNotEmpty());
  }

  // Control -> settings.
  auto* level = sliderTitled(*panel, "Backing level");
  level->setValue(-12.5, juce::sendNotificationSync);
  CHECK_THAT(pa.settings().levelDb, WithinAbs(-12.5, 1e-9));
  auto* offset = sliderTitled(*panel, "Backing offset");
  offset->setValue(190.0, juce::sendNotificationSync);
  CHECK_THAT(pa.settings().offsetMs, WithinAbs(190.0, 1e-9));
  auto* bpm = sliderTitled(*panel, "Count-in BPM");
  bpm->setValue(150.0, juce::sendNotificationSync);
  click(*buttonTitled(*panel, "COUNT-IN"));
  CHECK(pa.settings().countIn);
  CHECK_THAT(pa.settings().bpm, WithinAbs(150.0, 1e-9));
  click(*buttonTitled(*panel, "GHOST"));
  CHECK(pa.settings().guitarMode == sawblade::GuitarMode::Ghost);
  click(*buttonTitled(*panel, "FULL"));
  CHECK(pa.settings().guitarMode == sawblade::GuitarMode::Full);
  click(*buttonTitled(*panel, "MUTE"));
  CHECK(pa.settings().guitarMode == sawblade::GuitarMode::Muted);
  click(*buttonTitled(*panel, "KEEP KEYS"));
  CHECK(pa.settings().keepOther);
  click(*buttonTitled(*panel, "KEEP KEYS"));
  CHECK_FALSE(pa.settings().keepOther);

  // Plugin mode (the default here): the backing is off and the host owns the transport.
  auto* sync = buttonTitled(*panel, "SYNC TO HOST");
  panel->refresh();
  CHECK(sync->isVisible());
  CHECK_FALSE(sync->getToggleState());
  CHECK_FALSE(buttonTitled(*panel, "PLAY")->isEnabled());
  click(*sync);
  CHECK(pa.settings().hostSync);
  click(*sync);
  CHECK_FALSE(pa.settings().hostSync);

  // Settings -> control.
  pa.setLevelDb(-5.0);
  pa.setGuitarMode(sawblade::GuitarMode::Ghost);
  pa.setCountIn(false, 99.0);
  pa.setOffsetMs(-40.0);
  panel->refresh();
  CHECK_THAT(level->getValue(), WithinAbs(-5.0, 1e-9));
  CHECK(buttonTitled(*panel, "GHOST")->getToggleState());
  CHECK_FALSE(buttonTitled(*panel, "MUTE")->getToggleState());
  CHECK_FALSE(buttonTitled(*panel, "COUNT-IN")->getToggleState());
  CHECK_THAT(bpm->getValue(), WithinAbs(99.0, 1e-9));
  CHECK_THAT(offset->getValue(), WithinAbs(-40.0, 1e-9));
  // Refreshing never feeds the values back as new edits.
  CHECK_THAT(pa.settings().levelDb, WithinAbs(-5.0, 1e-9));

  // Standalone: free-run transport. Load a song, then play / seek / loop from the panel.
  pa.setStandalone(true);
  pa.setOffsetMs(0.0);
  rig.proc.prepareToPlay(48000.0, 512);
  const auto song = writeSyntheticSong(tmp.dir, "song", 20.0);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  processBlocks(rig.proc, 4);
  panel->refresh();
  CHECK_FALSE(sync->isVisible());
  auto* play = buttonTitled(*panel, "PLAY");
  REQUIRE(play->isEnabled());
  auto* seek = sliderTitled(*panel, "Seek");
  CHECK_THAT(seek->getMaximum(), WithinAbs(20.0, 0.01));
  seek->setValue(7.5, juce::sendNotificationSync);
  processBlocks(rig.proc, 2);
  CHECK(std::abs(static_cast<double>(pa.snapshot().position) / 48000.0 - 7.5) < 0.05);
  click(*play);
  processBlocks(rig.proc, 40);
  panel->refresh();
  CHECK(pa.snapshot().playing);
  CHECK(play->getButtonText() == "PAUSE");
  click(*play);
  processBlocks(rig.proc, 40);
  panel->refresh();
  CHECK_FALSE(pa.snapshot().playing);
  CHECK(play->getButtonText() == "PLAY");

  // Loop A / B from the current position.
  auto* loop = buttonTitled(*panel, "LOOP");
  CHECK_FALSE(loop->isEnabled());
  seek->setValue(5.0, juce::sendNotificationSync);
  processBlocks(rig.proc, 2);
  click(*buttonTitled(*panel, "SET A"));
  seek->setValue(9.0, juce::sendNotificationSync);
  processBlocks(rig.proc, 2);
  click(*buttonTitled(*panel, "SET B"));
  CHECK(std::abs(pa.settings().loopAMs - 5000.0) < 20.0);
  CHECK(std::abs(pa.settings().loopBMs - 9000.0) < 20.0);
  panel->refresh();
  REQUIRE(loop->isEnabled());
  click(*loop);
  CHECK(pa.settings().loopOn);
  processBlocks(rig.proc, 2);
  CHECK(pa.snapshot().loopActive);
  CHECK(pa.snapshot().loopStart == 240000);
}

TEST_CASE("play-along: dropping a folder loads it; a missing folder shows a message", "[editor][playalong]") {
  Rig rig;
  TempFolder tmp;
  auto& pa = rig.proc.playAlong();
  const auto song = writeSyntheticSong(tmp.dir, "drop", 6.0);
  juce::StringArray dirs;
  dirs.add(juce::String(song.string()));
  CHECK(rig.ed->isInterestedInFileDrag(dirs));
  juce::StringArray notDirs;
  notDirs.add(juce::String((song / "drums.wav").string()));
  CHECK_FALSE(rig.ed->isInterestedInFileDrag(notDirs));

  CHECK_FALSE(rig.ed->playAlongOpen());
  rig.ed->filesDropped(dirs, 10, 10);
  CHECK(rig.ed->playAlongOpen());
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == PlayAlong::LoadStatus::State::Ready);
  CHECK(pa.settings().folder == song.string());
  all<PlayAlongPanel>(*rig.ed).at(0)->refresh();
  CHECK(anyLabelContains(*rig.ed, "drop"));

  // A missing folder (e.g. from a restored session): a clear message, no throw, the path is kept.
  PlayAlongSettings s;
  s.folder = (tmp.dir / "vanished").string();
  REQUIRE_NOTHROW(pa.restore(s));
  REQUIRE(pa.waitForLoader());
  all<PlayAlongPanel>(*rig.ed).at(0)->refresh();
  CHECK(anyLabelContains(*rig.ed, "Song folder not found"));
  CHECK(anyLabelContains(*rig.ed, "vanished"));
}

TEST_CASE("play-along: screenshots with the panel closed and open", "[editor][playalong]") {
  Rig rig;
  TempFolder tmp;
  auto& pa = rig.proc.playAlong();
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);

  // Closed: the same screen as before, plus the PLAY ALONG button.
  rig.ed->setPlayAlongOpen(false);
  const juce::Image closed = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  REQUIRE(closed.getWidth() == 1280);
  REQUIRE(closed.getHeight() == 800);
  savePng(closed, "sawblade_playalong_closed_1x.png");

  // Open, with a song loaded (Standalone look: free-run transport), a loop set and the count-in on.
  pa.setStandalone(true);
  rig.proc.prepareToPlay(48000.0, 512);
  const auto song = writeSyntheticSong(tmp.dir, "Gatecreeper - Dark Superstition (stems)", 60.0);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  processBlocks(rig.proc, 4);
  pa.setCountIn(true, 142.0);
  pa.setLoopMs(21000.0, 33500.0, true);
  pa.seekSamples(26 * 48000 + 7200);
  processBlocks(rig.proc, 3);
  rig.ed->setPlayAlongOpen(true);
  const juce::Image open = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  savePng(open, "sawblade_playalong_open_1x.png");

  // The panel region differs from the closed screen and is not plain background; above it nothing moved.
  const juce::Rectangle<int> region(0, 800 - PlayAlongPanel::kHeight, 1280, PlayAlongPanel::kHeight);
  int differing = 0, total = 0;
  for (int y = region.getY(); y < region.getBottom(); ++y)
    for (int x = region.getX(); x < region.getRight(); ++x, ++total)
      if (open.getPixelAt(x, y) != closed.getPixelAt(x, y)) ++differing;
  CHECK(differing > total / 2);
  CHECK(nonBackgroundFraction(open, region) > 0.1);
  // Strip between the top bar and the panel is the same, apart from the PLAY ALONG button's pressed look.
  int changedAbove = 0;
  for (int y = 70; y < region.getY() - 2; ++y)
    for (int x = 0; x < 1280; ++x)
      if (open.getPixelAt(x, y) != closed.getPixelAt(x, y)) ++changedAbove;
  CHECK(changedAbove == 0);
  // The open panel shows the loaded song and no error.
  CHECK(anyLabelContains(*rig.ed, "Gatecreeper"));
  CHECK_FALSE(anyLabelContains(*rig.ed, "not found"));
}

TEST_CASE("play-along: in plugin mode with sync off the status shows warnings and the suggestion before the off hint", "[editor][playalong]") {
  Rig rig;
  TempFolder tmp;
  auto& pa = rig.proc.playAlong();
  auto* panel = all<PlayAlongPanel>(*rig.ed).at(0);
  REQUIRE_FALSE(pa.standalone());
  REQUIRE_FALSE(pa.settings().hostSync);

  // A folder with an unknown stem name: the loader warning wins over the hint.
  const auto warn = writeSyntheticSong(tmp.dir, "warn", 4.0);
  std::vector<float> x(48000, 0.1f);
  sawblade::writeWavFloat32Stereo(warn / "piano.wav", 48000.0, x, x);
  pa.loadFolder(warn.string(), false);
  REQUIRE(pa.waitForLoader());
  panel->refresh();
  CHECK(anyLabelContains(*rig.ed, "piano.wav"));
  CHECK_FALSE(anyLabelContains(*rig.ed, "Backing is off"));

  // A clean user load: the one-time level suggestion is shown.
  const auto clean = writeSyntheticSong(tmp.dir, "clean", 4.0);
  pa.loadFolder(clean.string(), true);
  REQUIRE(pa.waitForLoader());
  panel->refresh();
  CHECK(anyLabelContains(*rig.ed, "Level set to"));
  CHECK_FALSE(anyLabelContains(*rig.ed, "Backing is off"));

  // A restored (non-user) clean load has nothing to say but the hint.
  PlayAlongSettings s = pa.settings();
  s.folder = clean.string();
  pa.restore(s);
  REQUIRE(pa.waitForLoader());
  panel->refresh();
  CHECK(anyLabelContains(*rig.ed, "Backing is off"));
}

// ---------------------------------------------------------------------------------------------
// Pedal face and advanced drawer (docs/specs/phase7b_chainsaw_pedal.md, 5.3-5.6; acceptance 13).
namespace {

constexpr int hmP(int live) { return kHmFirst + live; }
constexpr int muffP(int live) { return kMuffFirst + live; }

const std::filesystem::path kChainsawPresets = std::filesystem::path(SAWBLADE_PRESETS_DIR) / "modeled" / "chainsaw";

PedalFace& faceOf(Rig& rig) {
  auto v = all<PedalFace>(*rig.ed);
  REQUIRE(v.size() == 1);
  return *v[0];
}
AdvancedDrawer& drawerOf(Rig& rig) {
  auto v = all<AdvancedDrawer>(*rig.ed);
  REQUIRE(v.size() == 1);
  return *v[0];
}
skin::RigView& rigViewOf(Rig& rig) {
  auto v = all<skin::RigView>(*rig.ed);
  REQUIRE(v.size() == 1);
  return *v[0];
}

void setParam(Rig& rig, int index, double value) {
  auto* p = rig.proc.parameters().getParameter(paramSpec(index).id);
  p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(value)));
}
double getParam(Rig& rig, int index) { return static_cast<double>(rig.proc.parameters().getRawParameterValue(paramSpec(index).id)->load()); }

void press(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(c, centre, centre, false));
}

void doubleClick(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDoubleClick(mouse(c, centre, centre, false, 2));
}

// Every string a player can see for the pedal controls: parameter and choice names, titles, tooltips,
// drawer labels, OLED lines and the table's panel labels.
std::vector<juce::String> uiStrings(Rig& rig) {
  std::vector<juce::String> out;
  for (int i = 0; i < kNumParams; ++i) {
    auto* p = rig.proc.parameters().getParameter(paramSpec(i).id);
    out.push_back(p->getName(128));
    for (const auto& v : p->getAllValueStrings()) out.push_back(v);
  }
  std::function<void(juce::Component&)> walk = [&](juce::Component& c) {
    out.push_back(c.getTitle());
    if (auto* t = dynamic_cast<juce::SettableTooltipClient*>(&c)) out.push_back(t->getTooltip());
    if (auto* l = dynamic_cast<juce::Label*>(&c)) out.push_back(l->getText());
    if (auto* b = dynamic_cast<juce::Button*>(&c)) out.push_back(b->getButtonText());
    for (auto* child : c.getChildren()) walk(*child);
  };
  walk(*rig.ed);
  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    out.push_back(f.oledName);
    out.push_back(f.focus.label);
    for (const auto& k : f.knobs) out.push_back(k.label);
    for (const auto& k : f.drawerKnobs) out.push_back(k.label);
    for (const auto& k : f.drawerSwitches) out.push_back(k.label);
  }
  return out;
}

}  // namespace

TEST_CASE("pedal face: shown only when the preset has a circuit block", "[editor][pedal]") {
  Rig rig;
  PedalFace& face = faceOf(rig);
  rig.loadInit();
  face.refresh();
  CHECK_FALSE(face.isVisible());
  CHECK_FALSE(face.activeCircuit().has_value());

  rig.load(kChainsawPresets / "classic_buzzsaw.json");
  face.refresh();
  CHECK(face.isVisible());
  REQUIRE(face.activeCircuit().has_value());
  CHECK(*face.activeCircuit() == Circuit::Chainsaw);

  // The face lies exactly over the saw pedal render and lets background clicks through.
  const auto& piece = rigViewOf(rig).piece(skin::Piece::SawPedal);
  CHECK(face.getBounds() == piece.getBounds() + rigViewOf(rig).getPosition());
  bool self = true, kids = false;
  face.getInterceptsMouseClicks(self, kids);
  CHECK_FALSE(self);
  CHECK(kids);
  CHECK(rig.ed->getLocalArea(&face, face.getLocalBounds()).getWidth() == piece.getWidth());

  rig.loadInit();
  face.refresh();
  CHECK_FALSE(face.isVisible());
}

TEST_CASE("pedal face: controls round-trip with the parameters; FOCUS and CIRCUIT behave", "[editor][pedal]") {
  Rig rig;
  PedalFace& face = faceOf(rig);
  rig.load(kChainsawPresets / "classic_buzzsaw.json");
  face.refresh();

  // Knobs: knob -> parameter -> knob at 0.8 / 0.25, for both circuits' sets.
  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    for (int k = 0; k < 6; ++k) {
      auto* knob = face.knob(static_cast<Circuit>(c), k);
      REQUIRE(knob != nullptr);
      auto* param = rig.proc.parameters().getParameter(paramSpec(f.knobs[static_cast<size_t>(k)].param).id);
      INFO(param->getName(64));
      CHECK(knob->paramId() == param->paramID);
      knob->setValue(knob->proportionOfLengthToValue(0.8), juce::sendNotificationSync);
      CHECK_THAT(static_cast<double>(param->getValue()), WithinAbs(0.8, 1e-3));
      param->setValueNotifyingHost(0.25f);
      CHECK_THAT(knob->proportion(), WithinAbs(0.25, 1e-3));
      CHECK(knob->getTitle().isNotEmpty());
    }
  }

  // CLIP: pressing cycles through the four clips and wraps; the parameter and the switch agree.
  for (int c = 0; c < kNumCircuits; ++c) {
    const CircuitFace& f = circuitFace(static_cast<Circuit>(c));
    PedalSwitch& clip = face.clipSwitch(static_cast<Circuit>(c));
    setParam(rig, f.clipParam, 0.0);
    CHECK(clip.position() == 0);
    for (int expect : {1, 2, 3, 0}) {
      press(clip);
      CHECK(clip.position() == expect);
      CHECK(getParam(rig, f.clipParam) == expect);
    }
    setParam(rig, f.clipParam, 2.0);  // parameter -> switch
    CHECK(clip.position() == 2);
    CHECK(clip.valueText() == clipShortName(2));
    // the mouse wheel steps without wrapping
    juce::MouseWheelDetails w{};
    w.deltaY = 1.0f;
    clip.mouseWheelMove(mouse(clip, {1.0f, 1.0f}, {1.0f, 1.0f}, false), w);
    CHECK(clip.position() == 3);
    clip.mouseWheelMove(mouse(clip, {1.0f, 1.0f}, {1.0f, 1.0f}, false), w);
    CHECK(clip.position() == 3);
    w.deltaY = -1.0f;
    clip.mouseWheelMove(mouse(clip, {1.0f, 1.0f}, {1.0f, 1.0f}, false), w);
    CHECK(clip.position() == 2);
  }

  // FOCUS: writes its two values; reads NARROW on the narrow side of the threshold.
  for (int c = 0; c < kNumCircuits; ++c) {
    const FaceSwitchSpec& fs = circuitFace(static_cast<Circuit>(c)).focus;
    PedalSwitch& focus = face.focusSwitch(static_cast<Circuit>(c));
    setParam(rig, fs.param, fs.wideValue);
    CHECK(focus.position() == 0);
    CHECK(focus.valueText() == "WIDE");
    press(focus);
    CHECK_THAT(getParam(rig, fs.param), WithinAbs(fs.narrowValue, 1e-4));
    CHECK(focus.position() == 1);
    CHECK(focus.valueText() == "NARROW");
    press(focus);
    CHECK_THAT(getParam(rig, fs.param), WithinAbs(fs.wideValue, 1e-4));
    const double past = fs.narrowValue > fs.wideValue ? fs.threshold + 0.01 : fs.threshold - 0.01;
    const double before = fs.narrowValue > fs.wideValue ? fs.threshold - 0.01 : fs.threshold + 0.01;
    setParam(rig, fs.param, past);
    CHECK(focus.position() == 1);
    setParam(rig, fs.param, before);
    CHECK(focus.position() == 0);
  }

  // CIRCUIT: chainsaw -> big fuzz -> chainsaw; the face shows the other set after the loader settles.
  PedalSwitch& circuit = face.circuitSwitch();
  CHECK(circuit.numPositions() == 2);
  CHECK(circuit.position() == 0);
  const auto builds = rig.proc.engineBuilds();
  press(circuit);
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  face.refresh();
  CHECK(rig.proc.engineBuilds() == builds + 1);
  CHECK(circuit.position() == 1);
  REQUIRE(face.activeCircuit().has_value());
  CHECK(*face.activeCircuit() == Circuit::BigFuzz);
  for (int k = 0; k < 6; ++k) {
    CHECK(face.knob(Circuit::BigFuzz, k)->isVisible());
    CHECK_FALSE(face.knob(Circuit::Chainsaw, k)->isVisible());
  }
  CHECK(face.clipSwitch(Circuit::BigFuzz).isVisible());
  CHECK_FALSE(face.clipSwitch(Circuit::Chainsaw).isVisible());
  CHECK(face.focusSwitch(Circuit::BigFuzz).isVisible());
  press(circuit);
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  face.refresh();
  CHECK(circuit.position() == 0);
  CHECK(*face.activeCircuit() == Circuit::Chainsaw);
  CHECK(face.knob(Circuit::Chainsaw, 0)->isVisible());
  CHECK_FALSE(face.knob(Circuit::BigFuzz, 0)->isVisible());
  CHECK(rig.proc.engineBuilds() == builds + 2);
}

TEST_CASE("pedal drawer: closed by default, opens and closes on double-click, x and Escape", "[editor][pedal]") {
  Rig rig;
  PedalFace& face = faceOf(rig);
  AdvancedDrawer& drawer = drawerOf(rig);
  rig.load(kChainsawPresets / "classic_buzzsaw.json");
  face.refresh();
  skin::RigPiece& piece = rigViewOf(rig).piece(skin::Piece::SawPedal);

  CHECK_FALSE(drawer.isOpen());
  CHECK_FALSE(drawer.isVisible());

  doubleClick(piece);
  CHECK(drawer.isOpen());
  CHECK(drawer.isVisible());
  drawer.finishAnimation();
  CHECK(drawer.getBounds() == drawer.openBounds());

  // Geometry: inside the rig, to the right of the pedal, aligned with its vertical span.
  const auto rigBounds = rigViewOf(rig).getBounds();
  const auto pedal = piece.getBounds() + rigViewOf(rig).getPosition();
  CHECK(rigBounds.contains(drawer.getBounds()));
  CHECK_FALSE(drawer.getBounds().intersects(pedal));
  CHECK(drawer.getX() == pedal.getRight() + AdvancedDrawer::kGap);
  CHECK(drawer.getY() == pedal.getY());
  CHECK(drawer.getHeight() == pedal.getHeight());
  CHECK(drawer.getWidth() > 500);
  CHECK(drawer.getParentComponent() == face.getParentComponent());  // a child of the editor content, above the rig

  // Its visible knobs are the active circuit's drawer knobs, with titles and tooltips.
  auto visibleKnobs = [&] {
    std::set<std::string> ids;
    for (auto* k : all<skin::FilmstripKnob>(drawer))
      if (k->isVisible()) ids.insert(k->paramId().toStdString());
    return ids;
  };
  auto expected = [&](Circuit c) {
    std::set<std::string> ids;
    for (const auto& k : circuitFace(c).drawerKnobs) ids.insert(paramSpec(k.param).id.c_str());
    return ids;
  };
  CHECK(visibleKnobs() == expected(Circuit::Chainsaw));
  CHECK(visibleKnobs().size() == 10);
  for (auto* k : all<skin::FilmstripKnob>(drawer)) {
    CHECK(k->getTitle().isNotEmpty());
    CHECK(k->getTooltip().isNotEmpty());
  }
  int visibleSwitches = 0;
  for (auto* sw : all<PedalSwitch>(drawer))
    if (sw->isVisible()) ++visibleSwitches;
  CHECK(visibleSwitches == 2);  // MODE, CLIP 2

  // A drawer knob moves its (live) parameter.
  for (auto* k : all<skin::FilmstripKnob>(drawer))
    if (k->paramId() == "hmPresenceDb") {
      k->setValue(12.0, juce::sendNotificationSync);
      CHECK_THAT(getParam(rig, hmP(kHmPresenceDb)), WithinAbs(12.0, 1e-3));
    }

  // Second double-click closes it.
  doubleClick(piece);
  CHECK_FALSE(drawer.isOpen());
  drawer.finishAnimation();
  CHECK_FALSE(drawer.isVisible());

  // Escape closes it.
  drawer.setOpen(true, false);
  CHECK(drawer.isVisible());
  CHECK(drawer.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
  CHECK_FALSE(drawer.isOpen());
  drawer.finishAnimation();
  CHECK_FALSE(drawer.isVisible());
  CHECK_FALSE(drawer.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));  // nothing to close

  // The x button closes it.
  drawer.setOpen(true, false);
  juce::TextButton* close = nullptr;
  for (auto* b : all<juce::TextButton>(drawer)) close = b;
  REQUIRE(close != nullptr);
  CHECK(close->getTitle().isNotEmpty());
  close->onClick();  // (Button::triggerClick is asynchronous)
  CHECK_FALSE(drawer.isOpen());
  drawer.finishAnimation();
  CHECK_FALSE(drawer.isVisible());

  // The animation is the slide of the spec: opening from the pedal edge, 180 ms.
  CHECK(AdvancedDrawer::kAnimationMs == 180);
  drawer.setOpen(true, true);
  CHECK(drawer.isVisible());
  CHECK(drawer.getX() == drawer.openBounds().getX());
  CHECK(drawer.getWidth() < drawer.openBounds().getWidth());  // starts narrow: it grows out of the pedal side
  drawer.finishAnimation();
  CHECK(drawer.getBounds() == drawer.openBounds());

  // Switching the circuit swaps the drawer's set.
  press(face.circuitSwitch());
  REQUIRE(rig.proc.waitForLoader(std::chrono::milliseconds(60000)));
  face.refresh();
  drawer.refresh();
  REQUIRE(drawer.activeCircuit().has_value());
  CHECK(*drawer.activeCircuit() == Circuit::BigFuzz);
  CHECK(visibleKnobs() == expected(Circuit::BigFuzz));
  CHECK(drawer.title().containsIgnoreCase("big fuzz"));
  visibleSwitches = 0;
  for (auto* sw : all<PedalSwitch>(drawer))
    if (sw->isVisible()) ++visibleSwitches;
  CHECK(visibleSwitches == 1);  // CLIP 2
}

TEST_CASE("pedal face: the OLED shows the preset and the circuit, clip and focus", "[editor][pedal]") {
  Rig rig;
  PedalFace& face = faceOf(rig);
  rig.load(kChainsawPresets / "classic_buzzsaw.json");
  face.refresh();
  CHECK(face.oledLine1() == "CLASSIC BUZZSAW");
  const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
  CHECK(face.oledLine2() == "CHAINSAW" + dot + "SI" + dot + "WIDE");
  setParam(rig, hmP(kHmClip), 1.0);
  CHECK(face.oledLine2() == "CHAINSAW" + dot + "LED" + dot + "WIDE");
  setParam(rig, hmP(kHmLowQ), 1.6);
  CHECK(face.oledLine2() == "CHAINSAW" + dot + "LED" + dot + "NARROW");
  setParam(rig, hmP(kHmClip), 3.0);
  CHECK(face.oledLine2() == "CHAINSAW" + dot + "SOFT" + dot + "NARROW");

  rig.load(kChainsawPresets / "pickle_chainsaw.json");
  face.refresh();
  CHECK(face.oledLine1() == "BIG FUZZ CHAINSAW");
  CHECK(face.oledLine2() == "BIG FUZZ" + dot + "SI" + dot + "WIDE");
  setParam(rig, muffP(kMuffStackRatio), 2.5);
  CHECK(face.oledLine2() == "BIG FUZZ" + dot + "SI" + dot + "NARROW");
  setParam(rig, muffP(kMuffClip), 2.0);
  CHECK(face.oledLine2() == "BIG FUZZ" + dot + "ASYM" + dot + "NARROW");
}

TEST_CASE("pedal face: titles and tooltips on every new control; no trademark in any UI string", "[editor][pedal]") {
  Rig rig;
  rig.load(kChainsawPresets / "classic_buzzsaw.json");
  faceOf(rig).refresh();
  for (auto* sw : all<PedalSwitch>(*rig.ed)) {
    INFO(sw->paramId());
    CHECK(sw->getTitle().isNotEmpty());
    CHECK(sw->getTooltip().isNotEmpty());
    CHECK(sw->getTitle().containsIgnoreCase(sw->controlName()));
    CHECK(sw->getTooltip().containsIgnoreCase(sw->valueText()));
  }
  for (auto* k : all<skin::FilmstripKnob>(*rig.ed)) {
    INFO(k->paramId());
    CHECK(k->getTitle().isNotEmpty());
    CHECK(k->getTooltip().isNotEmpty());
  }
  CHECK(all<PedalSwitch>(*rig.ed).size() == 1 + 2 * kNumCircuits + 3);  // CIRCUIT, face CLIP+FOCUS per circuit, drawer MODE+CLIP2+CLIP2
  CHECK(faceOf(rig).getTitle().isNotEmpty());
  CHECK(drawerOf(rig).getTitle().isNotEmpty());

  // The strings are collected with both circuits shown in turn (the OLED text and the drawer title).
  for (const char* preset : {"classic_buzzsaw.json", "pickle_chainsaw.json"}) {
    rig.load(kChainsawPresets / preset);
    faceOf(rig).refresh();
    drawerOf(rig).refresh();
    auto strings = uiStrings(rig);
    strings.push_back(faceOf(rig).oledLine1());
    strings.push_back(faceOf(rig).oledLine2());
    strings.push_back(drawerOf(rig).title());
    for (const auto& s : strings)
      for (const char* bad : {"boss", "hm-2", "swollen", "pickle", "muff"}) {
        INFO("\"" << s << "\" contains " << bad);
        CHECK_FALSE(s.containsIgnoreCase(bad));
      }
  }
}

TEST_CASE("pedal face and drawer: screenshots", "[editor][pedal][screenshots]") {
  Rig rig;
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  PedalFace& face = faceOf(rig);
  AdvancedDrawer& drawer = drawerOf(rig);
  struct Shot { const char* preset; const char* tag; };
  for (const Shot shot : {Shot{"classic_buzzsaw.json", "chainsaw"}, Shot{"pickle_chainsaw.json", "bigfuzz"}}) {
    INFO(shot.tag);
    rig.load(kChainsawPresets / shot.preset);
    face.refresh();
    drawer.setOpen(false, false);
    REQUIRE(face.isVisible());
    const auto pedal = face.getBounds();

    const juce::Image closed = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 2.0f);
    REQUIRE(closed.getWidth() == 2560);
    savePng(closed, juce::String("sawblade_face_") + shot.tag + "_2x.png");
    // the face changed the pedal area: the controls are drawn (more than the plain render would be)
    CHECK(nonBackgroundFraction(closed, {pedal.getX() * 2, pedal.getY() * 2, pedal.getWidth() * 2, pedal.getHeight() * 2}) > 0.3);
    const juce::Image faceCrop = rig.ed->createComponentSnapshot(pedal.expanded(6), true, 3.0f);
    savePng(faceCrop, juce::String("face_") + shot.tag + "_crop.png");

    drawer.setOpen(true, false);
    drawer.refresh();
    const juce::Image open = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 2.0f);
    savePng(open, juce::String("sawblade_drawer_") + shot.tag + "_2x.png");
    const auto both = pedal.getUnion(drawer.getBounds()).expanded(8);
    savePng(rig.ed->createComponentSnapshot(both, true, 3.0f), juce::String("drawer_") + shot.tag + "_crop.png");
    // opening the drawer changed the picture to the right of the pedal
    int differing = 0;
    const auto d = drawer.getBounds();
    for (int y = d.getY() * 2; y < d.getBottom() * 2; y += 3)
      for (int x = d.getX() * 2; x < d.getRight() * 2; x += 3)
        if (closed.getPixelAt(x, y) != open.getPixelAt(x, y)) ++differing;
    CHECK(differing > 1000);
    drawer.setOpen(false, false);
  }
}
