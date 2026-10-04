// Editor tests of the cab mic page (docs/specs/phase9a_mic_page.md section 5): opening from the rig, dots, dragging, BLEND, the MIX
// fader, the studio (perPath) note, LOAD PACK through a fake sawblade-t3k, and the screenshots. Needs a display (xvfb-run).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "mic/MicPage.h"
#include "skin/RigView.h"
#include "sawblade/wav_io.h"

using namespace sawblade::plugin;
using nlohmann::json;
namespace fs = std::filesystem;

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

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, int clicks = 1) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos, now, clicks, true);
}

void click(juce::Button& b) {
  juce::Component& c = b;
  const auto centre = b.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(b, centre, centre));
  c.mouseUp(mouse(b, centre, centre));
}

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

struct TempDir {
  fs::path dir;
  TempDir() {
    dir = fs::temp_directory_path() / ("sawblade_micpage_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffffff));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// Synthetic cab IR: a one-pole low-pass (cutoff `fc`) with a presence bump (`gain` at `bumpHz`), never a capture.
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

// A pack in the shape of a real one: six dots on two drivers (merged models share a dot), 14 IRs.
const Model kModels[] = {
    {"V30 UL SM57 Cap 0.5in", 6500, 3500, 0.8},     {"V30 UL SM57 Cap 1in", 5200, 3200, 0.7},      {"V30 UL MD421 Cap 1in", 4300, 2800, 0.5},
    {"V30 UL SM57 CapEdge 1in", 4200, 2600, 0.6},   {"V30 UL SM57 CapEdge 2in", 3600, 2300, 0.5}, {"V30 UL SM57 Cone 1in", 3000, 2000, 0.4},
    {"V30 UL R121 Cone 2in", 2400, 1800, 0.3},      {"V30 UL SM57 Edge 1in", 2200, 1500, 0.4},     {"V30 UR SM57 Cap 1in", 5600, 3400, 0.7},
    {"V30 UR SM57 CapEdge 1in", 4400, 2700, 0.6},   {"V30 UR SM57 Cone 1in", 3100, 2100, 0.4},     {"V30 UR SM57 off axis 1in", 2700, 1900, 0.3},
    {"V30 UR SM57 Edge 1in", 2300, 1400, 0.4},      {"V30 UR MD421 Edge 2in", 1900, 1200, 0.3},
};
constexpr int kNumModels = static_cast<int>(sizeof(kModels) / sizeof(kModels[0]));
constexpr int kNumDots = 7;  // UL: Cap, CapEdge, Cone, Edge; UR: Cap, CapEdge, Cone(+OffAxis), Edge  ->  8? see below

// A manifest (as `sawblade-t3k pack` writes it) and its IR files under `dir`.
fs::path writePack(const fs::path& dir, const std::string& toneId, const std::string& title) {
  fs::create_directories(dir / "irs");
  json models = json::array();
  int i = 0;
  for (const Model& m : kModels) {
    const fs::path f = dir / "irs" / (std::to_string(i) + ".wav");
    sawblade::writeWavFloat32(f, 48000.0, syntheticIr(m.fc, m.bumpHz, m.gain));
    models.push_back({{"modelId", std::to_string(1000 + i)}, {"name", m.name}, {"file", f.string()}, {"sha256", ""}});
    ++i;
  }
  const fs::path manifest = dir / (toneId + ".json");
  std::ofstream(manifest) << json{{"toneId", toneId}, {"title", title}, {"creator", "testpack"}, {"license", "cc-by"},
                                  {"url", "https://example.org/tones/" + toneId}, {"models", models}}.dump(2);
  return manifest;
}

// The processor + editor. The cab uses a synthetic IR from the pack (tone 321, source attached), so the page can offer LOAD PACK.
struct Fixture {
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;
  TempDir tmp;
  fs::path manifest;

  Fixture() {
    manifest = writePack(tmp.dir, "321", "Synthetic 4x12 test pack");
    const json models = json::parse(std::ifstream(manifest))["models"];
    const std::string nam = (fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / "linear_identity.nam").string();
    auto block = [&](const char* id) { return json{{"id", id}, {"type", "nam"}, {"model", {{"file", nam}}}}; };
    json cabIr = {{"file", models[1]["file"]}, {"source", {{"provider", "tone3000"}, {"id", "321"}, {"modelId", "1001"}, {"title", models[1]["name"]}, {"creator", "testpack"}, {"license", "cc-by"}}}};
    const json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", "Mic test"},
                    {"paths", {{"a", {{"blocks", json::array({block("a1")})}}}, {"b", {{"blocks", json::array({block("b1")})}}}}},
                    {"align", {{"mode", "off"}}}, {"blend", 0.5}, {"cab", {{"mode", "shared"}, {"ir", cabIr}}}};
    const fs::path pf = tmp.dir / "preset.json";
    std::ofstream(pf) << j.dump(2);
    REQUIRE(proc.loadPresetFile(pf));
    proc.prepareToPlay(48000.0, 512);
    REQUIRE(proc.waitForLoader());
    REQUIRE(proc.status().error.empty());
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Fixture() { base.reset(); }

  MicPage& page() { return ed->micPage(); }
  skin::RigPiece& cabPiece() {
    for (auto* p : all<skin::RigPiece>(*ed))
      if (p->piece() == skin::Piece::Cab) return *p;
    FAIL("no cab piece");
    std::abort();
  }
  void openByDoubleClick() {
    auto& c = cabPiece();
    c.mouseDoubleClick(mouse(c, {20.0f, 20.0f}, {20.0f, 20.0f}, 2));
  }
  void wait() { REQUIRE(proc.waitForLoader()); }
  void loadPackFromCache() {
    page().session().setPack(sawblade::plugin::mic::IrPack::fromManifestFile(manifest));
    page().refresh();
  }
  // Drags mic `m` onto dot `d` with real mouse events on the stage.
  void dragTo(int m, int d) {
    auto& st = page().stage();
    const auto from = page().micCentre(m).isOrigin() ? page().dotCentre(0) : page().micCentre(m);  // not placed yet: from the first dot
    const auto fromLocal = from - st.getPosition().toFloat();
    const auto toLocal = page().dotCentre(d) + juce::Point<float>(4.0f, -3.0f) - st.getPosition().toFloat();
    st.mouseDown(mouse(st, fromLocal, fromLocal));
    st.mouseDrag(mouse(st, (fromLocal + toLocal) * 0.5f, fromLocal));
    st.mouseDrag(mouse(st, toLocal, fromLocal));
    st.mouseUp(mouse(st, toLocal, fromLocal));
  }
  juce::Image snapshot() { return ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f); }
};

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

bool anyLabelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}

void setEnv(const char* k, const std::string& v) { ::setenv(k, v.c_str(), 1); }

fs::path writeScript(const fs::path& dir, const std::string& name, const std::string& body) {
  const fs::path p = dir / name;
  std::ofstream(p) << "#!/bin/sh\n" << body << "\n";
  fs::permissions(p, fs::perms::owner_all);
  return p;
}

}  // namespace

TEST_CASE("mic page: double-clicking the cab opens it, the dots match the pack, '< RIG' closes it", "[editor][mic]") {
  Fixture f;
  CHECK_FALSE(f.ed->micPageOpen());
  CHECK_FALSE(f.page().isVisible());
  f.openByDoubleClick();
  REQUIRE(f.ed->micPageOpen());
  REQUIRE(f.page().isVisible());
  // opened on the cab's IR: a single IR, one dot (the cab's tone has no cached manifest here)
  CHECK(f.page().dotCount() == 1);
  CHECK(f.page().buttonTitled("LOAD PACK") != nullptr);
  CHECK(f.page().buttonTitled("LOAD PACK")->isVisible());
  CHECK(f.page().cabImageName() == "4x12");

  f.loadPackFromCache();
  // 14 IRs: UL cap {0,1,2} / capedge {3,4} / cone {5,6} / edge {7}; UR cap {8} / capedge {9} / cone+off axis {10,11} / edge {12,13}
  CHECK(f.page().dotCount() == 8);
  CHECK(f.page().session().pack().size() == kNumModels);
  CHECK(f.page().session().model(0) == 1);  // the cab's IR is model 1 of the pack
  CHECK(f.page().responseShown());
  (void)kNumDots;

  auto* back = f.page().buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 RIG"));
  REQUIRE(back != nullptr);
  click(*back);
  CHECK_FALSE(f.ed->micPageOpen());
}

TEST_CASE("mic page: dragging the mic snaps on release and loads that IR; nothing loads while dragging", "[editor][mic]") {
  Fixture f;
  f.openByDoubleClick();
  f.loadPackFromCache();
  const auto before = f.proc.currentPreset().cab.ir;
  const auto builds = f.proc.engineBuilds();
  auto& st = f.page().stage();

  // mouse down + drag: the nearest dot is highlighted, nothing is loaded
  const auto micLocal = f.page().micCentre(0) - st.getPosition().toFloat();
  const auto target = f.page().dotCentre(5) - st.getPosition().toFloat();  // UR capedge
  st.mouseDown(mouse(st, micLocal, micLocal));
  st.mouseDrag(mouse(st, target, micLocal));
  CHECK(f.proc.currentPreset().cab.ir == before);
  f.wait();
  CHECK(f.proc.engineBuilds() == builds);
  // mouse up: snap + load through the processor
  st.mouseUp(mouse(st, target, micLocal));
  f.wait();
  const auto after = f.proc.currentPreset().cab.ir;
  CHECK_FALSE(after == before);
  const auto& pack = f.page().session().pack();
  const int dot = pack.dotOfModel(pack.findModel(after));
  CHECK(dot == 5);
  CHECK(f.proc.engineBuilds() == builds + 1);
  // it kept the current mic (SM57) and the nearest distance (1 in)
  const auto& shot = pack.models()[static_cast<std::size_t>(pack.findModel(after))].shot;
  CHECK(shot.mic == "SM57");
  CHECK(shot.distanceIn == 1.0);
  REQUIRE(after.source);
  CHECK(after.source->id == "321");
  CHECK(after.source->creator == "testpack");

  // a click without moving also snaps (to the nearest dot)
  const auto p2 = f.page().dotCentre(2) - st.getPosition().toFloat();
  st.mouseDown(mouse(st, p2 + juce::Point<float>(3.0f, 2.0f), p2));
  st.mouseUp(mouse(st, p2 + juce::Point<float>(3.0f, 2.0f), p2));
  f.wait();
  CHECK(pack.dotOfModel(pack.findModel(f.proc.currentPreset().cab.ir)) == 2);

  // NEXT POSITION and A/B
  auto* next = f.page().buttonTitled("NEXT POSITION");
  auto* ab = f.page().buttonTitled("A / B");
  REQUIRE(next != nullptr);
  REQUIRE(ab != nullptr);
  click(*next);
  f.wait();
  CHECK(pack.dotOfModel(pack.findModel(f.proc.currentPreset().cab.ir)) == 3);
  REQUIRE(ab->isEnabled());
  click(*ab);
  f.wait();
  CHECK(pack.dotOfModel(pack.findModel(f.proc.currentPreset().cab.ir)) == 2);
}

TEST_CASE("mic page: the fields, and combo boxes when a dot has several mics or distances", "[editor][mic]") {
  Fixture f;
  f.openByDoubleClick();
  f.loadPackFromCache();
  f.dragTo(0, 0);  // UL cap: SM57 0.5 in / SM57 1 in / MD421 1 in
  f.wait();
  f.page().refresh();
  auto combos = all<juce::ComboBox>(f.page());
  REQUIRE(combos.size() == 2);
  juce::ComboBox* mic = combos[0]->getTitle() == "Mic model" ? combos[0] : combos[1];
  juce::ComboBox* dist = mic == combos[0] ? combos[1] : combos[0];
  CHECK(mic->isVisible());
  CHECK(dist->isVisible());
  CHECK(mic->getNumItems() == 2);   // SM57, MD421
  CHECK(dist->getNumItems() == 2);  // 0.5 in, 1 in
  CHECK(anyLabelContains(f.page(), "UPPER-LEFT"));
  CHECK(anyLabelContains(f.page(), "CAP"));
  // pick MD421: the cab now uses the MD421 shot of this dot
  for (int i = 0; i < mic->getNumItems(); ++i)
    if (mic->getItemText(i).contains("MD421")) mic->setSelectedItemIndex(i, juce::sendNotificationSync);
  f.wait();
  const auto& pack = f.page().session().pack();
  CHECK(pack.models()[static_cast<std::size_t>(pack.findModel(f.proc.currentPreset().cab.ir))].shot.mic == "MD421");
  // a dot with one model shows plain values
  f.dragTo(0, 3);  // UL edge: only SM57 1 in
  f.wait();
  f.page().refresh();
  CHECK_FALSE(mic->isVisible());
  CHECK_FALSE(dist->isVisible());
  CHECK(anyLabelContains(f.page(), "EDGE"));
}

TEST_CASE("mic page: BLEND 2 MICS, the second mic, the MIX fader and BLEND off", "[editor][mic]") {
  Fixture f;
  f.openByDoubleClick();
  f.loadPackFromCache();
  auto* blend = f.page().buttonTitled("BLEND 2 MICS");
  REQUIRE(blend != nullptr);
  CHECK_FALSE(f.page().mixSlider().isVisible());
  const auto irBefore = f.proc.currentPreset().cab.ir;

  click(*blend);
  f.wait();
  auto p = f.proc.currentPreset();
  REQUIRE(p.cab.mode == sawblade::CabMode::IrMix);
  CHECK(p.cab.irA == irBefore);
  CHECK(p.cab.irB == irBefore);  // the sound does not change
  CHECK(p.cab.mix == 0.5);
  CHECK(f.proc.status().liveCompatible);  // the chip says LIVE
  CHECK(f.page().mixSlider().isVisible());
  CHECK(f.page().mixSlider().getValue() == 50.0);

  // the second mic is draggable
  f.dragTo(1, 6);
  f.wait();
  p = f.proc.currentPreset();
  CHECK(p.cab.irA == irBefore);
  CHECK_FALSE(p.cab.irB == irBefore);
  CHECK(f.page().session().active() == 1);

  // the MIX fader: a flurry of changes ends with the last value (at most one submit per 150 ms while dragging, and on release)
  auto& slider = f.page().mixSlider();
  for (int v : {10, 20, 30, 40, 55, 70, 80}) slider.setValue(v, juce::sendNotificationSync);
  CHECK(slider.getValue() == 80.0);
  pump(400);
  f.wait();
  CHECK(f.proc.currentPreset().cab.mix == 0.8);
  slider.setValue(25.0, juce::sendNotificationSync);
  if (slider.onDragEnd) slider.onDragEnd();  // release: submits at once
  f.wait();
  CHECK(f.proc.currentPreset().cab.mix == 0.25);

  // back to one mic: shared with irA
  click(*blend);
  f.wait();
  p = f.proc.currentPreset();
  REQUIRE(p.cab.mode == sawblade::CabMode::Shared);
  CHECK(p.cab.ir == irBefore);
  CHECK_FALSE(f.page().mixSlider().isVisible());
}

TEST_CASE("mic page: an irMix preset opens with BLEND on; perPath is read-only", "[editor][mic]") {
  Fixture f;
  auto preset = f.proc.currentPreset();
  preset.cab.mode = sawblade::CabMode::IrMix;
  preset.cab.irA = preset.cab.ir;
  preset.cab.irB = preset.cab.ir;
  preset.cab.mix = 0.7;
  preset.cab.ir = sawblade::Capture{};
  f.proc.loadPreset(preset);
  f.wait();
  f.openByDoubleClick();
  CHECK(f.page().session().blend());
  CHECK(f.page().mixSlider().isVisible());
  CHECK(f.page().mixSlider().getValue() == 70.0);

  preset.cab.mode = sawblade::CabMode::PerPath;
  f.proc.loadPreset(preset);
  f.wait();
  f.page().refresh();  // follows the processor
  CHECK(f.page().session().readOnly());
  CHECK(anyLabelContains(f.page(), "studio blend: per-path cabs"));
  CHECK_FALSE(f.page().buttonTitled("BLEND 2 MICS")->isEnabled());
  // dragging does nothing
  f.loadPackFromCache();
  const auto before = f.proc.currentPreset().cab;
  f.dragTo(0, 4);
  f.wait();
  CHECK(sawblade::plugin::mic::sameCab(f.proc.currentPreset().cab, before));
}

TEST_CASE("mic page: LOAD IR FOLDER shows the folder as the pack", "[editor][mic]") {
  Fixture f;
  f.openByDoubleClick();
  const fs::path dir = f.tmp.dir / "my cab irs";
  fs::create_directories(dir);
  for (const char* n : {"Greenback 2x12 e906 25mm cap", "Greenback 2x12 e906 25mm edge", "Greenback 2x12 SM57 1in cone"})
    sawblade::writeWavFloat32(dir / (std::string(n) + ".wav"), 48000.0, syntheticIr(4000, 2500, 0.5));
  CHECK_FALSE(f.page().loadFolder(f.tmp.dir / "nothing here"));
  CHECK(f.page().statusText().isNotEmpty());
  REQUIRE(f.page().loadFolder(dir));
  CHECK(f.page().dotCount() == 3);
  CHECK(f.page().cabImageName() == "2x12");  // the majority of the parsed cab sizes
  CHECK(anyLabelContains(f.page(), "my cab irs"));
  CHECK(anyLabelContains(f.page(), "LOCAL FOLDER"));
}

TEST_CASE("mic page: LOAD PACK runs sawblade-t3k, shows progress, caches the manifest and reuses it", "[editor][mic][t3k]") {
  Fixture f;
  TempDir data;
  setEnv("SAWBLADE_APPDATA", data.dir.string());
  // fake tool: copies the prepared manifest to the -o path after a few progress lines
  const fs::path exe = writeScript(data.dir, "fake-t3k",
                                   "echo '{\"done\": 1, \"total\": 2, \"name\": \"a\"}'\n"
                                   "sleep 0.2\n"
                                   "echo '{\"done\": 2, \"total\": 2, \"name\": \"b\"}'\n"
                                   "cp \"" + f.manifest.string() + "\" \"$4\"");
  std::ofstream(data.dir / "settings.json") << json{{"t3kExecutable", exe.string()}, {"other", 1}}.dump();
  f.openByDoubleClick();
  auto* load = f.page().buttonTitled("LOAD PACK");
  REQUIRE(load != nullptr);
  CHECK(load->isVisible());
  click(*load);
  CHECK(f.page().packLoading());
  CHECK_FALSE(load->isVisible());
  for (int i = 0; i < 100 && f.page().packLoading(); ++i) pump(100);
  CHECK_FALSE(f.page().packLoading());
  CHECK(f.page().statusText().isEmpty());
  CHECK(f.page().dotCount() == 8);
  CHECK(f.page().session().pack().info().title == "Synthetic 4x12 test pack");
  CHECK(fs::exists(data.dir / "packs" / "321.json"));
  CHECK_FALSE(f.page().buttonTitled("LOAD PACK")->isVisible());  // the pack is loaded

  // reopening finds the cached manifest by the cab's tone id, without running the tool (here it would fail)
  std::ofstream(data.dir / "settings.json") << json{{"t3kExecutable", "/nonexistent/sawblade-t3k"}}.dump();
  f.ed->setMicPageOpen(false);
  f.page().session().setPack(sawblade::plugin::mic::IrPack::single(f.proc.currentPreset().cab.ir));
  f.ed->setMicPageOpen(true);
  CHECK(f.page().dotCount() == 8);
  ::unsetenv("SAWBLADE_APPDATA");
}

TEST_CASE("mic page: not logged in and a missing tool show the messages", "[editor][mic][t3k]") {
  Fixture f;
  TempDir data;
  setEnv("SAWBLADE_APPDATA", data.dir.string());
  const fs::path exe = writeScript(data.dir, "fake-t3k", "echo 'auth: token expired' >&2\nexit 4");
  std::ofstream(data.dir / "settings.json") << json{{"t3kExecutable", exe.string()}}.dump();
  f.openByDoubleClick();
  f.page().loadPack();
  for (int i = 0; i < 100 && f.page().packLoading(); ++i) pump(50);
  CHECK(f.page().statusText() == "Not logged in to TONE3000. Run `sawblade-t3k login` in a terminal, then try again.");
  CHECK(f.page().dotCount() == 1);
  CHECK(f.page().buttonTitled("LOAD PACK")->isVisible());  // can try again

  std::ofstream(data.dir / "settings.json") << json{{"t3kExecutable", (data.dir / "gone" / "sawblade-t3k").string()}}.dump();
  f.page().loadPack();
  for (int i = 0; i < 100 && f.page().packLoading(); ++i) pump(50);
  CHECK(f.page().statusText().contains("Cannot find the sawblade-t3k tool"));
  auto* locate = f.page().buttonTitled(juce::String::fromUTF8("LOCATE\xe2\x80\xa6"));
  REQUIRE(locate != nullptr);
  CHECK(locate->isVisible());
  ::unsetenv("SAWBLADE_APPDATA");
}

TEST_CASE("mic page: screenshots of the rig, the page with one mic and with BLEND on", "[editor][mic][screenshot]") {
  Fixture f;
  const juce::Image rig = f.snapshot();
  REQUIRE(rig.getWidth() == 1280);
  savePng(rig, "sawblade_micpage_rig_1x.png");

  f.openByDoubleClick();
  f.loadPackFromCache();
  f.dragTo(0, 3);  // UL cap edge 1 in
  f.wait();
  f.page().refresh();
  pump(50);
  const juce::Image single = f.snapshot();
  savePng(single, "sawblade_micpage_single_1x.png");
  CHECK(f.page().responseShown());

  click(*f.page().buttonTitled("BLEND 2 MICS"));
  f.wait();
  f.dragTo(1, 6);  // the second mic on the other speaker
  f.wait();
  f.page().mixSlider().setValue(35.0, juce::sendNotificationSync);
  if (f.page().mixSlider().onDragEnd) f.page().mixSlider().onDragEnd();
  f.wait();
  f.page().refresh();
  pump(50);
  const juce::Image blend = f.snapshot();
  savePng(blend, "sawblade_micpage_blend_1x.png");

  // the page is drawn over the rig + inspector: the area below the top bar differs from the rig, the top bar does not
  int differing = 0, total = 0, topDiffers = 0;
  for (int y = 70; y < 800; y += 3)
    for (int x = 0; x < 1280; x += 3, ++total)
      if (single.getPixelAt(x, y) != rig.getPixelAt(x, y)) ++differing;
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 1280; x += 3)
      if (single.getPixelAt(x, y) != rig.getPixelAt(x, y)) ++topDiffers;
  CHECK(differing > total / 2);
  CHECK(topDiffers < 200);  // the top bar stays (only the LAT chip may change)
  int different = 0;
  for (int y = 62; y < 726; y += 2)
    for (int x = 16; x < 680; x += 2)
      if (blend.getPixelAt(x, y) != single.getPixelAt(x, y)) ++different;
  CHECK(different > 100);  // the second mic and its dot
  CHECK(f.proc.currentPreset().cab.mode == sawblade::CabMode::IrMix);
}

TEST_CASE("mic page: in BLEND the highlighted row, the marker and the fields name the active mic", "[editor][mic]") {
  Fixture f;
  f.openByDoubleClick();
  f.loadPackFromCache();
  click(*f.page().buttonTitled("BLEND 2 MICS"));
  f.wait();
  f.dragTo(1, 6);  // mic 2 onto the upper-right cap: model "V30 UR SM57 Cap 1in"
  f.wait();
  f.page().refresh();
  REQUIRE(f.page().session().active() == 1);
  const auto& pack = f.page().session().pack();
  const std::string name2 = pack.models()[static_cast<std::size_t>(f.page().session().model(1))].name;
  const std::string name1 = pack.models()[static_cast<std::size_t>(f.page().session().model(0))].name;
  REQUIRE(name1 != name2);
  juce::Label *row1 = nullptr, *row2 = nullptr;
  for (auto* l : all<juce::Label>(f.page())) {
    if (l->getText().contains(juce::String(name1)) && l->getText().contains("1  ")) row1 = l;
    if (l->getText().contains(juce::String(name2)) && l->getText().contains("2  ")) row2 = l;
  }
  REQUIRE(row1 != nullptr);
  REQUIRE(row2 != nullptr);
  CHECK(row2->getText().contains(juce::String::fromUTF8("\xe2\x96\xb8")));  // the marker is on mic 2
  CHECK_FALSE(row1->getText().contains(juce::String::fromUTF8("\xe2\x96\xb8")));
  CHECK(row2->findColour(juce::Label::textColourId) == juce::Colour(0xffb8f08a));  // highlighted
  CHECK(row1->findColour(juce::Label::textColourId) != juce::Colour(0xffb8f08a));
  CHECK(anyLabelContains(f.page(), "UPPER-RIGHT"));  // the fields describe mic 2's model
  CHECK_FALSE(anyLabelContains(f.page(), "UPPER-LEFT"));
}
