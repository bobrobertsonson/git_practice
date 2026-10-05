// Tests of the skinned editor (docs/specs/phase2_5_skin.md section 5). Run under a display
// (xvfb-run -a on a headless machine; ctest does this when xvfb-run is installed).

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <cstdlib>
#include <functional>
#include <set>
#include <thread>
#include <vector>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "MatchScreen.h"
#include "PlayAlongPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/CircuitFaces.h"
#include "pedals/PedalFace.h"
#include "pedals/PedalSwitch.h"
#include "rig/EqGraph.h"
#include "rig/RigEditorPanel.h"
#include "rig/RigModel.h"
#include "rig/SlotStrip.h"
#include "skin/FilmstripKnob.h"
#include "skin/FootswitchButton.h"
#include "skin/LedIndicator.h"
#include "skin/RigView.h"
#include "skin/SkinAssets.h"
#include "fake_tools.h"
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
  // build/screenshots by default; SAWBLADE_SCREENSHOT_DIR (environment) puts them anywhere else.
  const char* envDir = std::getenv("SAWBLADE_SCREENSHOT_DIR");
  const juce::File dir(envDir != nullptr && *envDir != '\0' ? envDir : SAWBLADE_SCREENSHOT_DIR);
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

namespace {
// Like collect(), but without descending into the RigEditorPanel: it binds blend, the path levels and the gate
// threshold a second time (spec phase 10, section 1).
void collectOutsideRigPanel(juce::Component& c, std::vector<skin::FilmstripKnob*>& out) {
  for (auto* child : c.getChildren()) {
    if (dynamic_cast<rig::RigEditorPanel*>(child) != nullptr) continue;
    if (auto* k = dynamic_cast<skin::FilmstripKnob*>(child)) out.push_back(k);
    collectOutsideRigPanel(*child, out);
  }
}
}  // namespace

TEST_CASE("every parameter has exactly one bound control outside the rig panel", "[editor]") {
  Rig rig;
  std::vector<skin::FilmstripKnob*> knobs;
  collectOutsideRigPanel(*rig.ed, knobs);
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

// =====================================================================================================
// Rig editor (docs/specs/phase10_rig_editor.md, section 8, tests 8-11)
using namespace sawblade;
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

const fs::path kFx = SAWBLADE_FIXTURES_DIR;

json fxBlock(const std::string& id, const char* model = "linear_identity.nam") {
  return {{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFx / "nam" / model).string()}}}};
}

// Both paths identity (a nam block each), shared impulse cab, post EQ as given, blend 0.5, align off.
json fxPreset(const std::string& name = "Fixture") {
  return {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name},
          {"paths", {{"a", {{"role", "saw"}, {"blocks", json::array({fxBlock("a1")})}}},
                     {"b", {{"role", "body"}, {"blocks", json::array({fxBlock("b1")})}}}}},
          {"align", {{"mode", "off"}}},
          {"blend", 0.5},
          {"cab", {{"mode", "shared"}, {"ir", {{"file", (kFx / "ir" / "impulse.wav").string()}}}}},
          {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}},
                                  {{"type", "highPass"}, {"freq", 80}, {"q", 0.7071}}})}};
}

// A prepared processor with a fixture preset loaded, and its editor.
struct FxRig {
  juce::ScopedJuceInitialiser_GUI gui;
  TempFolder tmp;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  explicit FxRig(const json& preset) {
    proc.prepareToPlay(48000.0, 512);
    load(preset);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~FxRig() { base.reset(); }

  void load(const json& preset) {
    const fs::path f = tmp.dir / "preset.json";
    std::ofstream(f) << preset.dump(2);
    REQUIRE(proc.loadPresetFile(f));
    wait();
  }
  void wait() {
    REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
    REQUIRE(proc.status().error.empty());
  }
  rig::RigEditorPanel& panel() { return ed->rigEditor(); }
  void open(rig::RigEditorPanel::Tab t) {
    ed->setRigEditorOpen(true);
    panel().setTab(t);
    panel().refresh();
  }
  juce::Image snapshot(const char* file) {
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
    panel().refresh();
    const juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
    REQUIRE(img.getWidth() == 1280);
    REQUIRE(img.getHeight() == 800);
    savePng(img, file);
    return img;
  }
};

}  // namespace

TEST_CASE("rig editor: the panel exists, is closed by default, opens from RIG and shows the six tabs", "[editor][rig]") {
  json j = fxPreset();
  j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"slot", "fx"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 3}, {"q", 1}}})}});
  FxRig rig(j);
  auto panels = all<rig::RigEditorPanel>(*rig.ed);
  REQUIRE(panels.size() == 1);
  rig::RigEditorPanel& panel = *panels[0];
  CHECK_FALSE(panel.isVisible());
  CHECK_FALSE(rig.ed->rigEditorOpen());

  juce::Button* toggle = buttonTitled(*rig.ed, "RIG");
  REQUIRE(toggle != nullptr);
  CHECK(toggle->isEnabled());
  CHECK(toggle->getTooltip().isNotEmpty());
  const auto tb = rig.ed->getLocalArea(toggle, toggle->getLocalBounds());
  CHECK(tb.getBottom() <= 58);
  // The panel sits exactly over the rig area.
  const auto pb = rig.ed->getLocalArea(&panel, panel.getLocalBounds());
  CHECK(pb.getX() == 0);
  CHECK(pb.getY() == 58);
  CHECK(pb.getWidth() == 940);
  CHECK(pb.getHeight() == 742);
  click(*toggle);
  CHECK(panel.isVisible());
  CHECK(rig.ed->rigEditorOpen());
  CHECK(toggle->getToggleState());
  CHECK(panel.tab() == rig::RigEditorPanel::Tab::Chain);

  const char* names[] = {"CHAIN", "EQ", "BLEND", "CAB", "GATE", "COMP"};
  for (int i = 0; i < 6; ++i) {
    auto& b = panel.tabButton(static_cast<rig::RigEditorPanel::Tab>(i));
    INFO(names[i]);
    CHECK(b.getButtonText() == names[i]);
    click(b);
    CHECK(panel.tab() == static_cast<rig::RigEditorPanel::Tab>(i));
  }
  for (const char* t : {"SINGLE", "SINGLE + 2 PEDALS", "BLEND"}) {
    bool found = false;
    for (auto* b : all<juce::Button>(panel)) found = found || b->getButtonText() == t;
    CHECK(found);
  }
  click(*toggle);
  CHECK_FALSE(panel.isVisible());

  // Open / closed and the tab are UI state: the saved state is the preset only.
  rig.ed->setRigEditorOpen(true);
  juce::MemoryBlock state;
  rig.proc.getStateInformation(state);
  const std::string text(static_cast<const char*>(state.getData()), state.getSize());
  CHECK(text.find("rigEditor") == std::string::npos);
  CHECK(text.find("RIG") == std::string::npos);
  rig.ed->setRigEditorOpen(false);

  // The top bar still fits: nothing overlaps the new button.
  for (auto* b : all<juce::Button>(*rig.ed)) {
    if (b == toggle || !b->getParentComponent() || b->getParentComponent() != toggle->getParentComponent()) continue;
    if (b->getY() > 58) continue;
    INFO(b->getTitle());
    CHECK_FALSE(b->getBounds().intersects(toggle->getBounds()));
  }

  // Every control inside the panel (all tabs, all cards) has a title and a tooltip.
  int sliders = 0, buttons = 0;
  for (auto* s : all<juce::Slider>(panel)) {
    INFO("slider " << s->getTitle());
    CHECK(s->getTitle().isNotEmpty());
    CHECK(s->getTooltip().isNotEmpty());
    CHECK(s->getTextFromValue(s->getValue()).isNotEmpty());
    ++sliders;
  }
  for (auto* b : all<juce::Button>(panel)) {
    INFO("button " << b->getTitle() << " / " << b->getButtonText());
    CHECK(b->getTitle().isNotEmpty());
    CHECK(b->getTooltip().isNotEmpty());
    ++buttons;
  }
  CHECK(sliders >= 3 + 8 + 6 + 2);  // blend + levels, gate, comp, block inputs
  CHECK(buttons >= 30);
  CHECK(panel.eqGraph().getTitle().isNotEmpty());
  CHECK(panel.eqGraph().getTooltip().isNotEmpty());

  // The panel binds blend, the levels and the gate threshold a second time: those parameters have two knobs now.
  int blendKnobs = 0;
  for (auto* k : all<skin::FilmstripKnob>(*rig.ed))
    if (k->paramId() == "blend") ++blendKnobs;
  CHECK(blendKnobs == 2);
}

TEST_CASE("rig editor: dragging an EQ node edits the band and the parameter", "[editor][rig]") {
  FxRig rig(fxPreset());
  rig.open(rig::RigEditorPanel::Tab::Eq);
  rig.panel().setEqTarget(rig::EqTarget::Post);
  rig.panel().refresh();
  rig::EqGraph& g = rig.panel().eqGraph();
  REQUIRE(g.numBands() == 2);
  const double w = g.getWidth(), h = g.getHeight();
  CHECK(w == rig::EqGraph::kWidth);
  CHECK(h == rig::EqGraph::kHeight);

  // The documented mapping round-trips.
  CHECK_THAT(rig::EqGraph::xToFreq(rig::EqGraph::freqToX(1234.0, w), w), Catch::Matchers::WithinRel(1234.0, 1e-12));
  CHECK_THAT(rig::EqGraph::yToGain(rig::EqGraph::gainToY(-7.5, h), h), WithinAbs(-7.5, 1e-12));
  CHECK_THAT(rig::EqGraph::yToQ(rig::EqGraph::qToY(2.5, h), h), Catch::Matchers::WithinRel(2.5, 1e-12));
  CHECK_THAT(rig::EqGraph::xToFreq(0.0, w), WithinAbs(20.0, 1e-9));
  CHECK_THAT(rig::EqGraph::xToFreq(w, w), WithinAbs(20000.0, 1e-6));
  CHECK_THAT(rig::EqGraph::yToGain(0.0, h), WithinAbs(18.0, 1e-12));
  CHECK_THAT(rig::EqGraph::yToQ(h, h), WithinAbs(0.1, 1e-12));
  CHECK_THAT(rig::EqGraph::yToQ(0.0, h), WithinAbs(20.0, 1e-9));

  const std::uint64_t builds = rig.proc.engineBuilds();
  // node 0: peak 1 kHz / 0 dB. Press on it, drag by (+70, -40), release.
  const juce::Point<float> n0 = g.nodePosition(0);
  const juce::Point<float> to(n0.x + 70.0f, n0.y - 40.0f);
  juce::Component& gc = g;
  gc.mouseDown(mouse(g, n0, n0, false));
  CHECK(g.selected() == 0);
  gc.mouseDrag(mouse(g, to, n0, false));
  gc.mouseUp(mouse(g, to, n0, false));
  const double wantF = rig::EqGraph::xToFreq(to.x, w), wantG = rig::EqGraph::yToGain(to.y, h);
  const Preset p = rig.proc.currentPreset();
  CHECK_THAT(p.postEq[0].freq, Catch::Matchers::WithinRel(wantF, 1e-6));
  CHECK_THAT(p.postEq[0].gainDb, WithinAbs(wantG, 1e-4));
  CHECK_THAT(static_cast<double>(rig.proc.parameters().getRawParameterValue("postEq1")->load()), WithinAbs(wantG, 1e-4));
  CHECK(rig.proc.engineBuilds() == builds);  // a live edit: no rebuild
  EqBand shown = p.postEq[0];
  CHECK(rig.panel().eqReadoutText() == rig::EqGraph::bandReadout(shown));
  CHECK(rig.panel().eqReadoutText().startsWith("PEAK"));
  CHECK(rig.panel().eqReadoutText().contains("kHz"));
  rig.panel().refresh();
  CHECK_THAT(static_cast<double>(g.band(0).freq), Catch::Matchers::WithinRel(wantF, 1e-6));

  // node 1: high-pass 80 Hz, Q 0.7071. A vertical drag moves the Q and leaves the frequency alone.
  const juce::Point<float> n1 = g.nodePosition(1);
  const juce::Point<float> to1(n1.x, n1.y - 50.0f);
  gc.mouseDown(mouse(g, n1, n1, false));
  CHECK(g.selected() == 1);
  gc.mouseDrag(mouse(g, to1, n1, false));
  gc.mouseUp(mouse(g, to1, n1, false));
  const Preset p2 = rig.proc.currentPreset();
  CHECK_THAT(p2.postEq[1].q, Catch::Matchers::WithinRel(rig::EqGraph::yToQ(to1.y, h), 1e-6));
  CHECK(p2.postEq[1].freq == 80.0);
  CHECK(p2.postEq[1].gainDb == 0.0);
  CHECK(rig.panel().eqReadoutText().startsWith("HIGH-PASS"));
  CHECK(rig.proc.engineBuilds() == builds);

  // Shift-drag moves the Q only; the wheel too.
  const juce::Point<float> n0b = g.nodePosition(0);
  gc.mouseDown(mouse(g, n0b, n0b, false));
  gc.mouseDrag(mouse(g, {n0b.x + 30.0f, n0b.y - 30.0f}, n0b, true));
  gc.mouseUp(mouse(g, {n0b.x + 30.0f, n0b.y - 30.0f}, n0b, true));
  const Preset p3 = rig.proc.currentPreset();
  CHECK(p3.postEq[0].freq == p.postEq[0].freq);
  CHECK(p3.postEq[0].q > p.postEq[0].q);
  juce::MouseWheelDetails wheel;
  wheel.deltaY = -1.0f;
  const double qBefore = p3.postEq[0].q;
  gc.mouseWheelMove(mouse(g, g.nodePosition(0), g.nodePosition(0), false), wheel);
  CHECK(rig.proc.currentPreset().postEq[0].q < qBefore);

  // Structural edits: double-click a node toggles it, double-click empty space adds a peak band there.
  const auto n = g.nodePosition(0);
  gc.mouseDoubleClick(mouse(g, n, n, false, 2));
  rig.wait();
  CHECK_FALSE(rig.proc.currentPreset().postEq[0].enabled);
  const juce::Point<float> empty(200.0f, 60.0f);
  gc.mouseDoubleClick(mouse(g, empty, empty, false, 2));
  rig.wait();
  const Preset p4 = rig.proc.currentPreset();
  REQUIRE(p4.postEq.size() == 3);
  CHECK(p4.postEq[2].type == EqType::Peak);
  CHECK_THAT(p4.postEq[2].freq, Catch::Matchers::WithinRel(rig::EqGraph::xToFreq(200.0, w), 1e-9));
  CHECK_THAT(p4.postEq[2].gainDb, WithinAbs(rig::EqGraph::yToGain(60.0, h), 1e-4));  // a gain band: the parameter's 1e-4 grid
}

TEST_CASE("rig editor: topology and cab buttons change the preset through the loader", "[editor][rig]") {
  FxRig rig(fxPreset());
  rig.open(rig::RigEditorPanel::Tab::Chain);
  auto& panel = rig.panel();
  CHECK(rig::topologyOf(rig.proc.currentPreset()) == rig::Topology::Blend);

  click(panel.topologyButton(rig::Topology::Single));
  rig.wait();
  CHECK(rig::topologyOf(rig.proc.currentPreset()) == rig::Topology::Single);
  CHECK_FALSE(rig.proc.currentPreset().b.enabled);
  CHECK(rig.proc.currentPreset().blend == 0.0);
  panel.refresh();
  CHECK(anyLabelContains(*rig.ed, "BLEND OFF"));

  click(panel.topologyButton(rig::Topology::Blend));
  rig.wait();
  panel.refresh();
  CHECK(rig::topologyOf(rig.proc.currentPreset()) == rig::Topology::Blend);
  CHECK(rig.proc.currentPreset().blend == 0.5);
  CHECK_FALSE(anyLabelContains(*rig.ed, "BLEND OFF"));

  panel.setTab(rig::RigEditorPanel::Tab::Cab);
  CHECK(anyLabelContains(*rig.ed, "LIVE-COMPATIBLE"));
  CHECK_FALSE(anyLabelContains(*rig.ed, "STUDIO BLEND"));
  click(panel.cabModeButton(CabMode::PerPath));
  rig.wait();
  panel.refresh();
  CHECK(rig.proc.currentPreset().cab.mode == CabMode::PerPath);
  CHECK(anyLabelContains(*rig.ed, "STUDIO BLEND: only the with-cab NAM export is exact"));
  CHECK_FALSE(anyLabelContains(*rig.ed, "LIVE-COMPATIBLE"));
  click(panel.cabModeButton(CabMode::Shared));
  rig.wait();
  panel.refresh();
  CHECK(rig.proc.currentPreset().cab.mode == CabMode::Shared);
  CHECK(anyLabelContains(*rig.ed, "LIVE-COMPATIBLE: the no-cab NAM export is exact"));
}

TEST_CASE("rig editor: chain cards edit the blocks", "[editor][rig]") {
  json j = fxPreset();
  j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"slot", "fx"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 3}, {"q", 1}}})}});
  FxRig rig(j);
  rig.open(rig::RigEditorPanel::Tab::Chain);
  auto byTitle = [&](const juce::String& t) -> juce::Button* {
    for (auto* b : all<juce::Button>(rig.panel()))
      if (b->getTitle() == t) return b;
    return nullptr;
  };
  REQUIRE(byTitle("Bypass a2") != nullptr);
  click(*byTitle("Bypass a2"));
  rig.wait();
  CHECK(rig.proc.currentPreset().a.blocks[1].bypass);
  rig.panel().refresh();
  click(*byTitle("Move a2 left"));
  rig.wait();
  CHECK(rig.proc.currentPreset().a.blocks[0].id == "a2");
  rig.panel().refresh();
  click(*byTitle("Remove a2"));
  rig.wait();
  CHECK(rig.proc.currentPreset().a.blocks.size() == 1);
  rig.panel().refresh();
  CHECK(byTitle("Remove a2") == nullptr);

  // INPUT knob: live, no rebuild.
  const std::uint64_t builds = rig.proc.engineBuilds();
  auto* in = sliderTitled(rig.panel(), "Input gain a1");
  REQUIRE(in != nullptr);
  in->setValue(6.0, juce::sendNotificationSync);
  const Preset p = rig.proc.currentPreset();
  CHECK(static_cast<const NamBlockParams&>(*p.a.blocks[0].params).inputGainDb == 6.0);
  CHECK(rig.proc.engineBuilds() == builds);

  // ADD: an eq block is appended in front of the amp.
  rig.panel().refresh();
  click(rig.panel().tabButton(rig::RigEditorPanel::Tab::Chain));
  juce::Component* lane = nullptr;
  for (auto* b : all<juce::Button>(rig.panel()))
    if (b->getTitle().startsWith("Add block to A")) lane = b->getParentComponent();
  REQUIRE(lane != nullptr);
  dynamic_cast<rig::SlotStrip*>(lane)->addType("eq");
  rig.wait();
  const Preset p2 = rig.proc.currentPreset();
  REQUIRE(p2.a.blocks.size() == 2);
  CHECK(p2.a.blocks[0].type == "eq");
  CHECK(p2.a.blocks[1].type == "nam");
  CHECK(p2.a.blocks[0].id == "a2");
  // A failing block type reports an error and changes nothing.
  const std::uint64_t b2 = rig.proc.engineBuilds();
  std::string seen;
  dynamic_cast<rig::SlotStrip*>(lane)->onMessage = [&](const juce::String& m) { seen = m.toStdString(); };
  dynamic_cast<rig::SlotStrip*>(lane)->addType("no.such.type");
  CHECK_FALSE(seen.empty());
  CHECK(rig.proc.engineBuilds() == b2);
}

TEST_CASE("rig editor: screenshots", "[editor][rig]") {
  // Blend: both lanes populated from the identity preset plus an eq block.
  {
    json j = fxPreset();
    j["paths"]["a"]["blocks"].push_back({{"id", "a2"}, {"type", "eq"}, {"slot", "fx"}, {"bands", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 3}, {"q", 1}}})}});
    j["paths"]["b"]["blocks"][0]["slot"] = "amp";
    FxRig rig(j);
    rig.open(rig::RigEditorPanel::Tab::Chain);
    const juce::Image img = rig.snapshot("rig_blend.png");
    CHECK(nonBackgroundFraction(img, {0, 58, 940, 742}) > 0.04);
    CHECK(anyLabelContains(*rig.ed, juce::String::fromUTF8("A \xC2\xB7 SAW")));
    CHECK(anyLabelContains(*rig.ed, juce::String::fromUTF8("B \xC2\xB7 BODY")));
    CHECK(anyLabelContains(*rig.ed, "EQ 1 band"));
    CHECK(rig::topologyOf(rig.proc.currentPreset()) == rig::Topology::Blend);

    // Single: lane B shows the BLEND OFF note.
    click(rig.panel().topologyButton(rig::Topology::Single));
    rig.wait();
    // let the async parameter attachments land: queued messages run before the stop request
    juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    juce::MessageManager::getInstance()->runDispatchLoop();
    for (auto* k : all<skin::FilmstripKnob>(*rig.ed))
      if (k->paramId() == "blend") CHECK(k->getValue() == 0.0);
    const juce::Image single = rig.snapshot("rig_single.png");
    CHECK(nonBackgroundFraction(single, {0, 58, 940, 742}) > 0.04);
    CHECK(anyLabelContains(*rig.ed, "BLEND OFF"));
  }
  // EQ: three bands of different types on the post EQ.
  {
    json j = fxPreset();
    j["postEq"] = json::array({{{"type", "peak"}, {"freq", 800}, {"gainDb", 5.0}, {"q", 1.4}},
                               {{"type", "highShelf"}, {"freq", 6000}, {"gainDb", -6.0}, {"q", 0.7}},
                               {{"type", "highPass"}, {"freq", 90}, {"q", 0.9}}});
    FxRig rig(j);
    rig.open(rig::RigEditorPanel::Tab::Eq);
    const juce::Image img = rig.snapshot("rig_eq.png");
    CHECK(rig.panel().eqGraph().numBands() == 3);
    CHECK(nonBackgroundFraction(img, {0, 58, 940, 742}) > 0.05);
  }
  // Gate in expander mode.
  {
    json j = fxPreset();
    j["gate"] = {{"enabled", true}, {"mode", "expander"}, {"thresholdDb", -52.0}, {"hysteresisDb", 5.0}, {"attackMs", 0.4},
                 {"holdMs", 25.0}, {"releaseMs", 90.0}, {"rangeDb", -60.0}, {"ratio", 3.0}, {"keyHighPassHz", 120.0},
                 {"releaseCurve", "linear-db"}};
    FxRig rig(j);
    rig.open(rig::RigEditorPanel::Tab::Gate);
    const juce::Image img = rig.snapshot("rig_gate.png");
    CHECK(nonBackgroundFraction(img, {0, 58, 940, 742}) > 0.05);
    CHECK(anyLabelContains(*rig.ed, "120 Hz"));
    // The remaining tabs, for the reviewer's eyes (not in the spec's list).
    rig.panel().setTab(rig::RigEditorPanel::Tab::Blend);
    rig.snapshot("rig_tab_blend.png");
    rig.panel().setTab(rig::RigEditorPanel::Tab::Cab);
    rig.snapshot("rig_tab_cab.png");
    rig.panel().setTab(rig::RigEditorPanel::Tab::Comp);
    rig.snapshot("rig_tab_comp.png");
  }
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

// ---- record + match (docs/specs/phase6a_record_match_plugin.md) -------------------------------------------------
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Anything that falls back to the default data folder (settings, takes, jobs) stays inside a temp dir.
[[maybe_unused]] const bool kEditorDataDirSet = [] {
  const fs::path d = fs::temp_directory_path() / ("sawblade_editor_tests_data_" + std::to_string(::getpid()));
  ::setenv("SAWBLADE_DATA_DIR", d.c_str(), 1);
  return true;
}();

bool waitUntilTrue(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 15000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

// Feeds `seconds` of audio (silence in, like a quiet DI) at roughly 8x real time: the writer thread gets time to
// drain the ring, as it would from a live host.
void feedSeconds(SawbladeProcessor& p, double seconds) {
  const int blocks = static_cast<int>(seconds * 48000.0 / 512.0);
  for (int done = 0; done < blocks; done += 40) {
    processBlocks(p, std::min(40, blocks - done));
    std::this_thread::sleep_for(25ms);
  }
}

struct MatchRig : Rig {
  TempFolder tmp;
  fake_tools::Toolbox tools{tmp.dir / "tools"};
  MatchRig() {
    proc.recorder().setTakesDir(tmp.dir / "takes");
    proc.jobs().setJobsDir(tools.jobs);
    proc.matchSettings().setFile(tools.root / "settings.xml");  // the toolbox saved its fake tool paths there
  }
  MatchScreen& screen() { return *all<MatchScreen>(*ed).at(0); }
  PlayAlongPanel& panel() { return *all<PlayAlongPanel>(*ed).at(0); }
  juce::Button* screenButton(const juce::String& title) { return buttonTitled(screen(), title); }
};

juce::Image shot(SawbladeEditor& ed) {
  ed.setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  return ed.createComponentSnapshot(ed.getLocalBounds(), true, 1.0f);
}

}  // namespace

TEST_CASE("record: REC, the take list and USE FOR MATCH are bound to the recorder", "[editor][record]") {
  MatchRig rig;
  rig.ed->setPlayAlongOpen(true);
  PlayAlongPanel& panel = rig.panel();
  for (const char* title : {"REC", "RENAME", "DELETE", "USE FOR MATCH", "MATCH", "EXPORT NAM"}) {
    INFO(title);
    auto* b = buttonTitled(panel, title);
    REQUIRE(b != nullptr);
    CHECK(b->getTooltip().isNotEmpty());
  }
  // The panel grew by the record band; it still sits on the bottom edge of the design.
  const auto pb = rig.ed->getLocalArea(&panel, panel.getLocalBounds());
  CHECK(pb.getBottom() == SawbladeEditor::kDesignHeight);
  CHECK(pb.getHeight() == PlayAlongPanel::kHeight);
  CHECK(pb.getY() > 400);

  auto lists = all<juce::ListBox>(panel);
  REQUIRE(lists.size() == 1);
  juce::ListBox& list = *lists[0];
  CHECK(list.getListBoxModel()->getNumRows() == 0);
  CHECK(anyLabelContains(panel, "No takes yet"));
  CHECK_FALSE(buttonTitled(panel, "RENAME")->isEnabled());
  CHECK_FALSE(buttonTitled(panel, "DELETE")->isEnabled());

  auto& rec = rig.proc.recorder();
  juce::Button* recBtn = buttonTitled(panel, "REC");
  click(*recBtn);
  CHECK(rec.state() == TakeRecorder::State::Armed);
  processBlocks(rig.proc, 6);
  panel.refresh();
  CHECK(rec.state() == TakeRecorder::State::Recording);
  CHECK(recBtn->getButtonText() == "STOP");
  CHECK(anyLabelContains(panel, "REC 00:"));
  click(*recBtn);  // stop
  processBlocks(rig.proc, 2);
  REQUIRE(rec.waitIdle());
  panel.refresh();
  CHECK(recBtn->getButtonText() == "REC");
  REQUIRE(list.getListBoxModel()->getNumRows() == 1);
  CHECK_FALSE(anyLabelContains(panel, "No takes yet"));
  CHECK(buttonTitled(panel, "RENAME")->isEnabled());
  CHECK(buttonTitled(panel, "DELETE")->isEnabled());
  CHECK(list.getSelectedRow() == 0);

  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(rig.proc.matchSettings().selectedTake().empty());
  click(*buttonTitled(panel, "USE FOR MATCH"));
  CHECK(rig.proc.matchSettings().selectedTake() == takes[0].name);

  // A take deleted behind the panel's back disappears from the list.
  REQUIRE(rec.removeTake(takes[0].name));
  panel.refresh();
  CHECK(list.getListBoxModel()->getNumRows() == 0);
}

TEST_CASE("record: an overrun shows in the panel", "[editor][record]") {
  MatchRig rig;
  rig.ed->setPlayAlongOpen(true);
  auto& rec = rig.proc.recorder();
  rec.setWriterStalledForTest(true);
  REQUIRE(rec.start(""));
  processBlocks(rig.proc, static_cast<int>(rec.ringCapacity() / 512) + 8);
  rig.panel().refresh();
  CHECK(anyLabelContains(rig.panel(), "8 overruns"));
  rec.setWriterStalledForTest(false);
  rec.stop();
  processBlocks(rig.proc, 1);
  REQUIRE(rec.waitIdle());
  rig.panel().refresh();
  const auto takes = rec.listTakes();
  REQUIRE(takes.size() == 1);
  CHECK(takes[0].overruns == 8);
}

TEST_CASE("record/match: in plugin mode MATCH and EXPORT NAM say to open the Standalone app", "[editor][record][match]") {
  MatchRig rig;
  rig.ed->setPlayAlongOpen(true);
  REQUIRE_FALSE(rig.proc.matchEnabled());
  auto* match = buttonTitled(rig.panel(), "MATCH");
  auto* exportBtn = buttonTitled(rig.panel(), "EXPORT NAM");
  click(*match);
  rig.panel().refresh();
  CHECK_FALSE(rig.ed->matchScreenOpen());
  CHECK(anyLabelContains(rig.panel(), "Standalone app"));
  click(*exportBtn);
  rig.panel().refresh();
  CHECK_FALSE(rig.ed->matchScreenOpen());
  CHECK(anyLabelContains(rig.panel(), "Standalone app"));
  CHECK(anyLabelContains(rig.panel(), "EXPORT NAM runs"));
  // Recording still works in plugin mode.
  click(*buttonTitled(rig.panel(), "REC"));
  CHECK(rig.proc.recorder().state() == TakeRecorder::State::Armed);
  click(*buttonTitled(rig.panel(), "REC"));  // the same button, now labelled STOP
  CHECK(rig.proc.recorder().state() == TakeRecorder::State::Idle);

  // Standalone: the buttons open the screen.
  rig.proc.playAlong().setStandalone(true);
  click(*match);
  CHECK(rig.ed->matchScreenOpen());
  CHECK(rig.screen().mode() == MatchScreen::Mode::Match);
  click(*rig.screenButton(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  CHECK_FALSE(rig.ed->matchScreenOpen());
  click(*exportBtn);
  CHECK(rig.ed->matchScreenOpen());
  CHECK(rig.screen().mode() == MatchScreen::Mode::Export);
}

TEST_CASE("match screen: a missing executable or pool shows a clear message and a Locate button", "[editor][match]") {
  MatchRig rig;
  rig.proc.playAlong().setStandalone(true);
  rig.ed->openMatchScreen(false);
  MatchScreen& screen = rig.screen();
  auto* start = rig.screenButton("START MATCH");
  REQUIRE(start != nullptr);
  CHECK_FALSE(start->isEnabled());  // no song, no take yet
  CHECK(anyLabelContains(screen, "Load a song"));

  rig.proc.matchSettings().setMatchExecutable(rig.tmp.dir / "nowhere" / "sawblade-match");
  screen.refresh();
  CHECK(anyLabelContains(screen, "sawblade-match was not found"));
  CHECK(anyLabelContains(screen, "Locate"));
  int locate = 0;
  for (auto* b : all<juce::Button>(screen))
    if (b->getTitle() == "LOCATE..." && b->isVisible()) ++locate;
  CHECK(locate == 2);  // executable and pool

  rig.proc.matchSettings().setMatchExecutable(rig.tools.match);
  rig.proc.matchSettings().setPoolManifest(rig.tmp.dir / "nowhere" / "pool_manifest.json");
  screen.refresh();
  CHECK(anyLabelContains(screen, "pool manifest was not found"));
  CHECK_FALSE(start->isEnabled());
}

TEST_CASE("match screen: a job survives closing the editor and the reopened screen shows it", "[editor][match]") {
  MatchRig rig;
  auto& pa = rig.proc.playAlong();
  pa.setStandalone(true);
  const auto song = writeSyntheticSong(rig.tmp.dir, "Song (stems)", 20.0);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  auto& rec = rig.proc.recorder();
  REQUIRE(rec.start(song.string()));
  feedSeconds(rig.proc, 1.0);
  rec.stop();
  processBlocks(rig.proc, 1);
  REQUIRE(rec.waitIdle());
  rig.proc.matchSettings().setSelectedTake(rec.listTakes().at(0).name);

  rig.tools.cfgMatch({{"progressJson", true}, {"gates", nlohmann::json::array({"g2"})}});
  rig.ed->openMatchScreen(false);
  auto* start = rig.screenButton("START MATCH");
  REQUIRE(start != nullptr);
  REQUIRE(start->isEnabled());
  click(*start);
  REQUIRE(waitUntilTrue([&] { return rig.proc.jobs().snapshot(JobKind::Match).progress.message == "refining 1/3"; }));

  rig.proc.editorBeingDeleted(rig.base.get());  // what a plugin wrapper does when it closes the editor window
  rig.base.reset();  // the editor goes away; the processor (and its job) stays
  rig.ed = nullptr;
  CHECK(rig.proc.jobs().snapshot(JobKind::Match).state == JobState::Running);
  CHECK(rig.proc.getActiveEditor() == nullptr);
  rig.base.reset(rig.proc.createEditorAndMakeActive());
  rig.ed = dynamic_cast<SawbladeEditor*>(rig.base.get());
  REQUIRE(rig.ed != nullptr);
  rig.ed->openMatchScreen(false);
  CHECK(anyLabelContains(rig.screen(), "stage 2: fine-tuning"));
  CHECK(anyLabelContains(rig.screen(), "refining 1/3"));
  CHECK(rig.screenButton("CANCEL")->isEnabled());
  CHECK_FALSE(rig.screenButton("START MATCH")->isEnabled());
  click(*rig.screenButton("CANCEL"));
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Match, 10000ms));
  rig.screen().refresh();
  CHECK(anyLabelContains(rig.screen(), "Cancelled"));
}

TEST_CASE("record + match: screenshots of REC armed, the match progress and the result list", "[editor][record][match][screenshot]") {
  MatchRig rig;
  auto& proc = rig.proc;
  auto& pa = proc.playAlong();
  pa.setStandalone(true);
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);

  // A song, a few takes (the first two with the backing running, so they carry a position in the song), then a take in progress.
  const auto song = writeSyntheticSong(rig.tmp.dir, "Gatecreeper - Dark Superstition (stems)", 90.0);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  pa.setLevelDb(-6.0);
  processBlocks(proc, 4);
  auto& rec = proc.recorder();
  auto takeOf = [&](double seconds, bool playing, double seekSeconds, const char* name) {
    if (playing) {
      pa.seekSamples(static_cast<std::int64_t>(seekSeconds * 48000.0));
      pa.play();
    } else {
      pa.pause();
    }
    processBlocks(proc, 4);
    REQUIRE(rec.start(song.string()));
    feedSeconds(proc, seconds);
    rec.stop();
    processBlocks(proc, 1);
    REQUIRE(rec.waitIdle());
    pa.pause();
    processBlocks(proc, 2);
    std::string err;
    REQUIRE(rec.renameTake(rec.currentTakeName(), name, &err));
  };
  takeOf(3.0, true, 31.4, "verse riff");
  takeOf(2.0, true, 58.0, "breakdown");
  takeOf(1.5, false, 0.0, "warm-up (no song)");
  rig.proc.matchSettings().setSelectedTake("verse riff");

  pa.seekSamples(12 * 48000);
  pa.play();
  processBlocks(proc, 4);
  REQUIRE(rec.start(song.string()));
  feedSeconds(proc, 5.5);
  rig.ed->setPlayAlongOpen(true);
  rig.panel().refresh();
  REQUIRE(rec.state() == TakeRecorder::State::Recording);
  CHECK(anyLabelContains(rig.panel(), "REC 00:05"));
  const juce::Image armed = shot(*rig.ed);
  savePng(armed, "sawblade_record_armed_1x.png");
  const juce::Rectangle<int> band(0, 800 - PlayAlongPanel::kRecordHeight, 1280, PlayAlongPanel::kRecordHeight);
  CHECK(nonBackgroundFraction(armed, band) > 0.1);
  auto* list = all<juce::ListBox>(rig.panel()).at(0);
  CHECK(list->getListBoxModel()->getNumRows() == 3);
  rec.stop();
  processBlocks(proc, 1);
  REQUIRE(rec.waitIdle());
  rig.panel().refresh();
  CHECK(list->getListBoxModel()->getNumRows() == 4);

  // MATCH: a fake matcher parked at stage 2.
  rig.tools.cfgMatch({{"progressJson", true}, {"gates", nlohmann::json::array({"g2"})}});
  click(*buttonTitled(rig.panel(), "MATCH"));
  REQUIRE(rig.ed->matchScreenOpen());
  MatchScreen& screen = rig.screen();
  CHECK(anyLabelContains(screen, "'other' stem"));
  CHECK(anyLabelContains(screen, "verse riff"));
  CHECK(anyLabelContains(screen, "into the song"));
  auto* start = rig.screenButton("START MATCH");
  REQUIRE(start != nullptr);
  REQUIRE(start->isEnabled());
  click(*start);
  REQUIRE(waitUntilTrue([&] { return rig.proc.jobs().snapshot(JobKind::Match).progress.message == "refining 1/3"; }));
  std::this_thread::sleep_for(1200ms);  // some elapsed time to show
  screen.refresh();
  CHECK(anyLabelContains(screen, "stage 2: fine-tuning"));
  CHECK(anyLabelContains(screen, "refining 1/3"));
  CHECK(anyLabelContains(screen, "best error 4.20 dB"));
  CHECK(anyLabelContains(screen, "ETA 01:00"));
  const juce::Image progress = shot(*rig.ed);
  savePng(progress, "sawblade_match_progress_1x.png");
  CHECK(nonBackgroundFraction(progress, {0, 100, 1280, 700}) > 0.03);

  // Results: the job finishes; audition the second candidate (A/B state shows).
  fake_tools::release(rig.proc.jobs().snapshot(JobKind::Match).dir, "g2");
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Match));
  screen.refresh();
  auto* results = all<juce::ListBox>(screen).at(0);
  REQUIRE(results->getListBoxModel()->getNumRows() == 3);
  CHECK(anyLabelContains(screen, "Done"));
  results->selectRow(1);
  click(*rig.screenButton("AUDITION"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "match alt 1");
  CHECK(anyLabelContains(screen, "now playing B"));
  REQUIRE(rig.screenButton("A / B") != nullptr);
  CHECK(rig.screenButton("A / B")->getButtonText() == "A / B  (B)");
  click(*rig.screenButton("A / B"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "Init");
  CHECK(anyLabelContains(screen, "now playing A"));
  CHECK(rig.screenButton("A / B")->getButtonText() == "A / B  (A)");
  click(*rig.screenButton("A / B"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  const juce::Image resultsShot = shot(*rig.ed);
  savePng(resultsShot, "sawblade_match_results_1x.png");
  CHECK(nonBackgroundFraction(resultsShot, {460, 230, 800, 350}) > 0.1);
  click(*rig.screenButton("APPLY"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "match alt 1");
  CHECK_FALSE(rig.proc.audition().state().active);
  CHECK(anyLabelContains(screen, "Applied"));

  // EXPORT NAM on the applied preset: mode / size choice, device auto, progress from the checkpoint, Reveal.
  click(*rig.screenButton(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  rig.tools.cfgExport({{"gates", nlohmann::json::array({"g2"})}});
  click(*buttonTitled(rig.panel(), "EXPORT NAM"));
  REQUIRE(rig.ed->matchScreenOpen());
  CHECK(rig.screen().mode() == MatchScreen::Mode::Export);
  screen.refresh();
  CHECK(anyLabelContains(screen, "match_alt") == false);
  CHECK(anyLabelContains(screen, "Matched preset:"));
  click(*rig.screenButton("WITH CAB"));
  click(*rig.screenButton("LITE"));
  click(*rig.screenButton("TRAIN EXPORT"));
  REQUIRE(waitUntilTrue([&] { return rig.proc.jobs().snapshot(JobKind::Export).progress.fraction > 0.29; }));
  screen.refresh();
  CHECK(anyLabelContains(screen, "training"));
  CHECK(anyLabelContains(screen, "epoch 7"));
  const auto exportSnap = rig.proc.jobs().snapshot(JobKind::Export);
  CHECK(exportSnap.exportMode == "withcab");
  CHECK(exportSnap.exportSize == "lite");
  savePng(shot(*rig.ed), "sawblade_export_progress_1x.png");
  fs::path revealed;
  screen.reveal = [&](const juce::File& f) { revealed = fs::path(f.getFullPathName().toStdString()); };
  CHECK_FALSE(rig.screenButton("REVEAL")->isEnabled());
  fake_tools::release(exportSnap.outDir, "g2");
  REQUIRE(rig.proc.jobs().waitFinished(JobKind::Export));
  screen.refresh();
  CHECK(anyLabelContains(screen, "Model written to"));
  REQUIRE(rig.screenButton("REVEAL")->isEnabled());
  click(*rig.screenButton("REVEAL"));
  CHECK(revealed == exportSnap.outDir);
}
