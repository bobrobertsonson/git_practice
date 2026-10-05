// Tests of the skinned editor (docs/specs/phase2_5_skin.md section 5). Run under a display
// (xvfb-run -a on a headless machine; ctest does this when xvfb-run is installed).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "BuildInfo.h"
#include "PlayAlongPanel.h"
#include "about/AboutBox.h"
#include "settings/Settings.h"
#include "settings/SettingsPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "skin/FilmstripKnob.h"
#include "skin/FootswitchButton.h"
#include "skin/LedIndicator.h"
#include "skin/RigView.h"
#include "skin/SkinAssets.h"
#include "sawblade/wav_io.h"

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

// Each editor test points the settings at its own temp file (the editor reads Settings::shared() when it is
// constructed) and forgets the once-per-process first-run flag. `json == nullptr`: no settings file, so
// this is a first run.
struct SettingsEnv {
  std::filesystem::path dir;
  explicit SettingsEnv(const char* json) {
    dir = std::filesystem::temp_directory_path() / ("sawblade_editor_settings_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffff));
    std::filesystem::create_directories(dir);
    ::setenv("SAWBLADE_SETTINGS_FILE", (dir / "settings.json").c_str(), 1);
    ::setenv("SAWBLADE_T3K_TOKEN_FILE", (dir / "tokens.json").c_str(), 1);
    ::setenv("SAWBLADE_CACHE_DIR", (dir / "cache").c_str(), 1);
    ::unsetenv("SAWBLADE_MATCH_VENV");
    ::unsetenv("TONE3000_CLIENT_ID");
    if (json != nullptr) std::ofstream(dir / "settings.json") << json;
    sawblade::plugin::settings::Settings::resetSharedForTests();
    sawblade::plugin::settings::SettingsPanel::resetFirstRunShownForTests();
  }
  ~SettingsEnv() {
    sawblade::plugin::settings::Settings::resetSharedForTests();
    for (const char* v : {"SAWBLADE_SETTINGS_FILE", "SAWBLADE_T3K_TOKEN_FILE", "SAWBLADE_CACHE_DIR"}) ::unsetenv(v);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};
constexpr const char* kSettingsExist = "{\"version\": 1, \"firstRunCompleted\": true}";

// A processor with its editor. The barbaric preset is loaded only if all its captures exist (they are
// not committed); otherwise the processor stays on Init. Either way the tests never fail on it.
struct Rig {
  SettingsEnv env;
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  explicit Rig(const char* settingsJson = kSettingsExist) : env(settingsJson) {
    const auto preset = std::filesystem::path(SAWBLADE_PRESETS_DIR) / "matched" / "barbaric_v4.json";
    if (std::filesystem::exists(preset) && presetCapturesPresent(preset)) proc.loadPresetFile(preset);
    proc.waitForLoader(std::chrono::milliseconds(60000));
    proc.prepareToPlay(48000.0, 512);
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
  }
  ~Rig() { base.reset(); }
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

TEST_CASE("every parameter has exactly one knob bound to it", "[editor]") {
  Rig rig;
  auto knobs = all<skin::FilmstripKnob>(*rig.ed);
  REQUIRE(static_cast<int>(knobs.size()) == kNumParams);
  std::set<std::string> ids;
  for (auto* k : knobs) ids.insert(k->paramId().toStdString());
  CHECK(static_cast<int>(ids.size()) == kNumParams);

  for (int i = 0; i < kNumParams; ++i) {
    const ParamSpec& s = paramSpec(i);
    INFO(s.id);
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
  CHECK_FALSE(anyLabelContains(*all<PlayAlongPanel>(*rig.ed).at(0), "not found"));  // the (hidden) Settings panel has its own "not found" texts
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

// =============================================================================================
// Phase 11: Settings panel, first run, login view, checklist, About box, icon (spec section 9, tests 10-17)
// =============================================================================================
namespace {
using sawblade::plugin::settings::Settings;
using sawblade::plugin::settings::SettingsPanel;
namespace fs = std::filesystem;

template <class Pred>
bool pumpUntil(Pred pred, int timeoutMs) {
  const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (!pred()) {
    if (juce::Time::getMillisecondCounter() > end) return false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
  }
  return true;
}

SettingsPanel& settingsPanelOf(SawbladeEditor& ed) {
  auto v = all<SettingsPanel>(ed);
  REQUIRE(v.size() == 1);
  return *v[0];
}

juce::TextEditor* fieldTitled(juce::Component& root, const juce::String& title) {
  for (auto* e : all<juce::TextEditor>(root))
    if (e->getTitle() == title) return e;
  return nullptr;
}

// <tmp>/venv/bin/{sawblade-t3k, sawblade-match}: copies of the committed fake scripts.
fs::path makeFakeVenv(const fs::path& root, bool match = true) {
  const fs::path venv = root / "venv";
  fs::create_directories(venv / "bin");
  fs::copy_file(fs::path(SAWBLADE_TEST_TOOLS_DIR) / "fake_t3k.sh", venv / "bin" / "sawblade-t3k", fs::copy_options::overwrite_existing);
  if (match) fs::copy_file(fs::path(SAWBLADE_TEST_TOOLS_DIR) / "fake_match.sh", venv / "bin" / "sawblade-match", fs::copy_options::overwrite_existing);
  return venv;
}
std::string settingsJsonWithVenv(const fs::path& venv) {
  nlohmann::json j{{"version", 1}, {"firstRunCompleted", true}, {"matchVenvDir", venv.string()}};
  return j.dump();
}

sawblade::Capture makeCapture(const fs::path& dir, const std::string& name, bool onDisk, std::optional<sawblade::CaptureSource> src) {
  sawblade::Capture c;
  c.file = name;
  c.resolvedPath = dir / name;
  if (onDisk) std::ofstream(c.resolvedPath) << "x";
  c.source = std::move(src);
  return c;
}
sawblade::Block namBlock(const std::string& id, const std::string& slot, sawblade::Capture model) {
  sawblade::Block b;
  b.id = id;
  b.type = "nam";
  b.slot = slot;
  auto p = std::make_shared<sawblade::NamBlockParams>();
  p->model = std::move(model);
  b.params = p;
  return b;
}
sawblade::CaptureSource tone3000(const std::string& id, const std::string& title, const std::string& creator, const std::string& licence) {
  sawblade::CaptureSource s;
  s.provider = "tone3000";
  s.id = id;
  s.title = title;
  s.creator = creator;
  s.license = licence;
  return s;
}

}  // namespace

TEST_CASE("settings: the gear button opens and closes the panel; Esc closes it", "[editor][settings]") {
  Rig rig;
  SettingsPanel& panel = settingsPanelOf(*rig.ed);
  CHECK_FALSE(panel.isVisible());  // a settings file exists: not a first run
  CHECK_FALSE(rig.ed->settingsOpen());
  juce::Button* gear = buttonTitled(*rig.ed, "Settings");
  REQUIRE(gear != nullptr);
  CHECK(gear->isEnabled());
  CHECK(gear->getTooltip() == "Settings: tool paths, TONE3000 login, cache");
  const auto gb = rig.ed->getLocalArea(gear, gear->getLocalBounds());
  CHECK(gb.getBottom() <= 58);
  // the panel covers the rig area, below the top bar
  const auto pb = rig.ed->getLocalArea(&panel, panel.getLocalBounds());
  CHECK(pb.getY() == 58);
  CHECK(pb.getRight() == skin::RigView::kWidth);
  CHECK(pb.getBottom() == SawbladeEditor::kDesignHeight);

  click(*gear);
  CHECK(panel.isVisible());
  CHECK(rig.ed->settingsOpen());
  CHECK(gear->getToggleState());
  click(*gear);
  CHECK_FALSE(panel.isVisible());

  rig.ed->setSettingsOpen(true);
  CHECK(panel.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
  CHECK_FALSE(panel.isVisible());
  CHECK_FALSE(gear->getToggleState());
  // closing an ordinary (non-first-run) session wrote nothing
  CHECK_FALSE(rig.proc.getName().isEmpty());

  // The narrowed top bar: no button or chip overlaps another.
  std::vector<juce::Component*> bar;
  for (auto* c : gear->getParentComponent()->getChildren())
    if (c->isVisible() && c->getY() < 58 && c->getBottom() <= 58 && dynamic_cast<juce::Button*>(c) != nullptr) bar.push_back(c);
  for (size_t i = 0; i < bar.size(); ++i)
    for (size_t j = i + 1; j < bar.size(); ++j) {
      INFO(bar[i]->getTitle() << " vs " << bar[j]->getTitle());
      CHECK_FALSE(bar[i]->getBounds().intersects(bar[j]->getBounds()));
    }
}

TEST_CASE("settings: first run opens the checklist once per process; DONE writes the file", "[editor][settings]") {
  Rig rig(nullptr);
  const fs::path file = rig.env.dir / "settings.json";
  CHECK_FALSE(fs::exists(file));
  SettingsPanel& panel = settingsPanelOf(*rig.ed);
  CHECK(panel.isVisible());
  CHECK(rig.ed->settingsOpen());
  CHECK(panel.checklistExpanded());
  int rows = 0;
  for (auto* l : all<juce::Label>(*rig.ed))
    if (l->isVisible() && (l->getTitle() == "Tools found" || l->getTitle() == "Logged in" || l->getTitle() == "Captures cached")) ++rows;
  CHECK(rows == 3);

  // every control (including the new ones) has a title and a tooltip
  for (auto* b : all<juce::Button>(*rig.ed)) {
    INFO(b->getTitle());
    CHECK(b->getTitle().isNotEmpty());
    CHECK(b->getTooltip().isNotEmpty());
  }
  for (auto* e : all<juce::TextEditor>(*rig.ed)) CHECK(e->getTitle().isNotEmpty());
  for (auto* c : all<juce::ComboBox>(*rig.ed)) CHECK(c->getTitle().isNotEmpty());

  const juce::Image shot = [&] {
    rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
    return rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  }();
  savePng(shot, "sawblade_firstrun_1x.png");
  CHECK(nonBackgroundFraction(shot, {0, 58, 940, 742}) > 0.03);

  juce::Button* done = buttonTitled(*rig.ed, "DONE");
  REQUIRE(done != nullptr);
  CHECK(done->isVisible());
  click(*done);
  CHECK_FALSE(panel.isVisible());
  CHECK(fs::exists(file));
  CHECK(nlohmann::json::parse(std::ifstream(file))["firstRunCompleted"] == true);

  // A second editor in the same process does not open it.
  std::unique_ptr<juce::AudioProcessorEditor> second(rig.proc.createEditor());
  auto* ed2 = dynamic_cast<SawbladeEditor*>(second.get());
  REQUIRE(ed2 != nullptr);
  CHECK_FALSE(ed2->settingsOpen());
}

TEST_CASE("settings: first run is shown once even when the file is still missing", "[editor][settings]") {
  Rig rig(nullptr);
  CHECK(rig.ed->settingsOpen());
  std::unique_ptr<juce::AudioProcessorEditor> second(rig.proc.createEditor());  // e.g. a DAW with several instances
  CHECK_FALSE(dynamic_cast<SawbladeEditor*>(second.get())->settingsOpen());
}

TEST_CASE("settings: a secret key in the client id field is refused", "[editor][settings]") {
  Rig rig;
  rig.ed->setSettingsOpen(true);
  auto* field = fieldTitled(*rig.ed, "TONE3000 client id");
  REQUIRE(field != nullptr);
  Settings& s = Settings::shared();
  REQUIRE(s.tone3000ClientId().empty());

  field->setText("t3k_cs_x", false);
  REQUIRE(field->onReturnKey != nullptr);
  field->onReturnKey();
  CHECK(anyLabelContains(*rig.ed, "secret"));
  CHECK(anyLabelContains(*rig.ed, "never stores it"));
  CHECK(s.tone3000ClientId().empty());
  CHECK(field->getText().isEmpty());  // restored; never echoes the secret
  CHECK_FALSE(anyLabelContains(*rig.ed, "t3k_cs_x"));
  std::ifstream in(rig.env.dir / "settings.json");
  CHECK(std::string((std::istreambuf_iterator<char>(in)), {}).find("t3k_cs_") == std::string::npos);

  field->setText("t3k_pub_x", false);
  field->onReturnKey();
  CHECK(s.tone3000ClientId() == "t3k_pub_x");
  CHECK_FALSE(anyLabelContains(*rig.ed, "secret"));

  field->setText("odd_key", false);
  field->onReturnKey();
  CHECK(s.tone3000ClientId() == "odd_key");
  CHECK(anyLabelContains(*rig.ed, "does not look like a publishable key"));
}

TEST_CASE("settings: the login view shows the device code and URL; CANCEL ends the job", "[editor][settings][login]") {
  TempFolder tmp;
  const fs::path venv = makeFakeVenv(tmp.dir);
  const std::string json = settingsJsonWithVenv(venv);
  Rig rig(json.c_str());
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  rig.ed->setSettingsOpen(true);
  SettingsPanel& panel = settingsPanelOf(*rig.ed);
  juce::Button* login = buttonTitled(*rig.ed, "Log in to TONE3000");
  REQUIRE(login != nullptr);
  CHECK_FALSE(anyLabelContains(*rig.ed, "ABCD-1234"));
  click(*login);

  REQUIRE(pumpUntil([&] { return anyLabelContains(*rig.ed, "ABCD-1234"); }, 8000));
  CHECK(panel.loginRunning());
  CHECK(anyLabelContains(*rig.ed, "https://www.tone3000.com/device"));
  CHECK(anyLabelContains(*rig.ed, "Waiting for approval"));
  CHECK(anyLabelContains(*rig.ed, "expires in"));
  for (auto* l : all<juce::Label>(*rig.ed))
    if (l->getText() == "ABCD-1234") CHECK(l->getFont().getHeight() >= 36.0f);
  for (const char* b : {"COPY CODE", "COPY URL", "OPEN", "CANCEL"}) {
    INFO(b);
    auto* btn = buttonTitled(*rig.ed, b);
    REQUIRE(btn != nullptr);
    CHECK(btn->isVisible());
  }
  const juce::Image shot = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  savePng(shot, "sawblade_login_1x.png");

  click(*buttonTitled(*rig.ed, "CANCEL"));
  CHECK(pumpUntil([&] { return !panel.loginRunning(); }, 5000));
  CHECK_FALSE(anyLabelContains(*rig.ed, "ABCD-1234"));
}

TEST_CASE("settings: Test buttons run the tools through ToolRunner", "[editor][settings]") {
  TempFolder tmp;
  const fs::path venv = makeFakeVenv(tmp.dir);
  const std::string json = settingsJsonWithVenv(venv);
  Rig rig(json.c_str());
  rig.ed->setSettingsOpen(true);
  std::vector<juce::Button*> tests;
  for (auto* b : all<juce::Button>(*rig.ed))
    if (b->getTitle() == "Test") tests.push_back(b);
  REQUIRE(tests.size() == 2);  // tools, TONE3000
  click(*tests[0]);
  REQUIRE(pumpUntil([&] { return anyLabelContains(*rig.ed, "ok (exit 0,"); }, 8000));
  click(*tests[1]);
  REQUIRE(pumpUntil([&] { return anyLabelContains(*rig.ed, "Logged in as @gatefan (Gate Fan)"); }, 8000));
}

TEST_CASE("settings: the checklist reflects tools, token and captures", "[editor][settings]") {
  TempFolder tmp;
  const fs::path venv = makeFakeVenv(tmp.dir);
  const std::string json = settingsJsonWithVenv(venv);
  Rig rig(json.c_str());
  SettingsPanel& panel = settingsPanelOf(*rig.ed);
  panel.open(/*firstRun=*/true);  // checklist expanded
  enum { Unknown, Ok, Warn, Bad };
  CHECK(panel.checklistLight(0) == Ok);   // both tools present
  CHECK(panel.checklistLight(1) == Bad);  // no token file
  CHECK(anyLabelContains(*rig.ed, "not logged in"));
  std::ofstream(rig.env.dir / "tokens.json") << "{}";
  panel.refresh();
  CHECK(panel.checklistLight(1) == Ok);
  CHECK(anyLabelContains(*rig.ed, "token file present"));

  // a preset with one TONE3000 capture whose file is missing
  sawblade::Preset p;
  p.name = "needs one capture";
  p.a.role = "saw";
  p.a.blocks.push_back(namBlock("a1", "amp", makeCapture(tmp.dir, "missing.nam", false, tone3000("77", "Missing amp", "someone", "cc-by"))));
  rig.proc.restorePreset(p);  // like a host session restore: committed even though its capture cannot be loaded
  rig.proc.waitForLoader(std::chrono::milliseconds(20000));
  REQUIRE(rig.proc.currentPreset().a.blocks.size() == 1);
  panel.refresh();
  CHECK(panel.checklistLight(2) == Bad);
  CHECK(anyLabelContains(*rig.ed, "1 of 1 captures missing"));
  auto* fetch = buttonTitled(*rig.ed, "FETCH CAPTURES");
  REQUIRE(fetch != nullptr);
  CHECK(fetch->isEnabled());

  // no venv at all: red
  Settings::shared().setMatchVenvDir(tmp.dir / "nowhere");
  panel.refresh();
  CHECK(panel.checklistLight(0) == Bad);
  CHECK(anyLabelContains(*rig.ed, "match venv not found"));
}

TEST_CASE("settings: the About box lists the preset's captures with creator, licence and link", "[editor][settings][about]") {
  Rig rig;
  TempFolder tmp;
  sawblade::Preset p;
  p.name = "attribution";
  p.a.role = "saw";
  p.a.blocks.push_back(namBlock("a1", "pedal", makeCapture(tmp.dir, "hm2.nam", true, tone3000("101", "HM-2 clone", "gater", "cc-by-nc"))));
  p.b.role = "body";
  p.b.blocks.push_back(namBlock("b1", "amp", makeCapture(tmp.dir, "local_amp.nam", false, std::nullopt)));
  p.cab.ir = makeCapture(tmp.dir, "cab.wav", true, tone3000("202", "4x12 IR", "cabber", "cc-by"));
  rig.proc.restorePreset(p);
  rig.proc.waitForLoader(std::chrono::milliseconds(20000));
  REQUIRE(rig.proc.currentPreset().cab.ir.file == "cab.wav");

  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  rig.ed->setSettingsOpen(true);
  CHECK_FALSE(rig.ed->aboutOpen());
  juce::Button* about = buttonTitled(*rig.ed, "About Sawblade...");
  REQUIRE(about != nullptr);
  click(*about);
  REQUIRE(rig.ed->aboutOpen());

  const std::string ver = sawblade::plugin::about::kVersion;
  CHECK(anyLabelContains(*rig.ed, juce::String("Sawblade ") + ver));
  CHECK(std::string(sawblade::plugin::about::kGitHash).size() > 0);
  CHECK(std::string(sawblade::plugin::about::kBuildDate).size() > 0);
  CHECK(anyLabelContains(*rig.ed, sawblade::plugin::about::kGitHash));
  CHECK(anyLabelContains(*rig.ed, sawblade::plugin::about::kBuildDate));
  CHECK(anyLabelContains(*rig.ed, "AGPLv3 for personal, non-commercial use"));
  CHECK(anyLabelContains(*rig.ed, "Sawblade is not sold"));

  auto* third = fieldTitled(*rig.ed, "Third-party licences");
  REQUIRE(third != nullptr);
  CHECK(third->isReadOnly());
  CHECK(third->getText().contains("JUCE"));
  CHECK(third->getText().contains("AGPLv3"));

  // one row per capture: title, creator, licence, link
  CHECK(anyLabelContains(*rig.ed, "Saw pedal: HM-2 clone"));
  CHECK(anyLabelContains(*rig.ed, "by gater"));
  CHECK(anyLabelContains(*rig.ed, "licence: cc-by-nc"));
  CHECK(anyLabelContains(*rig.ed, "non-commercial"));
  CHECK(anyLabelContains(*rig.ed, "Cab: 4x12 IR"));
  CHECK(anyLabelContains(*rig.ed, "licence: cc-by"));
  CHECK(anyLabelContains(*rig.ed, "Body amp: local_amp.nam"));
  CHECK(anyLabelContains(*rig.ed, "local file, no TONE3000 metadata"));
  std::vector<juce::String> urls;
  for (auto* h : all<juce::HyperlinkButton>(*rig.ed))
    if (h->isVisible() && h->getURL().toString(false).contains("tone3000.com/tones/")) urls.push_back(h->getURL().toString(false));
  CHECK(urls.size() == 2);
  CHECK(std::find(urls.begin(), urls.end(), juce::String("https://www.tone3000.com/tones/101")) != urls.end());

  const juce::Image shot = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  savePng(shot, "sawblade_about_1x.png");
  CHECK(nonBackgroundFraction(shot, {200, 100, 880, 600}) > 0.05);

  click(*buttonTitled(*rig.ed, "CLOSE"));
  CHECK_FALSE(rig.ed->aboutOpen());
  pumpUntil([] { return false; }, 60);  // let the deferred delete run
  CHECK(buttonTitled(*rig.ed, "CLOSE") == nullptr);
}

TEST_CASE("app icon: every PNG is square, transparent at the corners and opaque in the middle", "[editor][icon]") {
  juce::ScopedJuceInitialiser_GUI gui;
  for (int n : {16, 32, 64, 128, 256, 512, 1024}) {
    const juce::File f = juce::File(SAWBLADE_ICON_DIR).getChildFile("icon_" + juce::String(n) + ".png");
    INFO(f.getFullPathName());
    REQUIRE(f.existsAsFile());
    const juce::Image img = juce::ImageFileFormat::loadFrom(f);
    REQUIRE(img.isValid());
    CHECK(img.getWidth() == n);
    CHECK(img.getHeight() == n);
    for (auto pt : {juce::Point<int>(0, 0), {n - 1, 0}, {0, n - 1}, {n - 1, n - 1}}) CHECK(img.getPixelAt(pt.x, pt.y).getAlpha() == 0);
    CHECK(img.getPixelAt(n / 2, n / 2).getAlpha() == 255);
    CHECK(img.getPixelAt(n / 2, juce::roundToInt(n * 0.2f)).getAlpha() >= 240);  // inside the tile, near the top
  }
}

TEST_CASE("settings: screenshot with the panel open and the checklist collapsed", "[editor][settings]") {
  TempFolder tmp;
  const fs::path venv = makeFakeVenv(tmp.dir);
  const std::string json = settingsJsonWithVenv(venv);
  Rig rig(json.c_str());
  std::ofstream(rig.env.dir / "tokens.json") << "{}";
  rig.ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  const juce::Image closed = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  rig.ed->setSettingsOpen(true);
  SettingsPanel& panel = settingsPanelOf(*rig.ed);
  CHECK_FALSE(panel.checklistExpanded());
  CHECK(anyLabelContains(*rig.ed, "Setup:"));
  pumpUntil([&] { return anyLabelContains(*rig.ed, "captures on disk"); }, 3000);
  const juce::Image open = rig.ed->createComponentSnapshot(rig.ed->getLocalBounds(), true, 1.0f);
  savePng(open, "sawblade_settings_1x.png");
  const juce::Rectangle<int> region(0, 58, SettingsPanel::kWidth, SettingsPanel::kHeight);
  int differing = 0, total = 0;
  for (int y = region.getY(); y < region.getBottom(); ++y)
    for (int x = region.getX(); x < region.getRight(); ++x, ++total)
      if (open.getPixelAt(x, y) != closed.getPixelAt(x, y)) ++differing;
  CHECK(differing > total / 4);
  CHECK(nonBackgroundFraction(open, region) > 0.03);
  // the inspector on the right is untouched
  int changedRight = 0;
  for (int y = 70; y < 790; ++y)
    for (int x = 960; x < 1280; ++x)
      if (open.getPixelAt(x, y) != closed.getPixelAt(x, y)) ++changedRight;
  CHECK(changedRight == 0);
}
