// Tests of the skinned editor (docs/specs/phase2_5_skin.md section 5). Run under a display
// (xvfb-run -a on a headless machine; ctest does this when xvfb-run is installed).

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "PlayAlongPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "rig/EqGraph.h"
#include "rig/RigEditorPanel.h"
#include "rig/RigModel.h"
#include "rig/SlotStrip.h"
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

TEST_CASE("every parameter has exactly one knob bound to it outside the rig panel", "[editor]") {
  Rig rig;
  std::vector<skin::FilmstripKnob*> knobs;
  collectOutsideRigPanel(*rig.ed, knobs);
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
