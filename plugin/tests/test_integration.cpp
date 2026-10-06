// The v0.1 integration walk (docs/specs: v0.1 integration pass). ONE SawbladeEditor over ONE SawbladeProcessor, configured as the
// Standalone app (playAlong().setStandalone(true)), walks every panel in the order a player meets them and saves a 1x screenshot of
// each to <build>/screenshots/v01/<nn>_<name>.png.
//
// Everything is synthetic or fake: SAWBLADE_DATA_DIR / SAWBLADE_APPDATA / SAWBLADE_CACHE_DIR / HOME point into a temp dir, the TONE3000
// tool is plugin/tests/fake_t3k.py (behind a venv wrapper), MATCH and EXPORT run the fake child of fake_tools.h, the song is a
// synthetic stems folder / a synthetic song file separated by a synthetic ONNX model. The factory presets are the committed ones: a
// preset that names TONE3000 captures loads because the capture cache holds stand-ins for them (what `sawblade-t3k fetch` would have
// put there). No GPU, no audio device, no network.
//
// Each step is a TEST_CASE (ctest prefix "integration: "). Steps run in declaration order against one shared Walk; a step run on its
// own (ctest runs every case in its own process) first runs the steps it depends on.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <nlohmann/json.hpp>

#include "BuildInfo.h"
#include "ExportPanel.h"
#include "MatchGlue.h"
#include "MatchScreen.h"
#include "PlayAlongPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SettingsEnv.h"
#include "about/AboutBox.h"
#include "browser/BrowserSettings.h"
#include "browser/CaptureBrowser.h"
#include "fake_tools.h"
#include "mic/MicPage.h"
#include "pedals/AdvancedDrawer.h"
#include "pedals/PedalFace.h"
#include "pedals/PedalSwitch.h"
#include "presets/AbCompare.h"
#include "presets/PresetBrowser.h"
#include "presets/T3kTool.h"
#include "rig/CabScreen.h"
#include "rig/EqGraph.h"
#include "rig/Pedalboard.h"
#include "rig/RigEditorPanel.h"
#include "settings/Settings.h"
#include "settings/SettingsPanel.h"
#include "skin/FilmstripKnob.h"
#include "skin/RigView.h"
#include "sawblade/wav_io.h"
#ifdef SAWBLADE_WITH_SEPARATOR
#include "onnx_synth.h"
#include "sawblade/sha256.h"
#endif

using namespace sawblade;
using namespace sawblade::plugin;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- helpers (the same shapes the editor tests use) ---------------------------------------------------------------------------
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

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, int clicks = 1,
                       int modifiers = juce::ModifierKeys::leftButtonModifier) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(modifiers), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c,
                          &c, now, downPos, now, clicks, true);
}

// A real click (mouse down + up inside the button).
void click(juce::Button& b) {
  juce::Component& c = b;
  const auto centre = b.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(b, centre, centre));
  c.mouseUp(mouse(b, centre, centre));
}
void press(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(c, centre, centre));
}
void doubleClick(juce::Component& c) {
  const auto centre = c.getLocalBounds().toFloat().getCentre();
  c.mouseDoubleClick(mouse(c, centre, centre, 2));
}

juce::Button* buttonTitled(juce::Component& root, const juce::String& title) {
  for (auto* b : all<juce::Button>(root))
    if (b->getTitle() == title) return b;
  return nullptr;
}
bool anyLabelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}
bool anyLabelEquals(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText() == text) return true;
  return false;
}
// The top-bar button of that title (the play-along panel has a MATCH and an EXPORT NAM of its own).
juce::Button* topBarButton(SawbladeEditor& ed, const juce::String& title) {
  for (auto* b : all<juce::Button>(ed))
    if (b->getTitle() == title && ed.getLocalArea(b, b->getLocalBounds()).getBottom() <= 58) return b;
  return nullptr;
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }
template <class Pred>
bool pumpUntil(Pred pred, int timeoutMs = 20000) {
  const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (!pred()) {
    if (juce::Time::getMillisecondCounter() > end) return false;
    pump(15);
  }
  return true;
}
bool waitUntilTrue(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 20000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

void processBlocks(SawbladeProcessor& p, int blocks) {
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  for (int i = 0; i < blocks; ++i) {
    buf.clear();
    p.processBlock(buf, midi);
  }
}
// Feeds `seconds` of audio (silence in, like a quiet DI) at roughly 8x real time so the writer thread can drain its ring.
void feedSeconds(SawbladeProcessor& p, double seconds) {
  const int blocks = static_cast<int>(seconds * 48000.0 / 512.0);
  for (int done = 0; done < blocks; done += 40) {
    processBlocks(p, std::min(40, blocks - done));
    std::this_thread::sleep_for(25ms);
  }
}

double luminance(juce::Colour c) { return 0.299 * c.getFloatRed() + 0.587 * c.getFloatGreen() + 0.114 * c.getFloatBlue(); }
// Fraction of the pixels of `r` that differ visibly from the plain editor background.
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
double luminanceSd(const juce::Image& img) {
  double sum = 0, sum2 = 0;
  const int n = img.getWidth() * img.getHeight();
  for (int y = 0; y < img.getHeight(); ++y)
    for (int x = 0; x < img.getWidth(); ++x) {
      const double l = luminance(img.getPixelAt(x, y));
      sum += l;
      sum2 += l * l;
    }
  const double mean = sum / n;
  return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
}

struct EnvVar {
  std::string k;
  std::optional<std::string> old;
  EnvVar(const std::string& key, const std::string& v) : k(key) {
    if (const char* o = std::getenv(k.c_str())) old = o;
    ::setenv(k.c_str(), v.c_str(), 1);
  }
  ~EnvVar() {
    if (old) ::setenv(k.c_str(), old->c_str(), 1);
    else ::unsetenv(k.c_str());
  }
};

// ---- synthetic inputs (never committed) ---------------------------------------------------------------------------------------
// A folder of four stems, `seconds` long at 48 kHz.
fs::path writeSyntheticSong(const fs::path& root, const std::string& name, double seconds) {
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
  writeWavFloat32Stereo(dir / "drums.wav", 48000.0, drums, drums);
  writeWavFloat32Stereo(dir / "bass.wav", 48000.0, bass, bass);
  writeWavFloat32Stereo(dir / "vocals.wav", 48000.0, vocals, vocals);
  writeWavFloat32Stereo(dir / "other.wav", 48000.0, other, other);
  return dir;
}

// A synthetic cab IR (one-pole low-pass with a presence bump) and a pack in the shape `sawblade-t3k pack` writes: 14 IRs, 8 dots.
std::vector<float> syntheticIr(double fc, double bumpHz, double gain) {
  std::vector<float> h(1500);
  const double a = std::exp(-2.0 * std::numbers::pi * fc / 48000.0);
  double env = 1.0 - a;
  for (std::size_t n = 0; n < h.size(); ++n, env *= a)
    h[n] = static_cast<float>(env * (1.0 + gain * std::cos(2.0 * std::numbers::pi * bumpHz * static_cast<double>(n) / 48000.0)));
  return h;
}
struct Model {
  const char* name;
  double fc, bumpHz, gain;
};
const Model kPackModels[] = {
    {"V30 UL SM57 Cap 0.5in", 6500, 3500, 0.8},     {"V30 UL SM57 Cap 1in", 5200, 3200, 0.7},      {"V30 UL MD421 Cap 1in", 4300, 2800, 0.5},
    {"V30 UL SM57 CapEdge 1in", 4200, 2600, 0.6},   {"V30 UL SM57 CapEdge 2in", 3600, 2300, 0.5}, {"V30 UL SM57 Cone 1in", 3000, 2000, 0.4},
    {"V30 UL R121 Cone 2in", 2400, 1800, 0.3},      {"V30 UL SM57 Edge 1in", 2200, 1500, 0.4},     {"V30 UR SM57 Cap 1in", 5600, 3400, 0.7},
    {"V30 UR SM57 CapEdge 1in", 4400, 2700, 0.6},   {"V30 UR SM57 Cone 1in", 3100, 2100, 0.4},     {"V30 UR SM57 off axis 1in", 2700, 1900, 0.3},
    {"V30 UR SM57 Edge 1in", 2300, 1400, 0.4},      {"V30 UR MD421 Edge 2in", 1900, 1200, 0.3},
};
// Writes the IR files under `dir` and the manifest at `manifest` (the app-data pack cache location of the tone).
void writePack(const fs::path& dir, const fs::path& manifest, const std::string& toneId) {
  fs::create_directories(dir / "irs");
  fs::create_directories(manifest.parent_path());
  json models = json::array();
  int i = 0;
  for (const Model& m : kPackModels) {
    const fs::path f = dir / "irs" / (std::to_string(i) + ".wav");
    writeWavFloat32(f, 48000.0, syntheticIr(m.fc, m.bumpHz, m.gain));
    models.push_back({{"modelId", std::to_string(1000 + i)}, {"name", m.name}, {"file", f.string()}, {"sha256", ""}});
    ++i;
  }
  std::ofstream(manifest) << json{{"toneId", toneId}, {"title", "Synthetic 4x12 test pack"}, {"creator", "testpack"}, {"license", "cc-by"},
                                  {"url", "https://example.org/tones/" + toneId}, {"models", models}}.dump(2);
}

// A preset on the identity fixtures whose captures carry TONE3000 sources (title, creator, licence).
fs::path writeExportRig(const fs::path& dir, const std::string& name, const std::string& license) {
  const std::string nam = (fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / "linear_identity.nam").string();
  const std::string ir = (fs::path(SAWBLADE_FIXTURES_DIR) / "ir" / "impulse.wav").string();
  auto cap = [&](const std::string& file, const std::string& id, const std::string& title, const std::string& creator, const std::string& lic) {
    return json{{"file", file}, {"source", {{"provider", "tone3000"}, {"id", id}, {"title", title}, {"creator", creator}, {"license", lic}}}};
  };
  auto block = [&](const std::string& id, const std::string& slot, json model) { return json{{"id", id}, {"type", "nam"}, {"slot", slot}, {"model", std::move(model)}}; };
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name},
            {"paths", {{"a", {{"role", "saw"}, {"blocks", json::array({block("a1", "pedal", cap(nam, "11", "HM-2w CHAINSAW", "@ebheron", license)),
                                                                     block("a2", "amp", cap(nam, "12", "JCM800 2203", "@sarcobe", "t3k"))})}}},
                       {"b", {{"role", "body"}, {"blocks", json::array({block("b1", "amp", cap(nam, "13", "5150III Ivory", "@AmpsPedalsPickups", "cc-by"))})}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"cab", {{"mode", "shared"}, {"ir", cap(ir, "21", "V30 Mesa 4x12", "@OutmodedElectronics", "t3k")}}},
            {"busComp", {{"enabled", true}, {"releaseMs", 80.0}}},
            {"postEq", json::array()}};
  const fs::path p = dir / (name + ".json");
  std::ofstream(p) << j.dump(2);
  return p;
}

// Both paths identity (a nam block each), shared impulse cab; path B at -6 dB, level match off, constant-loudness law.
fs::path writeLevelsRig(const fs::path& dir) {
  const fs::path fx = SAWBLADE_FIXTURES_DIR;
  auto block = [&](const std::string& id) { return json{{"id", id}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (fx / "nam" / "linear_identity.nam").string()}}}}; };
  json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "Levels rig"},
            {"paths", {{"a", {{"role", "saw"}, {"blocks", json::array({block("a1")})}}}, {"b", {{"role", "body"}, {"levelDb", -6.0}, {"blocks", json::array({block("b1")})}}}}},
            {"align", {{"mode", "off"}}},
            {"blend", 0.5},
            {"levelMatch", {{"mode", "off"}}},
            {"blendLaw", "constantLoudness"},
            {"cab", {{"mode", "shared"}, {"ir", {{"file", (fx / "ir" / "impulse.wav").string()}}}}},
            {"postEq", json::array({{{"type", "peak"}, {"freq", 1000}, {"gainDb", 0.0}, {"q", 1.0}}})}};
  const fs::path p = dir / "levels_rig.json";
  std::ofstream(p) << j.dump(2);
  return p;
}

// The mic page's rig: a cab whose IR comes from the synthetic pack (tone 321, model 1001).
fs::path writeMicRig(const fs::path& dir, const fs::path& manifest) {
  const json models = json::parse(std::ifstream(manifest))["models"];
  const std::string nam = (fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / "linear_identity.nam").string();
  auto block = [&](const char* id) { return json{{"id", id}, {"type", "nam"}, {"model", {{"file", nam}}}}; };
  json cabIr = {{"file", models[1]["file"]}, {"source", {{"provider", "tone3000"}, {"id", "321"}, {"modelId", "1001"}, {"title", models[1]["name"]}, {"creator", "testpack"}, {"license", "cc-by"}}}};
  const json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "Mic test"},
                  {"paths", {{"a", {{"blocks", json::array({block("a1")})}}}, {"b", {{"blocks", json::array({block("b1")})}}}}},
                  {"align", {{"mode", "off"}}}, {"blend", 0.5}, {"cab", {{"mode", "shared"}, {"ir", cabIr}}}};
  const fs::path pf = dir / "mic_rig.json";
  std::ofstream(pf) << j.dump(2);
  return pf;
}

// Puts a stand-in for every TONE3000 capture a preset names into the capture cache (<cache>/<tone id>/<model id>.nam | .wav), as
// `sawblade-t3k fetch` / `resolve` would have: the fixture identity NAM and impulse IR.
void populateCache(const json& node, const fs::path& cache) {
  if (node.is_object()) {
    if (node.contains("file") && node["file"].is_string() && node.contains("source") && node["source"].is_object()) {
      const auto& s = node["source"];
      if (s.value("provider", "") == "tone3000" && s.contains("id") && s.contains("modelId")) {
        const std::string file = node["file"].get<std::string>();
        const bool ir = fs::path(file).extension() == ".wav";
        const fs::path dest = cache / s["id"].get<std::string>() / (s["modelId"].get<std::string>() + (ir ? ".wav" : ".nam"));
        fs::create_directories(dest.parent_path());
        fs::copy_file(fs::path(SAWBLADE_FIXTURES_DIR) / (ir ? "ir/impulse.wav" : "nam/linear_identity.nam"), dest, fs::copy_options::skip_existing);
      }
    }
    for (const auto& kv : node.items()) populateCache(kv.value(), cache);
  } else if (node.is_array()) {
    for (const auto& v : node) populateCache(v, cache);
  }
}
void populateCacheForDir(const fs::path& presetsDir, const fs::path& cache) {
  for (const auto& e : fs::recursive_directory_iterator(presetsDir)) {
    if (e.path().extension() != ".json" || e.path().string().find("/modeled/") != std::string::npos) continue;
    const json j = json::parse(std::ifstream(e.path()), nullptr, /*allow_exceptions=*/false);
    if (j.is_object()) populateCache(j, cache);
  }
}

// ---- screenshots ---------------------------------------------------------------------------------------------------------------
fs::path shotDir() {
  const char* envDir = std::getenv("SAWBLADE_SCREENSHOT_DIR");
  return fs::path(envDir != nullptr && *envDir != '\0' ? envDir : SAWBLADE_SCREENSHOT_DIR) / "v01";
}
void savePng(const juce::Image& img, const juce::String& name) {
  const juce::File dir(shotDir().string());
  REQUIRE(dir.createDirectory().wasOk());
  const juce::File f = dir.getChildFile(name + ".png");
  f.deleteFile();
  juce::FileOutputStream out(f);
  REQUIRE(out.openedOk());
  juce::PNGImageFormat png;
  REQUIRE(png.writeImageToStream(img, out));
}
// A 1x snapshot of the editor at its 1280 x 800 design size: checked for size and not blank, then saved.
juce::Image shot(SawbladeEditor& ed, const juce::String& name) {
  pump(80);  // the 4 Hz refresh and any queued callbacks settle first
  ed.setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  const juce::Image img = ed.createComponentSnapshot(ed.getLocalBounds(), true, 1.0f);
  REQUIRE(img.getWidth() == 1280);
  REQUIRE(img.getHeight() == 800);
  INFO("screenshot " << name);
  CHECK(luminanceSd(img) > 0.03);
  savePng(img, name);
  return img;
}

// ---- the walk's world --------------------------------------------------------------------------------------------------------
struct Walk {
  SettingsEnv env{kSettingsExist, /*isolateHome=*/true};
  EnvVar appData{"SAWBLADE_APPDATA", (env.dir / "appdata").string()};
  EnvVar dataDir{"SAWBLADE_DATA_DIR", (env.dir / "appdata").string()};
  EnvVar noModels{"SAWBLADE_MODELS_DIR", (env.dir / "models").string()};
  EnvVar stemsDir{"SAWBLADE_STEMS_DIR", (env.dir / "stems").string()};
  juce::ScopedJuceInitialiser_GUI gui;
  fs::path root = env.dir;
  fs::path appdata = env.dir / "appdata";
  fake_tools::Toolbox tools{env.dir / "tools"};  // <tools>/bin/{sawblade-match, sawblade-export}: the fake child
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  Walk() {
    // ONE place for the tool paths: the Settings panel's match venv = <tools>, so <tools>/bin has all three tools. No older store
    // (browser.settings, settings.json t3kExecutable, the match settings file) is given a path.
    {
      std::ofstream w(tools.root / "bin" / "sawblade-t3k");
      w << "#!/bin/sh\nif [ \"$1\" = \"--help\" ]; then echo 'usage: sawblade-t3k [-h] {login,whoami,pull,search,resolve} ...'; exit 0; fi\n"
           "exec python3 \"" SAWBLADE_FAKE_T3K "\" \"$@\"\n";
    }
    fs::permissions(tools.root / "bin" / "sawblade-t3k", fs::perms::owner_all);
    REQUIRE(settings::Settings::shared().setMatchVenvDir(tools.root).ok);
    // the matcher's pool manifest is looked up in <HOME>/.cache/sawblade/captures (HOME is the temp dir)
    const fs::path pool = env.dir / "home" / ".cache" / "sawblade" / "captures" / "pool_manifest.json";
    fs::create_directories(pool.parent_path());
    fs::copy_file(tools.pool, pool);
    populateCacheForDir(SAWBLADE_PRESETS_DIR, captureCacheRoot());

    proc.playAlong().setStandalone(true);
    proc.prepareToPlay(48000.0, 512);
    REQUIRE(proc.waitForLoader(60000ms));
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Walk() { base.reset(); }

  void load(const fs::path& file) {
    INFO("loading " << file.string());
    REQUIRE(proc.loadPresetFile(file));
    REQUIRE(proc.waitForLoader(60000ms));
    REQUIRE(proc.status().error.empty());
    pump(60);
  }
  void settle() { REQUIRE(proc.waitForLoader(60000ms)); pump(80); }

  // The visible overlays, as one string for messages and comparisons.
  struct Overlays {
    bool rig, mic, browser, match, exportPanel, settings, cab;
    int count() const { return int(rig) + int(mic) + int(browser) + int(match) + int(exportPanel) + int(cab); }
  };
  Overlays overlays() const {
    return {ed->rigEditorOpen(), ed->micPageOpen(), ed->browserOpen(), ed->matchScreenOpen(), ed->exportPanelOpen(), ed->settingsOpen(), ed->cabPageOpen()};
  }

  PlayAlongPanel& panel() { return *all<PlayAlongPanel>(*ed).at(0); }
  MatchScreen& screen() { return *all<MatchScreen>(*ed).at(0); }
  ExportPanel& exportPanel() { return ed->exportPanel(); }
  rig::RigEditorPanel& rigPanel() { return ed->rigEditor(); }
  PedalFace& face() { return *all<PedalFace>(*ed).at(0); }
  AdvancedDrawer& drawer() { return *all<AdvancedDrawer>(*ed).at(0); }
  skin::RigView& rigView() { return *all<skin::RigView>(*ed).at(0); }
  juce::Button* matchButton(const juce::String& t) { return buttonTitled(screen(), t); }
  juce::Button* exportButton(const juce::String& t) { return buttonTitled(exportPanel(), t); }
  int presetIndex(const std::string& name) {
    const auto& e = ed->browser().library().entries();
    for (int i = 0; i < static_cast<int>(e.size()); ++i)
      if (e[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
  }
  double param(int index) { return static_cast<double>(proc.parameters().getRawParameterValue(paramSpec(index).id)->load()); }
  void setParam(int index, double value) {
    auto* p = proc.parameters().getParameter(paramSpec(index).id);
    p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(value)));
  }
};

std::unique_ptr<Walk> gWalk;
Walk& walk() {
  if (!gWalk) gWalk = std::make_unique<Walk>();
  return *gWalk;
}
// The Walk (JUCE objects, a message manager) must be gone before static destruction.
struct WalkReleaser : Catch::EventListenerBase {
  using Catch::EventListenerBase::EventListenerBase;
  void testRunEnded(const Catch::TestRunStats&) override { gWalk.reset(); }
};

// ---- the steps ---------------------------------------------------------------------------------------------------------------
enum Step { kMain, kPresetBrowser, kAbCompare, kRigEditor, kPedal, kDrawer, kCaptureBrowser, kMicPage, kPlayAlong, kRecord, kMatchProgress, kMatchResults,
            kExport, kSeparation, kOverlays, kStateRoundtrip, kLevels, kSettings, kNumSteps };
bool gDone[kNumSteps] = {};
void runStep(Step s);

// Steps that must have run before `s`.
std::vector<Step> needs(Step s) {
  switch (s) {
    case kAbCompare: return {kMain};
    case kRigEditor: return {kMain};
    case kPlayAlong: return {};
    case kRecord: return {kPlayAlong};
    case kMatchProgress: return {kRecord};
    case kMatchResults: return {kMatchProgress};
    case kExport: return {kMatchResults};
    case kSeparation: return {kRecord};
    default: return {};
  }
}

void step01Main(Walk& w) {
  INFO("01 main");
  auto& br = w.ed->browser();
  br.scanBlocking();  // the library at <repo>/presets (settings key factoryPresetDir: unset)
  const int idx = w.presetIndex("UK Death (Bolt Thrower-style)");
  REQUIRE(idx >= 0);
  const auto& entry = br.library().entries()[static_cast<std::size_t>(idx)];
  CHECK(entry.bank == SubBank::Styles);
  REQUIRE(entry.loadable());
  REQUIRE(br.loadEntry(idx));
  w.settle();
  REQUIRE(w.proc.status().error.empty());
  CHECK(w.proc.status().presetName == "UK Death (Bolt Thrower-style)");
  CHECK(w.proc.currentPreset().name == "UK Death (Bolt Thrower-style)");
  auto* selector = buttonTitled(*w.ed, "Preset");
  REQUIRE(selector != nullptr);
  CHECK(pumpUntil([&] { return selector->getButtonText() == "UK DEATH (BOLT THROWER-STYLE)"; }, 3000));
  CHECK(w.proc.playAlong().standalone());
  const auto o = w.overlays();
  CHECK(o.count() == 0);
  CHECK_FALSE(o.settings);
  CHECK_FALSE(w.ed->playAlongOpen());
  const juce::Image img = shot(*w.ed, "01_main");
  CHECK(nonBackgroundFraction(img, {0, 58, 940, 742}) > 0.12);  // the rig renders are on screen (v0.4 Task D: the cab render left the main page)
}

void step02PresetBrowser(Walk& w) {
  INFO("02 preset_browser");
  auto* selector = buttonTitled(*w.ed, "Preset");
  REQUIRE(selector != nullptr);
  click(*selector);
  REQUIRE(w.ed->browserOpen());
  auto& br = w.ed->browser();
  br.scanBlocking();
  juce::TextEditor* search = nullptr;
  for (auto* t : all<juce::TextEditor>(br)) search = t;
  REQUIRE(search != nullptr);
  search->setText("doom", true);
  REQUIRE(br.visible().size() >= 1);
  const int want = w.presetIndex("Fuzz Doom (Electric Wizard / Conan-style)");
  REQUIRE(want >= 0);
  int row = -1;
  for (std::size_t i = 0; i < br.visible().size(); ++i)
    if (br.visible()[i] == want) row = static_cast<int>(i);
  REQUIRE(row >= 0);

  // double-click the result's row in the list (264, 58 design px; rows are 46 px)
  const juce::Point<int> at(264 + 200, 58 + 46 * row + 23);
  juce::Component* list = br.getComponentAt(at);
  REQUIRE(list != nullptr);
  REQUIRE(list != &br);
  const auto local = list->getLocalPoint(&br, at.toFloat());
  list->mouseDown(mouse(*list, local, local));
  list->mouseUp(mouse(*list, local, local));
  list->mouseDoubleClick(mouse(*list, local, local, 2));
  w.settle();
  REQUIRE(w.proc.status().error.empty());
  CHECK(w.proc.currentPreset().name == "Fuzz Doom (Electric Wizard / Conan-style)");
  CHECK(br.selectedEntry() == want);
  const juce::Image img = shot(*w.ed, "02_preset_browser");
  CHECK(nonBackgroundFraction(img, {0, 58, 1280, 742}) > 0.05);
  CHECK(anyLabelContains(*w.ed, "PRESETS"));
  click(*br.buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 BACK")));
  CHECK_FALSE(w.ed->browserOpen());
}

void step03AbCompare(Walk& w) {
  INFO("03 ab_compare");
  auto* ab = buttonTitled(*w.ed, "A/B compare");
  REQUIRE(ab != nullptr);
  CHECK(ab->getButtonText() == "A");
  const std::string nameA = w.proc.status().presetName;
  click(*ab);
  CHECK(ab->getButtonText() == "B");
  w.settle();
  CHECK(w.proc.status().presetName == nameA);  // B starts as a copy of A
  shot(*w.ed, "03_ab_compare");
  click(*ab);
  CHECK(ab->getButtonText() == "A");
  w.settle();
  // right-click: the context menu (copy A to B, B to A, reset). Showing it needs a window manager (JUCE's native popup window
  // raises an X BadAtom under bare xvfb), so the test checks the button advertises it.
  CHECK(ab->getTooltip().contains("right-click"));
  CHECK(ab->getTooltip().contains("copy A to B"));
}

void step04to09RigEditor(Walk& w) {
  INFO("04-09 rig_editor");
  // A blend, so that both lanes are populated: "Barbaric Pleasures" from the Matched bank.
  w.ed->browser().scanBlocking();
  const int idx = w.presetIndex("Barbaric Pleasures \xc2\xb7 matched v4");
  REQUIRE(idx >= 0);
  REQUIRE(w.ed->browser().loadEntry(idx));
  w.settle();
  INFO(w.proc.status().error);
  REQUIRE(w.proc.status().error.empty());

  auto* rigBtn = buttonTitled(*w.ed, "RIG");
  REQUIRE(rigBtn != nullptr);
  CHECK_FALSE(w.ed->rigEditorOpen());
  click(*rigBtn);
  REQUIRE(w.ed->rigEditorOpen());
  CHECK(rigBtn->getToggleState());
  auto& panel = w.rigPanel();
  panel.refresh();
  struct TabShot {
    rig::RigEditorPanel::Tab tab;
    const char* file;
  };
  const TabShot tabs[] = {{rig::RigEditorPanel::Tab::Chain, "04_rig_editor_chain"}, {rig::RigEditorPanel::Tab::Eq, "05_rig_editor_eq"},
                          {rig::RigEditorPanel::Tab::Blend, "06_rig_editor_blend"}, {rig::RigEditorPanel::Tab::Cab, "07_rig_editor_cab"},
                          {rig::RigEditorPanel::Tab::Gate, "08_rig_editor_gate"}, {rig::RigEditorPanel::Tab::Comp, "09_rig_editor_comp"}};
  for (const TabShot& t : tabs) {
    INFO(t.file);
    click(panel.tabButton(t.tab));
    CHECK(panel.tab() == t.tab);
    if (t.tab == rig::RigEditorPanel::Tab::Chain) {  // the matched preset is a single path: BLEND turns on the second lane
      click(panel.topologyButton(rig::Topology::Blend));
      w.settle();
    }
    panel.refresh();
    const juce::Image img = shot(*w.ed, t.file);
    CHECK(nonBackgroundFraction(img, {0, 58, 940, 742}) > 0.04);
    if (t.tab == rig::RigEditorPanel::Tab::Chain) {
      CHECK(rig::topologyOf(w.proc.currentPreset()) == rig::Topology::Blend);
      CHECK(anyLabelContains(*w.ed, juce::String::fromUTF8("A \xC2\xB7 SAW")));
      CHECK(anyLabelContains(*w.ed, juce::String::fromUTF8("B \xC2\xB7 BODY")));
    }
    if (t.tab == rig::RigEditorPanel::Tab::Eq) CHECK(panel.eqGraph().isVisible());
  }
  click(*rigBtn);
  CHECK_FALSE(w.ed->rigEditorOpen());
  CHECK_FALSE(rigBtn->getToggleState());
}

void step10PedalFace(Walk& w) {
  INFO("10 pedal_face");
  w.load(fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "chainsaw" / "classic_buzzsaw.json");
  auto& face = w.face();
  face.refresh();
  REQUIRE(face.isVisible());
  REQUIRE(face.activeCircuit().has_value());
  CHECK(*face.activeCircuit() == Circuit::Chainsaw);
  CHECK(w.param(kSawCircuit) == 0.0);
  const auto pedal = face.getBounds();
  const char* files[] = {"10_pedal_face", "10_pedal_face_bigfuzz", "10_pedal_face_moddedsaw", "10_pedal_face_oneknobsaw"};
  const Circuit order[] = {Circuit::Chainsaw, Circuit::BigFuzz, Circuit::ModdedSaw, Circuit::OneKnobSaw};
  PedalSwitch& circuit = face.circuitSwitch();
  for (int i = 0; i < 4; ++i) {
    INFO("circuit " << i);
    if (i > 0) {
      press(circuit);  // the CIRCUIT switch steps to the next circuit through the loader
      w.settle();
      face.refresh();
    }
    REQUIRE(face.activeCircuit().has_value());
    CHECK(*face.activeCircuit() == order[i]);
    CHECK(w.param(kSawCircuit) == static_cast<double>(i));  // the host parameter follows
    CHECK(circuit.position() == i);
    const juce::Image img = shot(*w.ed, files[i]);
    CHECK(nonBackgroundFraction(img, {pedal.getX(), pedal.getY(), pedal.getWidth(), pedal.getHeight()}) > 0.3);
  }
  press(circuit);  // one-knob saw -> chainsaw
  w.settle();
  face.refresh();
  REQUIRE(face.activeCircuit().has_value());
  CHECK(*face.activeCircuit() == Circuit::Chainsaw);
  CHECK(w.param(kSawCircuit) == 0.0);
}

void step11PedalDrawer(Walk& w) {
  INFO("11 pedal_drawer");
  auto& face = w.face();
  auto& drawer = w.drawer();
  w.ed->refreshNow();
  face.refresh();
  REQUIRE(face.isVisible());
  rig::BoardTile* tile = w.ed->pedalboard().tileForBlock(0, 0);  // the circuit pedal's tile on the SAW board
  REQUIRE(tile != nullptr);
  CHECK_FALSE(drawer.isOpen());
  doubleClick(*tile);
  CHECK(drawer.isOpen());
  drawer.finishAnimation();
  CHECK(drawer.isVisible());
  CHECK(drawer.getBounds() == drawer.openBounds());
  drawer.refresh();
  const juce::Image img = shot(*w.ed, "11_pedal_drawer");
  CHECK(nonBackgroundFraction(img, drawer.getBounds()) > 0.2);
  CHECK(drawer.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
  CHECK_FALSE(drawer.isOpen());
  drawer.finishAnimation();
  CHECK_FALSE(drawer.isVisible());
}

void step12CaptureBrowser(Walk& w) {
  INFO("12 capture_browser");
  auto* browse = buttonTitled(*w.ed, "BROWSE CAPTURES");
  REQUIRE(browse != nullptr);
  CHECK(all<CaptureBrowser>(*w.ed).empty());
  click(*browse);
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*w.ed).size() == 1; }));
  CaptureBrowser& cb = *all<CaptureBrowser>(*w.ed)[0];
  auto& ctl = cb.controller();
  // the tool is the one in the Settings venv: nothing was set in the browser's own store
  CHECK(ctl.executable() == (w.tools.root / "bin" / "sawblade-t3k").string());
  REQUIRE(pumpUntil([&] { return ctl.state().view == BrowserController::View::Browse && !ctl.state().loading; }));
  // SEARCH through the fake: the SEARCH source, a query, Enter
  juce::TextEditor* search = nullptr;
  for (auto* t : all<juce::TextEditor>(cb))
    if (t->getTitle() == "Search captures") search = t;
  REQUIRE(search != nullptr);
  ctl.setSource(BrowserController::Source::Search);
  search->setText("chainsaw", false);
  search->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
  REQUIRE(pumpUntil([&] { return ctl.state().query == "chainsaw" && !ctl.state().loading && !ctl.state().records.empty(); }));
  CHECK(ctl.state().records.size() == 6);
  CHECK(ctl.visible().size() == 5);  // QUALITY on hides the failing record
  ctl.select(101);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2 && ctl.selected() != nullptr && ctl.selected()->toneId == 101; }));
  CHECK(anyLabelContains(cb, "Boss HM-2w CHAINSAW"));
  CHECK(anyLabelContains(cb, "SAW PEDAL SLOT"));
  CHECK(cb.getWidth() == 1280);
  shot(*w.ed, "12_capture_browser");
  auto* back = buttonTitled(cb, juce::String::fromUTF8("\xe2\x80\xb9 RIG"));
  REQUIRE(back != nullptr);
  click(*back);
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(*w.ed).empty(); }));
  CHECK(w.overlays().count() == 0);
}

void step13MicPage(Walk& w) {
  INFO("13 mic_page");
  const fs::path manifest = packManifestPath("321");  // <appdata>/packs/321.json: what LOAD PACK caches
  writePack(w.root / "pack", manifest, "321");
  w.load(writeMicRig(w.root, manifest));
  // v0.4 Task D: the cab has its own page (the CAB button, or the cab chip), and MIC POSITIONS on it opens the mic page.
  auto* cabBtn = buttonTitled(*w.ed, "Cab page");
  REQUIRE(cabBtn != nullptr);
  click(*cabBtn);
  REQUIRE(w.ed->cabPageOpen());
  CHECK(cabBtn->getToggleState());
  auto* micBtn = buttonTitled(w.ed->cabScreen(), "MIC POSITIONS");
  REQUIRE(micBtn != nullptr);
  click(*micBtn);
  REQUIRE(w.ed->micPageOpen());
  CHECK_FALSE(w.ed->cabPageOpen());
  MicPage& page = w.ed->micPage();
  CHECK(page.dotCount() == 8);  // the cached pack was found by the cab's tone id
  CHECK(page.session().pack().size() == 14);
  CHECK(page.responseShown());

  const auto before = w.proc.currentPreset().cab.ir;
  auto& st = page.stage();
  auto drag = [&](int mic, int dot) {
    const auto from = page.micCentre(mic).isOrigin() ? page.dotCentre(0) : page.micCentre(mic);
    const auto fromLocal = from - st.getPosition().toFloat();
    const auto toLocal = page.dotCentre(dot) + juce::Point<float>(4.0f, -3.0f) - st.getPosition().toFloat();
    st.mouseDown(mouse(st, fromLocal, fromLocal));
    st.mouseDrag(mouse(st, (fromLocal + toLocal) * 0.5f, fromLocal));
    st.mouseDrag(mouse(st, toLocal, fromLocal));
    st.mouseUp(mouse(st, toLocal, fromLocal));
  };
  drag(0, 5);
  w.settle();
  const auto after = w.proc.currentPreset().cab.ir;
  CHECK_FALSE(after == before);
  const auto& pack = page.session().pack();
  CHECK(pack.dotOfModel(pack.findModel(after)) == 5);

  auto* blend = page.buttonTitled("BLEND 2 MICS");
  REQUIRE(blend != nullptr);
  click(*blend);
  w.settle();
  const Preset p = w.proc.currentPreset();
  CHECK(p.cab.mode == CabMode::IrMix);
  CHECK(w.proc.status().liveCompatible);
  CHECK(page.mixSlider().isVisible());
  drag(1, 6);
  w.settle();
  CHECK_FALSE(w.proc.currentPreset().cab.irB == w.proc.currentPreset().cab.irA);
  const juce::Image img = shot(*w.ed, "13_mic_page");
  CHECK(nonBackgroundFraction(img, {0, 58, 1280, 742}) > 0.1);
  click(*page.buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  CHECK_FALSE(w.ed->micPageOpen());
  CHECK(w.ed->cabPageOpen());  // closing the mic page returns to the CAB page it was opened from
  shot(*w.ed, "13_cab_page");
  click(*cabBtn);
  CHECK_FALSE(w.ed->cabPageOpen());
  CHECK(w.overlays().count() == 0);
}

void step14PlayAlongOpen(Walk& w) {
  INFO("14 playalong_open");
  auto& pa = w.proc.playAlong();
  auto* toggle = buttonTitled(*w.ed, "PLAY ALONG");
  REQUIRE(toggle != nullptr);
  click(*toggle);
  REQUIRE(w.ed->playAlongOpen());
  CHECK(w.panel().isVisible());
  const auto song = writeSyntheticSong(w.root, "Gatecreeper - Dark Superstition (stems)", 40.0);
  pa.loadFolder(song.string(), true);
  REQUIRE(pa.waitForLoader());
  processBlocks(w.proc, 4);
  w.panel().refresh();
  CHECK(anyLabelContains(w.panel(), "Gatecreeper"));
  auto* play = buttonTitled(w.panel(), "PLAY");
  REQUIRE(play != nullptr);
  click(*play);
  processBlocks(w.proc, 6);
  w.panel().refresh();
  CHECK(play->getButtonText() == "PAUSE");
  // the record band is part of the panel
  auto* rec = buttonTitled(w.panel(), "REC");
  REQUIRE(rec != nullptr);
  CHECK(rec->isVisible());
  CHECK(buttonTitled(w.panel(), "USE FOR MATCH")->isVisible());
  const juce::Image img = shot(*w.ed, "14_playalong_open");
  const juce::Rectangle<int> region(0, 800 - PlayAlongPanel::kHeight, 1280, PlayAlongPanel::kHeight);
  CHECK(nonBackgroundFraction(img, region) > 0.1);
}

void step15Record(Walk& w) {
  INFO("15 record");
  auto& rec = w.proc.recorder();
  auto& panel = w.panel();
  auto* recBtn = buttonTitled(panel, "REC");
  REQUIRE(recBtn != nullptr);
  auto* list = all<juce::ListBox>(panel).at(0);
  const int rowsBefore = list->getListBoxModel()->getNumRows();
  click(*recBtn);
  CHECK(rec.state() == TakeRecorder::State::Armed);
  processBlocks(w.proc, 6);
  panel.refresh();
  CHECK(rec.state() == TakeRecorder::State::Recording);
  CHECK(recBtn->getButtonText() == "STOP");
  feedSeconds(w.proc, 0.5);
  panel.refresh();
  CHECK(anyLabelContains(panel, "REC 00:"));
  click(*recBtn);  // stop
  processBlocks(w.proc, 2);
  REQUIRE(rec.waitIdle());
  panel.refresh();
  CHECK(recBtn->getButtonText() == "REC");
  CHECK(list->getListBoxModel()->getNumRows() == rowsBefore + 1);
  CHECK_FALSE(anyLabelContains(panel, "No takes yet"));
  CHECK(rec.listTakes().size() == static_cast<std::size_t>(rowsBefore) + 1);
  CHECK(fs::exists(rec.listTakes().at(0).wav));
  CHECK(rec.listTakes().at(0).wav.string().find((w.appdata / "takes").string()) == 0);  // the takes folder follows the app-data dir
  shot(*w.ed, "15_record");
}

void step16MatchProgress(Walk& w) {
  INFO("16 match_progress");
  auto& panel = w.panel();
  auto& rec = w.proc.recorder();
  REQUIRE_FALSE(rec.listTakes().empty());
  const std::string take = rec.listTakes().at(0).name;
  CHECK(w.proc.matchSettings().selectedTake().empty());
  click(*buttonTitled(panel, "USE FOR MATCH"));
  CHECK(w.proc.matchSettings().selectedTake() == take);
  // the tools were found through the Settings venv: nothing was set in the match settings file
  CHECK(w.proc.matchSettings().matchExecutable() == w.tools.root / "bin" / "sawblade-match");
  CHECK(w.proc.matchSettings().exportExecutable() == w.tools.root / "bin" / "sawblade-export");

  // the play-along panel's own MATCH button reaches the same screen as the top bar's
  click(*buttonTitled(panel, "MATCH"));
  CHECK(w.ed->matchScreenOpen());
  w.screen().close();
  CHECK_FALSE(w.ed->matchScreenOpen());

  // a quick pass parked at stage 2; its thorough pass (auto-refine) parked at g1
  w.tools.cfgTwoPass({{"gatesQuick", json::array({"g2"})}, {"gatesThorough", json::array({"g1"})}, {"thoroughLevelDb", 2.0}});
  auto* top = topBarButton(*w.ed, "MATCH");
  REQUIRE(top != nullptr);
  CHECK(top->isEnabled());
  click(*top);
  REQUIRE(w.ed->matchScreenOpen());
  auto& screen = w.screen();
  CHECK(anyLabelContains(screen, "'other' stem"));
  CHECK(anyLabelContains(screen, "Gatecreeper"));
  CHECK(anyLabelContains(screen, take));
  CHECK(anyLabelContains(screen, "matched by tone"));  // the take was recorded with the backing running
  auto* start = w.matchButton("START MATCH");
  REQUIRE(start != nullptr);
  REQUIRE(start->isEnabled());
  click(*start);
  REQUIRE(waitUntilTrue([&] { return w.proc.jobs().snapshot(JobKind::Match).progress.message == "refining 1/3"; }));
  std::this_thread::sleep_for(1100ms);  // some elapsed time to show
  screen.refresh();
  CHECK(anyLabelContains(screen, "stage 2: fine-tuning"));
  CHECK(anyLabelContains(screen, "refining 1/3"));
  CHECK(anyLabelContains(screen, "best error 4.20 dB"));
  CHECK(w.proc.jobs().snapshot(JobKind::Match).pass == "quick");
  const juce::Image img = shot(*w.ed, "16_match_progress");
  CHECK(nonBackgroundFraction(img, {0, 100, 1280, 700}) > 0.03);
}

void step17MatchResults(Walk& w) {
  INFO("17 match_results");
  auto& proc = w.proc;
  auto& screen = w.screen();
  // the quick pass finishes; the thorough pass starts and parks at g1
  fake_tools::release(proc.jobs().snapshot(JobKind::Match).dir, "g2");
  REQUIRE(waitUntilTrue([&] { return proc.jobs().snapshot(JobKind::Match).state == JobState::Succeeded; }));
  REQUIRE(waitUntilTrue([&] { return proc.jobs().refineSnapshot().state == JobState::Running && proc.jobs().refineSnapshot().progress.fraction > 0.0; }));
  screen.refresh();
  auto* results = all<juce::ListBox>(screen).at(0);
  REQUIRE(results->getListBoxModel()->getNumRows() == 3);
  CHECK(anyLabelEquals(screen, "PREVIEW"));
  CHECK(anyLabelContains(screen, juce::String::fromUTF8("REFINING\xe2\x80\xa6")));
  CHECK(anyLabelContains(screen, "Done (quick pass)"));
  CHECK_FALSE(w.matchButton("APPLY REFINED BEST")->isVisible());
  const std::uint64_t builds0 = proc.engineBuilds();
  const std::string beforeName = proc.status().presetName;
  CHECK(proc.status().presetName == beforeName);

  // AUDITION the second candidate, A/B, APPLY
  results->selectRow(1);
  click(*w.matchButton("AUDITION"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "quick alt 1");
  CHECK(anyLabelContains(screen, "now playing B"));
  REQUIRE(w.matchButton("A / B") != nullptr);
  CHECK(w.matchButton("A / B")->getButtonText() == "A / B  (B)");
  click(*w.matchButton("A / B"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == beforeName);
  CHECK(w.matchButton("A / B")->getButtonText() == "A / B  (A)");
  click(*w.matchButton("A / B"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.engineBuilds() > builds0);
  shot(*w.ed, "17_match_results");
  click(*w.matchButton("APPLY"));
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "quick alt 1");
  CHECK(anyLabelContains(screen, "Applied"));

  // the refine job finishes: a REFINED section; nothing is loaded by itself
  const std::uint64_t builds = proc.engineBuilds();
  fake_tools::release(proc.jobs().refineSnapshot().dir, "g1");
  REQUIRE(proc.jobs().waitRefineFinished());
  screen.refresh();
  CHECK(proc.engineBuilds() == builds);
  CHECK(proc.status().presetName == "quick alt 1");
  CHECK(anyLabelEquals(screen, "REFINED READY"));
  CHECK(anyLabelContains(screen, "Refined result ready: nothing was loaded"));
  auto* applyRefined = w.matchButton("APPLY REFINED BEST");
  REQUIRE(applyRefined != nullptr);
  CHECK(applyRefined->isVisible());
  CHECK(applyRefined->isEnabled());
  shot(*w.ed, "17_match_refined");
  click(*applyRefined);
  REQUIRE(proc.waitForLoader());
  screen.refresh();
  CHECK(proc.status().presetName == "refined best");
  CHECK(proc.currentPreset().a.levelDb == Catch::Approx(2.0));
  CHECK(anyLabelContains(screen, "Applied #1 (refined best)"));
}

void step18Export(Walk& w) {
  INFO("18 export");
  auto& proc = w.proc;
  ExportSettings s = proc.exportSettings();
  s.outputFolder = (w.root / "exports").string();
  proc.setExportSettings(s);
  w.load(writeExportRig(w.root, "Gatecreeper-style blend", "cc-by-nc"));
  w.tools.cfgExport({{"progressJson", true}, {"gates", json::array({"g1", "g2"})}, {"listen", "wav"}, {"nonCommercial", true}});

  // the play-along panel's own EXPORT NAM reaches the same panel as the top bar's (and closes the match screen)
  CHECK(w.ed->matchScreenOpen());
  click(*buttonTitled(w.panel(), "EXPORT NAM"));
  CHECK(w.ed->exportPanelOpen());
  CHECK_FALSE(w.ed->matchScreenOpen());
  click(*w.exportButton(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  CHECK_FALSE(w.ed->exportPanelOpen());

  auto* top = topBarButton(*w.ed, "EXPORT NAM");
  REQUIRE(top != nullptr);
  click(*top);
  REQUIRE(w.ed->exportPanelOpen());
  auto& panel = w.exportPanel();
  panel.refresh();
  CHECK(panel.view() == ExportPanel::View::Configure);
  // the mode default follows the cab mode: a shared cab is exact without the cab; the licences are listed
  CHECK(w.exportButton("NO CAB")->getToggleState());
  CHECK_FALSE(w.exportButton("WITH CAB")->getToggleState());
  CHECK(anyLabelContains(panel, "shared IR"));
  CHECK(anyLabelContains(panel, "HM-2w CHAINSAW - @ebheron (cc-by-nc)"));
  CHECK(anyLabelContains(panel, "V30 Mesa 4x12 - @OutmodedElectronics (t3k)"));
  CHECK(anyLabelContains(panel, "NON-COMMERCIAL"));
  CHECK(anyLabelContains(panel, "for your own use"));
  const juce::Image configure = shot(*w.ed, "18_export");
  CHECK(nonBackgroundFraction(configure, {0, 58, 1280, 742}) > 0.05);

  click(*w.exportButton("TRAIN EXPORT"));
  REQUIRE(waitUntilTrue([&] { return proc.jobs().snapshot(JobKind::Export).progress.epoch == 3; }));
  const fs::path run = proc.jobs().snapshot(JobKind::Export).outDir;
  fake_tools::release(run, "g1");
  REQUIRE(waitUntilTrue([&] { return proc.jobs().snapshot(JobKind::Export).progress.epoch == 7; }));
  std::this_thread::sleep_for(1100ms);
  panel.refresh();
  CHECK(panel.view() == ExportPanel::View::Training);
  shot(*w.ed, "18_export_training");
  fake_tools::release(run, "g2");
  REQUIRE(proc.jobs().waitFinished(JobKind::Export, 15000ms));
  panel.refresh();
  CHECK(panel.view() == ExportPanel::View::Result);
  CHECK(anyLabelContains(panel, "MET"));
  auto* reveal = w.exportButton("REVEAL");
  REQUIRE(reveal != nullptr);
  CHECK(reveal->isEnabled());
  CHECK(fs::exists(run));
  shot(*w.ed, "18_export_result");
  click(*w.exportButton(juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  CHECK_FALSE(w.ed->exportPanelOpen());
}

#ifdef SAWBLADE_WITH_SEPARATOR
// Synthetic htdemucs-shaped model with a sidecar (test_playalong.cpp).
void writeSynthModel(const fs::path& dir, const char* id, std::size_t sources) {
  fs::create_directories(dir);
  const std::vector<float> t(sources, 0.5f), f(sources, 0.0f);
  const std::string bytes = sawblade::test::onnx_synth::buildCoreModel(t, f);
  const fs::path p = dir / (std::string(id) + "-core-opset17.onnx");
  sawblade::test::onnx_synth::write(p, bytes);
  std::ofstream(p.string() + ".sha256") << sawblade::sha256Hex(bytes.data(), bytes.size()) << "\n";
}
#endif

void step19Separation(Walk& w) {
  INFO("19 separation");
#ifdef SAWBLADE_WITH_SEPARATOR
  using State = PlayAlong::LoadStatus::State;
  auto& pa = w.proc.playAlong();
  writeSynthModel(w.root / "models", "htdemucs_6s", 6);
  const fs::path song = w.root / "my song.wav";
  {
    const std::size_t n = 44100 * 4;
    std::vector<float> l(n), r(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / 44100.0;
      l[i] = static_cast<float>(0.3 * std::sin(6.283185307179586 * 196.0 * t));
      r[i] = static_cast<float>(0.3 * std::sin(6.283185307179586 * 247.0 * t));
    }
    writeWavFloat32Stereo(song, 44100.0, l, r);
  }
  pa.loadSong(song.string(), true);  // LOAD SONG with a file: separated once (the synthetic model), cached, loaded like a folder
  REQUIRE(pa.waitForLoader());
  auto st = pa.loadStatus();
  REQUIRE(st.state == State::Ready);
  CHECK(st.songName == "my song");
  CHECK_FALSE(st.cacheHit);
  CHECK(st.hasGuitarStem);
  processBlocks(w.proc, 4);
  w.ed->setPlayAlongOpen(true);
  w.panel().refresh();
  CHECK(anyLabelContains(w.panel(), "my song"));
  CHECK(activeSongName(w.proc) == "my song");  // never the cache directory's name

  // a second load hits the cache (the "pre-populated stem cache" of a restart)
  pa.loadSong(song.string(), true);
  REQUIRE(pa.waitForLoader());
  CHECK(pa.loadStatus().state == State::Ready);
  CHECK(pa.loadStatus().cacheHit);
  CHECK(pa.loadStatus().songName == "my song");

  if (w.proc.matchSettings().selectedTake().empty()) click(*buttonTitled(w.panel(), "USE FOR MATCH"));  // (step 16 chose it in a full walk)
  // MATCH and EXPORT plan from the song file's stems: the reference is the guitar stem inside the stems cache
  const MatchPlan plan = planMatch(w.proc);
  INFO(plan.message);
  CHECK(plan.ok);
  CHECK(plan.reference.found);
  CHECK(plan.request.referenceLabel.find("my song") == 0);
  CHECK(plan.request.ref.string().find((w.root / "stems").string()) == 0);
  CHECK(plan.offsetNote.find("another song") != std::string::npos);  // the take was recorded against the stems folder
  click(*topBarButton(*w.ed, "MATCH"));
  REQUIRE(w.ed->matchScreenOpen());
  w.screen().refresh();
  CHECK(anyLabelContains(w.screen(), "my song"));
  CHECK(anyLabelContains(w.screen(), "guitar stem"));
  CHECK(w.matchButton("START MATCH")->isEnabled());
  w.screen().close();
  shot(*w.ed, "19_separation");
#else
  SKIP("built without SAWBLADE_WITH_SEPARATOR");
#endif
}

void step20Overlays(Walk& w) {
  INFO("20 overlays");
  w.ed->setPlayAlongOpen(false);
  auto& ed = *w.ed;
  const auto show = [&](const char* what) {
    const auto o = w.overlays();
    INFO(what << ": rig " << o.rig << " mic " << o.mic << " browser " << o.browser << " match " << o.match << " export " << o.exportPanel);
    return o;
  };
  CHECK(show("closed").count() == 0);
  auto* rigBtn = buttonTitled(ed, "RIG");
  REQUIRE(rigBtn != nullptr);
  ed.setRigEditorOpen(true);
  CHECK(show("rig").rig);
  CHECK(rigBtn->getToggleState());
  // exactly one overlay at a time: each one closes the others, and the RIG button follows
  ed.setMicPageOpen(true);
  {
    const auto o = show("rig, then the mic page");
    CHECK(o.mic);
    CHECK_FALSE(o.rig);
    CHECK(o.count() == 1);
    CHECK_FALSE(rigBtn->getToggleState());
  }
  ed.setCabPageOpen(true);
  {
    const auto o = show("then the CAB page");
    CHECK(o.cab);
    CHECK_FALSE(o.mic);
    CHECK(o.count() == 1);
    CHECK(buttonTitled(ed, "Cab page")->getToggleState());
  }
  ed.setBrowserOpen(true);
  {
    const auto o = show("then the preset browser");
    CHECK(o.browser);
    CHECK_FALSE(o.cab);
    CHECK(o.count() == 1);
    CHECK_FALSE(buttonTitled(ed, "Cab page")->getToggleState());
  }
  ed.openMatchScreen();
  {
    const auto o = show("then MATCH");
    CHECK(o.match);
    CHECK(o.count() == 1);
  }
  ed.openExportPanel();
  CHECK(show("then EXPORT NAM").exportPanel);
  CHECK(w.overlays().count() == 1);
  ed.setRigEditorOpen(true);  // the RIG button's own click path: the rig replaces the export panel
  CHECK(show("then the rig again").rig);
  CHECK(w.overlays().count() == 1);
  CHECK(rigBtn->getToggleState());
  // the capture browser closes them all too, and "< RIG" returns to the rig screen with nothing open
  click(*buttonTitled(ed, "BROWSE CAPTURES"));
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(ed).size() == 1; }));
  CHECK(show("then the capture browser").count() == 0);
  CHECK_FALSE(rigBtn->getToggleState());
  click(*buttonTitled(*all<CaptureBrowser>(ed)[0], juce::String::fromUTF8("\xe2\x80\xb9 RIG")));
  REQUIRE(pumpUntil([&] { return all<CaptureBrowser>(ed).empty(); }));
  ed.setRigEditorOpen(true);
  ed.openMatchScreen();
  shot(ed, "20_overlays");
  w.screen().close();  // closing returns to the rig screen: nothing else is open
  CHECK(show("closed again").count() == 0);
  CHECK_FALSE(rigBtn->getToggleState());
}

void step21StateRoundtrip(Walk& w) {
  INFO("21 state_roundtrip");
  // The level-match demo blend: modeled pedals on both paths, levelMatch auto, constant-loudness law.
  w.load(fs::path(SAWBLADE_PRESETS_DIR) / "modeled" / "saw_body_blend_demo.json");
  w.setParam(kBlend, 0.37);
  w.setParam(kHmFirst + static_cast<int>(kHmDistortion), 6.5);
  w.settle();
  w.setParam(kSawCircuit, 1.0);  // BIG FUZZ: the loader swaps the first pedal block
  w.settle();
  const Preset before = w.proc.currentPreset();
  REQUIRE(before.levelMatch.mode == LevelMatchMode::Auto);
  REQUIRE(before.blendLaw == BlendLaw::ConstantLoudness);
  const double circuit = w.param(kSawCircuit);
  CHECK(circuit == 1.0);

  juce::MemoryBlock state;
  w.proc.getStateInformation(state);
  SawbladeProcessor other;
  other.prepareToPlay(48000.0, 512);
  other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  REQUIRE(other.waitForLoader(60000ms));
  REQUIRE(other.status().error.empty());
  const Preset after = other.currentPreset();
  CHECK(other.status().presetName == w.proc.status().presetName);
  CHECK(after.name == before.name);
  CHECK(after.blend == Catch::Approx(0.37).margin(1e-4));
  CHECK(after.levelMatch.mode == before.levelMatch.mode);
  CHECK(after.blendLaw == before.blendLaw);
  CHECK(static_cast<double>(other.parameters().getRawParameterValue(paramSpec(kSawCircuit).id)->load()) == circuit);
  for (int i = kSawCircuit; i < kNumParams; ++i)
    CHECK(static_cast<double>(other.parameters().getRawParameterValue(paramSpec(i).id)->load()) == Catch::Approx(w.param(i)).margin(1e-4));
  // UI state is not state: a fresh editor on the restored processor has every overlay closed
  std::unique_ptr<juce::AudioProcessorEditor> second(other.createEditor());
  auto* ed2 = dynamic_cast<SawbladeEditor*>(second.get());
  REQUIRE(ed2 != nullptr);
  CHECK_FALSE(ed2->rigEditorOpen());
  CHECK_FALSE(ed2->micPageOpen());
  CHECK_FALSE(ed2->browserOpen());
  CHECK_FALSE(ed2->matchScreenOpen());
  CHECK_FALSE(ed2->exportPanelOpen());
  CHECK_FALSE(ed2->playAlongOpen());
  CHECK_FALSE(ed2->settingsOpen());
  second.reset();
}

void step21bLevels(Walk& w) {
  INFO("21b rig_editor_blend_levels");
  w.load(writeLevelsRig(w.root));
  w.ed->setRigEditorOpen(true);
  auto& panel = w.rigPanel();
  panel.setTab(rig::RigEditorPanel::Tab::Blend);
  panel.refresh();
  CHECK(anyLabelContains(*w.ed, "0.0 dB off"));
  auto* match = buttonTitled(*w.ed, "MATCH LEVELS");
  REQUIRE(match != nullptr);
  CHECK(match->isEnabled());
  click(*match);
  w.settle();
  panel.refresh();
  const Preset p = w.proc.currentPreset();
  CHECK(p.levelMatch.mode == LevelMatchMode::Manual);
  CHECK(p.levelMatch.trimBDb == Catch::Approx(6.0).margin(0.1));
  CHECK(anyLabelContains(*w.ed, "+6.0 dB manual"));
  CHECK_FALSE(anyLabelContains(*w.ed, "0.0 dB off"));
  auto* linear = buttonTitled(*w.ed, "Blend law LINEAR");
  auto* constant = buttonTitled(*w.ed, "Blend law CONSTANT");
  REQUIRE(linear != nullptr);
  REQUIRE(constant != nullptr);
  const auto builds = w.proc.engineBuilds();
  click(*linear);
  CHECK(w.proc.currentPreset().blendLaw == BlendLaw::Linear);
  click(*constant);
  CHECK(w.proc.currentPreset().blendLaw == BlendLaw::ConstantLoudness);
  CHECK(w.proc.engineBuilds() == builds);  // live edits
  shot(*w.ed, "21b_rig_editor_blend_levels");
  w.ed->setRigEditorOpen(false);
}

void step21cSettings(Walk& w) {
  INFO("21c settings");
  auto* gear = buttonTitled(*w.ed, "Settings");
  REQUIRE(gear != nullptr);
  CHECK_FALSE(w.ed->settingsOpen());
  click(*gear);
  REQUIRE(w.ed->settingsOpen());
  CHECK(gear->getToggleState());
  // the tools row shows the venv, the Test buttons run the tools through it: t3k --help, then whoami through the fake
  CHECK(pumpUntil([&] { return anyLabelContains(*w.ed, "captures on disk") || anyLabelContains(*w.ed, "Setup:"); }, 3000));
  std::vector<juce::Button*> tests;
  for (auto* b : all<juce::Button>(*w.ed))
    if (b->getTitle() == "Test") tests.push_back(b);
  REQUIRE(tests.size() == 2);
  click(*tests[0]);
  CHECK(pumpUntil([&] { return anyLabelContains(*w.ed, "ok (exit 0,"); }, 10000));
  click(*tests[1]);
  CHECK(pumpUntil([&] { return anyLabelContains(*w.ed, "Logged in as @tester (Test User)"); }, 10000));
  bool pathShown = false;
  for (auto* e : all<juce::TextEditor>(*w.ed))
    if (e->getText().contains(w.tools.root.string())) pathShown = true;
  CHECK(pathShown);
  shot(*w.ed, "21c_settings");

  auto* about = buttonTitled(*w.ed, "About Sawblade...");
  REQUIRE(about != nullptr);
  click(*about);
  REQUIRE(w.ed->aboutOpen());
  CHECK(anyLabelContains(*w.ed, juce::String("Sawblade ") + about::kVersion));
  CHECK(anyLabelContains(*w.ed, "Sawblade is not sold"));
  shot(*w.ed, "21c_about");
  click(*buttonTitled(*w.ed, "CLOSE"));
  CHECK_FALSE(w.ed->aboutOpen());
  pump(80);
  click(*gear);
  CHECK_FALSE(w.ed->settingsOpen());
}

void runStep(Step s) {
  if (gDone[s]) return;
  for (Step n : needs(s)) runStep(n);
  gDone[s] = true;
  Walk& w = walk();
  switch (s) {
    case kMain: step01Main(w); break;
    case kPresetBrowser: step02PresetBrowser(w); break;
    case kAbCompare: step03AbCompare(w); break;
    case kRigEditor: step04to09RigEditor(w); break;
    case kPedal: step10PedalFace(w); break;
    case kDrawer: step10PedalFace(w); step11PedalDrawer(w); break;
    case kCaptureBrowser: step12CaptureBrowser(w); break;
    case kMicPage: step13MicPage(w); break;
    case kPlayAlong: step14PlayAlongOpen(w); break;
    case kRecord: step15Record(w); break;
    case kMatchProgress: step16MatchProgress(w); break;
    case kMatchResults: step17MatchResults(w); break;
    case kExport: step18Export(w); break;
    case kSeparation: step19Separation(w); break;
    case kOverlays: step20Overlays(w); break;
    case kStateRoundtrip: step21StateRoundtrip(w); break;
    case kLevels: step21bLevels(w); break;
    case kSettings: step21cSettings(w); break;
    case kNumSteps: break;
  }
}

}  // namespace

CATCH_REGISTER_LISTENER(WalkReleaser)

TEST_CASE("01 main", "[integration]") { runStep(kMain); }
TEST_CASE("02 preset browser", "[integration]") { runStep(kPresetBrowser); }
TEST_CASE("03 A/B compare", "[integration]") { runStep(kAbCompare); }
TEST_CASE("04-09 rig editor tabs", "[integration]") { runStep(kRigEditor); }
TEST_CASE("10 pedal face and the CIRCUIT switch", "[integration]") { runStep(kPedal); }
TEST_CASE("11 pedal drawer", "[integration]") { runStep(kDrawer); }
TEST_CASE("12 capture browser", "[integration]") { runStep(kCaptureBrowser); }
TEST_CASE("13 mic page", "[integration]") { runStep(kMicPage); }
TEST_CASE("14 play-along", "[integration]") { runStep(kPlayAlong); }
TEST_CASE("15 record", "[integration]") { runStep(kRecord); }
TEST_CASE("16 match progress", "[integration]") { runStep(kMatchProgress); }
TEST_CASE("17 match results", "[integration]") { runStep(kMatchResults); }
TEST_CASE("18 export", "[integration]") { runStep(kExport); }
TEST_CASE("19 separation from a song file", "[integration]") { runStep(kSeparation); }
TEST_CASE("20 overlays", "[integration]") { runStep(kOverlays); }
TEST_CASE("21 state round trip", "[integration]") { runStep(kStateRoundtrip); }
TEST_CASE("21b blend levels", "[integration]") { runStep(kLevels); }
TEST_CASE("21c settings and About", "[integration]") { runStep(kSettings); }

// B2b: the tool path is set once, in the Settings panel's match venv. The older stores (browser.settings, settings.json
// t3kExecutable, the match settings file) hold no path of their own and read that value as their default, so the capture
// browser, the mic page / preset browser (T3kTool), MATCH and EXPORT all follow it; an explicit override in an older store
// still wins and clearing it falls back again.
TEST_CASE("one tool path in Settings reaches the capture browser, the mic page, MATCH and EXPORT", "[integration]") {
  Walk& w = walk();
  auto& st = settings::Settings::shared();
  auto sees = [&](const fs::path& dir) {
    const fs::path b = dir / "bin";
    CHECK(BrowserSettings::defaultExecutable() == (b / "sawblade-t3k").string());
    BrowserSettings browser;  // the real file under the temp HOME: no override stored
    CHECK(browser.executable() == (b / "sawblade-t3k").string());
    CHECK(settings::t3kExecutable() == b / "sawblade-t3k");  // the mic page and the preset browser (T3kTool)
    CHECK(w.proc.matchSettings().matchExecutable() == b / "sawblade-match");
    CHECK(w.proc.matchSettings().exportExecutable() == b / "sawblade-export");
  };
  sees(w.tools.root);

  // the user points Settings at another venv: everything follows at once
  const fs::path other = w.root / "other_venv";
  fs::create_directories(other / "bin");
  REQUIRE(st.setMatchVenvDir(other).ok);
  sees(other);

  // an explicit override in an older store wins, and resetting it returns to the Settings value
  {
    BrowserSettings browser;
    browser.setExecutable("/opt/custom/sawblade-t3k");
    CHECK(browser.executable() == "/opt/custom/sawblade-t3k");
    browser.reset();
    CHECK(browser.executable() == (other / "bin" / "sawblade-t3k").string());
  }
  REQUIRE(settings::setT3kExecutable("/opt/custom2/sawblade-t3k"));
  CHECK(settings::t3kExecutable() == "/opt/custom2/sawblade-t3k");
  CHECK(st.matchVenvDir() == other);  // writing the older key kept the Settings value
  REQUIRE(st.setMatchVenvDir(w.tools.root).ok);  // the other store's key is in the same file and survived the Settings write
  CHECK(settings::t3kExecutable() == "/opt/custom2/sawblade-t3k");
  REQUIRE(settings::setT3kExecutable(settings::defaultT3kExecutable()));
}

TEST_CASE("21c first run on a fresh app-data dir", "[integration]") {
  gWalk.reset();  // the walk's settings must not outlive this test's own
  SettingsEnv env(nullptr, /*isolateHome=*/true);  // no settings file: a first run
  EnvVar appData("SAWBLADE_APPDATA", (env.dir / "appdata").string());
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  proc.playAlong().setStandalone(true);
  proc.prepareToPlay(48000.0, 512);
  std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditorAndMakeActive());
  auto* ed = dynamic_cast<SawbladeEditor*>(base.get());
  REQUIRE(ed != nullptr);
  ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  const fs::path file = env.dir / "settings.json";
  CHECK_FALSE(fs::exists(file));
  CHECK(ed->settingsOpen());  // the checklist opens by itself
  int rows = 0;
  for (auto* l : all<juce::Label>(*ed))
    if (l->isVisible() && (l->getTitle() == "Tools found" || l->getTitle() == "Logged in" || l->getTitle() == "Captures cached")) ++rows;
  CHECK(rows == 3);
  shot(*ed, "21c_firstrun");
  auto* done = buttonTitled(*ed, "DONE");
  REQUIRE(done != nullptr);
  click(*done);
  CHECK_FALSE(ed->settingsOpen());
  REQUIRE(fs::exists(file));
  CHECK(json::parse(std::ifstream(file))["firstRunCompleted"] == true);
  base.reset();
}

// ---- v0.1.1 Task A: the launch "file not found" error (docs/specs/v0_1_1-init_classic.md) ------------------------------------------
namespace {

// Every piece of text a panel can show about a file: labels, button texts and text fields (hidden overlays included: their
// children keep their own visible flag, so closed panels are covered too).
std::vector<juce::String> allShownTexts(juce::Component& root) {
  std::vector<juce::String> t;
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible()) t.push_back(l->getText());
  for (auto* b : all<juce::Button>(root))
    if (b->isVisible()) t.push_back(b->getButtonText());
  for (auto* e : all<juce::TextEditor>(root))
    if (e->isVisible()) t.push_back(e->getText());
  return t;
}

// Text that reports a missing / unreadable file, or shows the INIT cab sentinel as if it were a file.
bool reportsMissingFile(const juce::String& s) {
  for (const char* phrase : {"(none)", "file not found", "file missing", "captures missing", "No such file", "cannot read the ir", "not a valid wav", "cannot open"})
    if (s.containsIgnoreCase(phrase)) return true;
  return false;
}

// `allowedName` empty: no text may report a missing file. Otherwise such text is allowed only when it names that file.
void expectNoMissingFileText(juce::Component& root, const std::string& where, const juce::String& allowedName = {}) {
  for (const auto& s : allShownTexts(root)) {
    if (!reportsMissingFile(s)) continue;
    if (allowedName.isNotEmpty() && s.contains(allowedName)) continue;
    INFO(where << ": \"" << s.toStdString() << "\"");
    CHECK_FALSE(reportsMissingFile(s));
  }
}

}  // namespace

TEST_CASE("launch: a clean first Standalone run shows no missing-file or (none) text on any panel", "[integration][launch]") {
  gWalk.reset();
  SettingsEnv env(nullptr, /*isolateHome=*/true);  // no settings file: a first run
  EnvVar appData("SAWBLADE_APPDATA", (env.dir / "appdata").string());
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;  // no saved state: the processor stays on the Init preset
  proc.playAlong().setStandalone(true);
  proc.prepareToPlay(48000.0, 512);
  REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
  std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditorAndMakeActive());
  auto* ed = dynamic_cast<SawbladeEditor*>(base.get());
  REQUIRE(ed != nullptr);
  ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  pump(250);  // the editor's timer refreshes the status

  CHECK(proc.status().presetName == "Init");
  CHECK(proc.status().error.empty());
  expectNoMissingFileText(*ed, "editor at launch");

  // Settings (opens by itself on a first run): the checklist's captures row, then the About box's capture rows.
  REQUIRE(ed->settingsOpen());
  auto panels = all<settings::SettingsPanel>(*ed);
  REQUIRE(panels.size() == 1);
  panels[0]->refresh();
  CHECK(panels[0]->checklistLight(2) != 3);  // 3 = Bad (see the editor tests): Init has no captures to be missing
  expectNoMissingFileText(*ed, "settings checklist");
  auto* about = buttonTitled(*ed, "About Sawblade...");
  REQUIRE(about != nullptr);
  click(*about);
  REQUIRE(ed->aboutOpen());
  expectNoMissingFileText(*ed, "About box");
  click(*buttonTitled(*ed, "CLOSE"));
  pump(80);

  // Rig editor: every tab (the status line, the cab tab's file field).
  ed->setRigEditorOpen(true);
  for (auto t : {rig::RigEditorPanel::Tab::Chain, rig::RigEditorPanel::Tab::Eq, rig::RigEditorPanel::Tab::Blend, rig::RigEditorPanel::Tab::Cab,
                 rig::RigEditorPanel::Tab::Gate, rig::RigEditorPanel::Tab::Comp}) {
    ed->rigEditor().setTab(t);
    ed->rigEditor().refresh();
    INFO("rig tab " << static_cast<int>(t));
    CHECK_FALSE(reportsMissingFile(ed->rigEditor().statusText()));
    expectNoMissingFileText(*ed, "rig editor tab " + std::to_string(static_cast<int>(t)));
  }
  ed->setRigEditorOpen(false);

  // Cab mic page.
  ed->setMicPageOpen(true);
  pump(250);
  expectNoMissingFileText(*ed, "cab mic page");
  ed->setMicPageOpen(false);

  // Preset browser and its info panel (nothing is selected on a clean launch: Init is not a library entry).
  ed->setBrowserOpen(true);
  REQUIRE(pumpUntil([&] { return !ed->browser().scanning(); }));
  pump(100);
  CHECK(ed->browser().message().isEmpty());
  CHECK_FALSE(reportsMissingFile(ed->browser().infoPanel().plainText()));
  expectNoMissingFileText(*ed, "preset browser");
  ed->setBrowserOpen(false);

  CHECK(proc.status().error.empty());  // opening the panels did not start a load that failed
  base.reset();
}

TEST_CASE("launch: a saved state whose captures no longer exist restores; only the load failure names the file", "[integration][launch]") {
  gWalk.reset();
  SettingsEnv env(kSettingsExist, /*isolateHome=*/true);
  EnvVar appData("SAWBLADE_APPDATA", (env.dir / "appdata").string());
  juce::ScopedJuceInitialiser_GUI gui;
  const fs::path gone = env.dir / "moved_away";  // never created: the folder the captures used to live in
  json state = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "Gone"},
                {"paths", {{"a", {{"role", "saw"}, {"blocks", json::array({{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"},
                                                                          {"model", {{"file", (gone / "amp.nam").string()}}}}})}}},
                           {"b", {{"role", "body"}, {"blocks", json::array({{{"id", "b1"}, {"type", "nam"}, {"slot", "amp"},
                                                                          {"model", {{"file", (gone / "amp2.nam").string()}}}}})}}}}},
                {"align", {{"mode", "off"}}},
                {"blend", 0.5},
                {"cab", {{"mode", "shared"}, {"ir", {{"file", (gone / "cab.wav").string()}}}}}};
  const std::string text = state.dump(2);

  SawbladeProcessor proc;
  proc.playAlong().setStandalone(true);
  proc.prepareToPlay(48000.0, 512);
  proc.setStateInformation(text.data(), static_cast<int>(text.size()));  // never throws
  REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(proc.currentPreset().name == "Gone");  // restored like a host session restore, even though it cannot be built
  CHECK_FALSE(proc.status().error.empty());
  CHECK(juce::String(proc.status().error).contains("amp.nam"));  // the failure names the file

  std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditorAndMakeActive());
  auto* ed = dynamic_cast<SawbladeEditor*>(base.get());
  REQUIRE(ed != nullptr);
  ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  pump(250);
  CHECK(anyLabelContains(*ed, "amp.nam"));  // the editor's message line shows the load failure

  // No other panel reports the missing file, except by naming it (the rig status line shows the same failure).
  expectNoMissingFileText(*ed, "editor", "amp.nam");
  ed->setRigEditorOpen(true);
  ed->rigEditor().refresh();
  expectNoMissingFileText(*ed, "rig editor", "amp.nam");
  ed->setRigEditorOpen(false);
  ed->setBrowserOpen(true);
  REQUIRE(pumpUntil([&] { return !ed->browser().scanning(); }));
  pump(100);
  CHECK(ed->browser().message().isEmpty());
  CHECK_FALSE(reportsMissingFile(ed->browser().infoPanel().plainText()));
  ed->setBrowserOpen(false);
  base.reset();
}

TEST_CASE("launch: a state saved by an older build with the absolutised \"(none)\" cab file shows no cab and no error", "[integration][launch]") {
  gWalk.reset();
  SettingsEnv env(kSettingsExist, /*isolateHome=*/true);
  EnvVar appData("SAWBLADE_APPDATA", (env.dir / "appdata").string());
  juce::ScopedJuceInitialiser_GUI gui;
  json state = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "Init"},
                {"paths", {{"a", {{"blocks", json::array()}}}, {"b", {{"blocks", json::array()}}}}},
                {"align", {{"mode", "off"}}},
                {"blend", 0.5},
                {"cab", {{"enabled", false}, {"mode", "shared"}, {"ir", {{"file", (env.dir / "(none)").string()}}}}}};
  const std::string text = state.dump(2);
  SawbladeProcessor proc;
  proc.playAlong().setStandalone(true);
  proc.prepareToPlay(48000.0, 512);
  proc.setStateInformation(text.data(), static_cast<int>(text.size()));
  REQUIRE(proc.waitForLoader(std::chrono::milliseconds(60000)));
  CHECK(proc.status().error.empty());
  CHECK(proc.currentPreset().cab.ir.file == "(none)");
  CHECK(exportBlockedReason(proc.currentPreset()).empty());

  std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditorAndMakeActive());
  auto* ed = dynamic_cast<SawbladeEditor*>(base.get());
  REQUIRE(ed != nullptr);
  ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  pump(250);
  expectNoMissingFileText(*ed, "editor");
  ed->setSettingsOpen(true);
  auto* about = buttonTitled(*ed, "About Sawblade...");
  REQUIRE(about != nullptr);
  click(*about);
  REQUIRE(ed->aboutOpen());
  expectNoMissingFileText(*ed, "About box");
  click(*buttonTitled(*ed, "CLOSE"));
  pump(80);
  ed->setSettingsOpen(false);
  ed->setRigEditorOpen(true);
  ed->rigEditor().setTab(rig::RigEditorPanel::Tab::Cab);
  ed->rigEditor().refresh();
  expectNoMissingFileText(*ed, "rig Cab tab");
  ed->setRigEditorOpen(false);
  ed->setMicPageOpen(true);
  pump(250);
  expectNoMissingFileText(*ed, "cab mic page");
  ed->setMicPageOpen(false);
  base.reset();

  juce::MemoryBlock out;
  proc.getStateInformation(out);
  const json saved = json::parse(std::string(static_cast<const char*>(out.getData()), out.getSize()));
  CHECK(saved["cab"]["ir"]["file"] == "(none)");  // written back verbatim, not absolutised
}
