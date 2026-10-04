// Capture browser (docs/specs/phase8_capture_browser.md): the child-process client against a fake CLI,
// the controller (login, USE, PREVIEW) and the overlay UI. Needs a message loop and a display (xvfb-run).

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SawbladeLookAndFeel.h"
#include "browser/BrowserSettings.h"
#include "browser/CaptureBrowser.h"
#include "browser/T3kClient.h"

using namespace sawblade;
using namespace sawblade::plugin;
namespace fs = std::filesystem;

namespace {
const fs::path kFixtures = SAWBLADE_FIXTURES_DIR;

template <class T>
void collectAll(juce::Component& c, std::vector<T*>& out) {
  for (auto* child : c.getChildren()) {
    if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
    collectAll(*child, out);
  }
}
template <class T>
std::vector<T*> allOf(juce::Component& root) {
  std::vector<T*> v;
  collectAll<T>(root, v);
  return v;
}

bool pumpUntil(const std::function<bool()>& pred, int timeoutMs = 20000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  return true;
}
void pumpFor(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

// Environment of the fake CLI, restored on exit.
struct Env {
  std::vector<std::string> keys;
  void set(const char* k, const std::string& v) {
    keys.push_back(k);
    setenv(k, v.c_str(), 1);
  }
  ~Env() {
    for (auto& k : keys) unsetenv(k.c_str());
  }
};

struct TempDir {
  fs::path dir;
  TempDir() {
    dir = fs::temp_directory_path() / ("sawblade_browser_ui_" + std::to_string(juce::Random::getSystemRandom().nextInt()));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

juce::String allText(juce::Component& root) {
  juce::String t;
  for (auto* l : allOf<juce::Label>(root)) t << l->getText() << "\n";
  for (auto* e : allOf<juce::TextEditor>(root)) t << e->getText() << "\n";
  for (auto* b : allOf<juce::TextButton>(root)) t << b->getButtonText() << "\n";
  return t;
}

juce::TextButton* buttonTitled(juce::Component& root, const juce::String& title) {
  for (auto* b : allOf<juce::TextButton>(root))
    if (b->getTitle() == title && b->isShowing() == b->isShowing()) return b;
  return nullptr;
}

struct Rig {
  juce::ScopedJuceInitialiser_GUI gui;
  SawbladeLookAndFeel laf;
  TempDir tmp;
  Env env;
  SawbladeProcessor proc;
  std::unique_ptr<BrowserSettings> settings;

  explicit Rig(const char* preset = "golden_shared.json") {
    juce::LookAndFeel::setDefaultLookAndFeel(&laf);
    settings = std::make_unique<BrowserSettings>(juce::File(juce::String((tmp.dir / "browser.settings").string())));
    settings->setExecutable(SAWBLADE_FAKE_T3K);
    if (preset != nullptr) proc.loadPresetFile(kFixtures / "presets" / preset);
    proc.prepareToPlay(48000.0, 512);
    proc.waitForLoader();
  }
  ~Rig() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); }
  std::function<std::string()> exe() { return [this] { return settings->executable(); }; }
};

template <class T>
Reply<T> await(std::function<void(std::function<void(Reply<T>)>)> start, int ms = 20000) {
  bool done = false;
  Reply<T> out;
  start([&](Reply<T> r) {
    out = std::move(r);
    done = true;
  });
  REQUIRE(pumpUntil([&] { return done; }, ms));
  return out;
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
}  // namespace

TEST_CASE("t3k client: every command against the fake CLI", "[browser][client]") {
  Rig rig(nullptr);
  T3kClient c(rig.exe());
  auto who = await<t3k::WhoAmI>([&](auto cb) { c.whoami(cb); });
  REQUIRE(who.ok);
  CHECK(who.value.username == "tester");

  auto s = await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Search, "HM-2", "pedal", 50, cb); });
  REQUIRE(s.ok);
  CHECK(s.value.size() == 6);
  CHECK(s.value[1].title == juce::String::fromUTF8("Swedish Chainsaw \xc3\xa5\xc3\xa4\xc3\xb6").toStdString());
  auto fav = await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Favorites, "", "pedal", 100, cb); });
  REQUIRE(fav.ok);
  CHECK(fav.value.size() == 6);
  auto pool = await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Pool, "", "amp", 100, cb); });
  REQUIRE(pool.ok);
  CHECK(pool.value.size() == 2);
  auto ir = await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Favorites, "", "ir", 100, cb); });
  REQUIRE(ir.ok);
  CHECK(ir.value.at(0).gear == "ir");

  auto m = await<t3k::ModelsResult>([&](auto cb) { c.models(101, cb); });
  REQUIRE(m.ok);
  REQUIRE(m.value.models.size() == 2);
  CHECK(m.value.models[0].modelId == 1011);
  auto nf = await<t3k::ModelsResult>([&](auto cb) { c.models(999, cb); });
  CHECK_FALSE(nf.ok);
  CHECK(nf.error.code == "not_found");

  auto f = await<t3k::FetchResult>([&](auto cb) { c.fetch(101, 1012, cb); });
  REQUIRE(f.ok);
  CHECK(f.value.modelId == 1012);
  CHECK(f.value.kind == "nam");
  CHECK(f.value.source.license == "cc-by");
  CHECK(fs::exists(f.value.path));
  auto lic = await<t3k::FetchResult>([&](auto cb) { c.fetch(103, 0, cb); });
  CHECK_FALSE(lic.ok);
  CHECK(lic.error.code == "license");

  CHECK(T3kClient::listArgs(T3kClient::Source::Search, "-x y", "amp", 20) ==
        std::vector<std::string>{"search", "--json", "--limit", "20", "--gear", "amp", "--", "-x y"});
  CHECK(T3kClient::listArgs(T3kClient::Source::Pool, "-q", "", 5) ==
        std::vector<std::string>{"list", "--source", "pool", "--json", "--query=-q", "--limit", "5"});

  // A query starting with '-' reaches the CLI intact.
  TempDir logd;
  rig.env.set("FAKE_T3K_LOG", (logd.dir / "log.txt").string());
  auto dashed = await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Search, "-fuzz --gear", "amp", 5, cb); });
  CHECK(dashed.ok);
  std::ifstream lf(logd.dir / "log.txt");
  std::string line, last;
  while (std::getline(lf, line)) last = line;
  CHECK(last == R"(["search", "--json", "--limit", "5", "--gear", "amp", "--", "-fuzz --gear"])");
}

TEST_CASE("t3k client: errors, garbage, timeout, merged stderr, launch failure", "[browser][client]") {
  Rig rig(nullptr);
  T3kClient c(rig.exe());
  auto query = [&] { return await<T3kClient::Records>([&](auto cb) { c.list(T3kClient::Source::Favorites, "", "pedal", 10, cb); }); };

  rig.env.set("FAKE_T3K_MODE", "error");
  rig.env.set("FAKE_T3K_CODE", "network");
  auto e = query();
  CHECK_FALSE(e.ok);
  CHECK(e.error.code == "network");
  CHECK(e.error.message == "fake failure");

  rig.env.set("FAKE_T3K_MODE", "garbage");
  auto g = query();
  CHECK_FALSE(g.ok);
  CHECK(g.error.code == "parse");

  rig.env.set("FAKE_T3K_MODE", "exit1");
  auto x = query();
  CHECK_FALSE(x.ok);
  CHECK(x.error.code == "exit");
  CHECK(x.error.message.find("Traceback") != std::string::npos);

  rig.env.set("FAKE_T3K_MODE", "noisy");
  CHECK(query().ok);

  rig.env.set("FAKE_T3K_MODE", "sleep");
  c.queryTimeoutMs = 600;
  const auto t0 = std::chrono::steady_clock::now();
  bool done = false;
  Reply<T3kClient::Records> tr;
  c.list(T3kClient::Source::Favorites, "", "pedal", 10, [&](auto r) {
    tr = std::move(r);
    done = true;
  });
  const auto returned = std::chrono::steady_clock::now() - t0;
  CHECK(returned < std::chrono::milliseconds(100));  // the message thread is not blocked
  CHECK_FALSE(done);                                 // and the callback comes through the message loop
  REQUIRE(pumpUntil([&] { return done; }, 10000));
  CHECK(tr.error.code == "timeout");
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8));  // the child was killed, not waited for

  rig.settings->setExecutable("/nonexistent/sawblade-t3k");
  auto l = query();
  CHECK_FALSE(l.ok);
  CHECK(l.error.code == "launch");
}

TEST_CASE("t3k client: a destroyed client never calls back", "[browser][client]") {
  Rig rig(nullptr);
  rig.env.set("FAKE_T3K_MODE", "ok");
  bool called = false;
  {
    T3kClient c(rig.exe());
    c.whoami([&](auto) { called = true; });
  }
  pumpFor(500);
  CHECK_FALSE(called);
}

TEST_CASE("t3k client: a newer query supersedes a queued older one", "[browser][client]") {
  Rig rig(nullptr);
  TempDir logd;
  rig.env.set("FAKE_T3K_LOG", (logd.dir / "log.txt").string());
  T3kClient c(rig.exe());
  int firstDone = 0, lastDone = 0;
  c.whoami([&](auto) { ++firstDone; });  // occupies the worker
  c.list(T3kClient::Source::Search, "one", "pedal", 10, [&](auto) { ++lastDone; });
  c.list(T3kClient::Source::Search, "two", "pedal", 10, [&](auto) { lastDone += 10; });
  REQUIRE(pumpUntil([&] { return lastDone >= 10; }));
  pumpFor(300);
  CHECK(lastDone == 10);  // "one" was dropped before it ran (or at worst ran first); never both callbacks
}

TEST_CASE("browser: login view, device code, logged_in retries the query, nothing secret is shown", "[browser][login]") {
  Rig rig;
  TempDir state;
  rig.env.set("FAKE_T3K_STATE", state.dir.string());
  rig.env.set("FAKE_T3K_LOGIN_DELAY", "1.0");
  CaptureBrowser b(rig.proc, *rig.settings, Slot::SawPedal);
  auto& ctl = b.controller();
  REQUIRE(pumpUntil([&] { return ctl.state().view == BrowserController::View::LoginRequired; }));
  CHECK(allText(b).contains("Log in to TONE3000"));
  auto* btn = buttonTitled(b, "LOG IN");
  REQUIRE(btn != nullptr);
  CHECK(btn->isVisible());
  btn->triggerClick();
  REQUIRE(pumpUntil([&] { return !ctl.state().loginCode.empty(); }));
  CHECK(ctl.state().loginCode == "ABCD-1234");
  CHECK(ctl.state().loginUri == "https://www.tone3000.com/device?code=ABCD-1234");
  const juce::String during = allText(b);
  CHECK(during.contains("ABCD-1234"));
  CHECK(during.contains("tone3000.com/device"));
  CHECK_FALSE(during.contains("SECRETTOKEN"));
  REQUIRE(pumpUntil([&] { return ctl.state().view == BrowserController::View::Browse && !ctl.state().records.empty(); }));
  pumpFor(100);
  CHECK(ctl.state().records.size() == 6);  // the pending query ran after logged_in
  CHECK_FALSE(allText(b).contains("SECRETTOKEN"));
}

TEST_CASE("browser: licences are shown on every card and in the selected panel; a licence error is displayed; screenshot", "[browser][ui]") {
  Rig rig;
  CaptureBrowser b(rig.proc, *rig.settings, Slot::SawPedal);
  auto& ctl = b.controller();
  REQUIRE(pumpUntil([&] { return ctl.state().view == BrowserController::View::Browse && !ctl.state().records.empty() && !ctl.state().loading; }));
  pumpFor(100);

  auto licences = [&] {
    std::vector<juce::String> v;
    for (auto* l : allOf<juce::Label>(b))
      if (l->getComponentID() == "licence") v.push_back(l->getText());
    return v;
  };
  // QUALITY on (default): the failing record is hidden.
  CHECK(licences().size() == 5);
  for (const auto& t : licences()) CHECK(t != "cc-by-nc");
  // QUALITY off: all six, each card's licence label equals its record's licence.
  ctl.setPassesOnly(false);
  REQUIRE(pumpUntil([&] { return licences().size() == 6; }));
  std::multiset<std::string> got, want;
  for (const auto& t : licences()) got.insert(t.toStdString());
  for (const auto& r : ctl.state().records) want.insert(r.license.empty() ? "unknown licence" : r.license);
  CHECK(got == want);
  CHECK(got.count("cc-by-nc") == 1);
  CHECK(got.count("unknown licence") == 1);  // the record with an empty licence

  // Selected panel: the licence, also for a record that fails the filter; models are listed.
  ctl.select(103);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2 && buttonTitled(b, "Standard   standard") != nullptr; }));
  juce::Label* sel = nullptr;
  for (auto* l : allOf<juce::Label>(b))
    if (l->getComponentID() == "selLicence") sel = l;
  REQUIRE(sel != nullptr);
  CHECK(sel->getText() == "cc-by-nc");
  CHECK(sel->isVisible());
  CHECK(allText(b).contains(CaptureBrowser::licenceNote()));

  // USE on it: the CLI refuses with code "license"; the message is displayed; nothing was swapped.
  const Preset before = rig.proc.currentPreset();
  ctl.use(0);
  REQUIRE(pumpUntil([&] { return ctl.state().statusIsError; }));
  CHECK(allText(b).contains("licence cc-by-nc is not allowed (license)"));
  CHECK(rig.proc.currentPreset() == before);

  // Screenshot: a selected, passing card with its models.
  ctl.select(101);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2 && ctl.selected() != nullptr && ctl.selected()->toneId == 101; }));
  pumpFor(100);
  b.setBounds(0, 0, CaptureBrowser::kWidth, CaptureBrowser::kHeight);
  const juce::Image img = b.createComponentSnapshot(b.getLocalBounds(), true, 1.0f);
  REQUIRE(img.getWidth() == 1280);
  REQUIRE(img.getHeight() == 800);
  savePng(img, "capture_browser.png");
}

TEST_CASE("browser: USE swaps the capture through the loader; mismatches and missing blocks do not", "[browser][ui]") {
  Rig rig;
  CaptureBrowser b(rig.proc, *rig.settings, Slot::BodyAmp);
  auto& ctl = b.controller();
  REQUIRE(pumpUntil([&] { return !ctl.state().records.empty() && !ctl.state().loading; }));
  ctl.select(104);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2; }));
  ctl.selectModel(1);
  const Preset before = rig.proc.currentPreset();
  ctl.use(0);
  REQUIRE(pumpUntil([&] { return ctl.state().status.rfind("Using", 0) == 0 || ctl.state().statusIsError; }));
  CHECK_FALSE(ctl.state().statusIsError);
  const Preset after = rig.proc.currentPreset();
  const auto& m = static_cast<const NamBlockParams&>(*after.b.blocks[1].params).model;
  CHECK(m.source->id == "104");
  CHECK(m.source->modelId == "1042");  // the selected model
  CHECK(m.source->license == "cc-by");
  CHECK(rig.proc.status().error.empty());
  CHECK(after.a == before.a);

  // kind mismatch: an IR for an amp slot
  rig.env.set("FAKE_T3K_KIND", "ir");
  rig.env.set("FAKE_T3K_FETCH", (kFixtures / "ir" / "ir_a.wav").string());
  const Preset p1 = rig.proc.currentPreset();
  ctl.use(0);
  REQUIRE(pumpUntil([&] { return ctl.state().statusIsError; }));
  CHECK(allText(b).contains("impulse response"));
  CHECK(rig.proc.currentPreset() == p1);
}

TEST_CASE("browser: a slot with no block disables USE and PREVIEW and says why", "[browser][ui]") {
  Rig rig("golden_perpath.json");  // no pedal in either path
  CaptureBrowser b(rig.proc, *rig.settings, Slot::SawPedal);
  auto& ctl = b.controller();
  REQUIRE(pumpUntil([&] { return !ctl.state().records.empty() && !ctl.state().loading; }));
  ctl.select(101);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2; }));
  pumpFor(100);
  CHECK(allText(b).contains("this preset has no pedal in the saw path"));
  for (auto* btn : allOf<juce::TextButton>(b))
    if (btn->getTitle().startsWith("Preview ") || btn->getTitle().startsWith("Use ") || btn->getButtonText().startsWith("USE IN"))
      if (btn->isVisible()) CHECK_FALSE(btn->isEnabled());
}

TEST_CASE("browser: per-path cab offers two USE buttons", "[browser][ui]") {
  Rig rig("golden_perpath.json");
  CaptureBrowser b(rig.proc, *rig.settings, Slot::Cab);
  auto& ctl = b.controller();
  rig.env.set("FAKE_T3K_KIND", "ir");
  rig.env.set("FAKE_T3K_FETCH", (kFixtures / "ir" / "ir_a.wav").string());
  REQUIRE(pumpUntil([&] { return !ctl.state().records.empty() && !ctl.state().loading; }));
  CHECK(ctl.state().gear == "ir");
  ctl.select(201);
  REQUIRE(pumpUntil([&] { return ctl.state().models.size() == 2; }));
  pumpFor(100);
  auto* a = buttonTitled(b, "USE IN SAW CAB");
  auto* bb = buttonTitled(b, "USE IN BODY CAB");
  REQUIRE(a != nullptr);
  REQUIRE(bb != nullptr);
  bb->triggerClick();
  REQUIRE(pumpUntil([&] { return ctl.state().status.rfind("Using", 0) == 0 || ctl.state().statusIsError; }));
  const Preset p = rig.proc.currentPreset();
  CHECK(p.cab.irB.source.has_value());
  CHECK(p.cab.irB.source->id == "201");
  CHECK_FALSE(p.cab.irA.source.has_value());
}

TEST_CASE("browser: PREVIEW renders through the rig and plays on the audio thread; closing stops it", "[browser][preview]") {
  Rig rig;
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  auto block = [&] {
    buf.clear();
    rig.proc.processBlock(buf, midi);
  };
  {
    CaptureBrowser b(rig.proc, *rig.settings, Slot::SawAmp);
    auto& ctl = b.controller();
    REQUIRE(pumpUntil([&] { return !ctl.state().records.empty() && !ctl.state().loading; }));
    auto* pv = buttonTitled(b, "Preview Boss HM-2w CHAINSAW");
    REQUIRE(pv != nullptr);
    pv->triggerClick();
    REQUIRE(pumpUntil([&] { return ctl.state().previewing; }, 60000));
    // silent input: only the preview can make sound
    float peak = 0.0f;
    for (int i = 0; i < 40; ++i) {
      block();
      peak = std::max(peak, buf.getMagnitude(0, 512));
    }
    CHECK(peak > 0.05f);
    CHECK(rig.proc.previewPlayer().playing());
    CHECK(rig.proc.currentPreset().a.blocks[1].id == "a2");  // the rig itself was not changed by the preview
  }
  // browser closed: the preview fades out
  for (int i = 0; i < 4; ++i) block();
  CHECK_FALSE(rig.proc.previewPlayer().playing());
  block();
  CHECK(buf.getMagnitude(0, 512) < 1e-6f);
}

TEST_CASE("browser: the editor's BROWSE CAPTURES opens the overlay for the selected piece and closes it", "[browser][ui]") {
  juce::ScopedJuceInitialiser_GUI gui;
  Env env;
  env.set("HOME", fs::temp_directory_path().string());  // the default settings file stays out of the real home
  SawbladeProcessor proc;
  proc.prepareToPlay(48000.0, 512);
  std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditorAndMakeActive());
  auto* ed = dynamic_cast<SawbladeEditor*>(base.get());
  REQUIRE(ed != nullptr);
  juce::TextButton* browse = nullptr;
  for (auto* bt : allOf<juce::TextButton>(*ed))
    if (bt->getButtonText() == "BROWSE CAPTURES") browse = bt;
  REQUIRE(browse != nullptr);
  CHECK(browse->isEnabled());
  CHECK(allOf<CaptureBrowser>(*ed).empty());
  browse->triggerClick();  // asynchronous
  REQUIRE(pumpUntil([&] { return allOf<CaptureBrowser>(*ed).size() == 1; }));
  auto bs = allOf<CaptureBrowser>(*ed);
  CHECK(allText(*bs[0]).contains("SAW PEDAL SLOT"));
  CHECK(bs[0]->getWidth() == 1280);
  buttonTitled(*bs[0], juce::String::fromUTF8("\xe2\x80\xb9 RIG"))->triggerClick();
  REQUIRE(pumpUntil([&] { return allOf<CaptureBrowser>(*ed).empty(); }));
}

TEST_CASE("browser: destroying the browser during a slow render returns at once and never starts the preview", "[browser][preview]") {
  Rig rig;
  std::atomic<bool> rendering{false}, finished{false};
  const auto t0 = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration destroyTime{};
  {
    auto b = std::make_unique<CaptureBrowser>(rig.proc, *rig.settings, Slot::SawAmp);
    auto& ctl = b->controller();
    ctl.previewRender = [&](const Preset&, double, std::string&) {
      rendering = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      finished = true;
      return std::vector<float>(48000, 0.3f);
    };
    REQUIRE(pumpUntil([&] { return !ctl.state().records.empty() && !ctl.state().loading; }));
    buttonTitled(*b, "Preview Boss HM-2w CHAINSAW")->triggerClick();
    REQUIRE(pumpUntil([&] { return rendering.load(); }));
    const auto d0 = std::chrono::steady_clock::now();
    b.reset();
    destroyTime = std::chrono::steady_clock::now() - d0;
  }
  CHECK(destroyTime < std::chrono::milliseconds(150));
  CHECK_FALSE(finished.load());  // the render was still running when the destructor returned
  REQUIRE(pumpUntil([&] { return finished.load(); }));
  pumpFor(300);  // the result is dropped: no preview starts
  CHECK_FALSE(rig.proc.previewPlayer().playing());
  juce::AudioBuffer<float> buf(2, 512);
  juce::MidiBuffer midi;
  buf.clear();
  rig.proc.processBlock(buf, midi);
  CHECK(buf.getMagnitude(0, 512) < 0.05f);  // the rig (not the 0.3 preview)
  (void)t0;
}
