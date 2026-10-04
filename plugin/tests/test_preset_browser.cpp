// Editor tests of the preset browser (docs/specs/phase9b_preset_browser.md section 5) and the top-bar A/B button / stepping
// buttons. Needs a display (xvfb-run).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "presets/AbCompare.h"
#include "presets/PresetBrowser.h"

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

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> pos, int clicks = 1) {
  const auto now = juce::Time::getCurrentTime();
  return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now, clicks, true);
}
void click(juce::Button& b) {
  juce::Component& c = b;
  const auto centre = b.getLocalBounds().toFloat().getCentre();
  c.mouseDown(mouse(b, centre));
  c.mouseUp(mouse(b, centre));
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
    dir = fs::temp_directory_path() / ("sawblade_browser_tests_" + std::to_string(juce::Random::getSystemRandom().nextInt64() & 0xffffffff));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct EnvVar {
  std::string k;
  EnvVar(const std::string& key, const std::string& v) : k(key) { ::setenv(k.c_str(), v.c_str(), 1); }
  ~EnvVar() { ::unsetenv(k.c_str()); }
};

// A loadable user preset (fixture captures, absolute paths).
void writeUserPreset(const fs::path& dir, const std::string& name, double blend) {
  const std::string nam = (fs::path(SAWBLADE_FIXTURES_DIR) / "nam" / "linear_identity.nam").string();
  auto block = [&](const char* id) { return json{{"id", id}, {"type", "nam"}, {"model", {{"file", nam}}}}; };
  const json j = {{"schema", "sawblade.preset"}, {"version", 1}, {"name", name}, {"category", "Thrash"},
                  {"paths", {{"a", {{"blocks", json::array({block("a1")})}}}, {"b", {{"blocks", json::array({block("b1")})}}}}},
                  {"align", {{"mode", "off"}}}, {"blend", blend},
                  {"cab", {{"mode", "shared"}, {"ir", {{"file", (fs::path(SAWBLADE_FIXTURES_DIR) / "ir" / "impulse.wav").string()}}}}}};
  fs::create_directories(dir);
  std::ofstream(dir / (name + ".json")) << j.dump(2);
}

// The factory presets copied to a temp dir (one licence changed to a non-commercial one, so the screenshot shows the tag),
// the user bank in the temp appdata, and an editor on top.
struct Fixture {
  juce::ScopedJuceInitialiser_GUI gui;
  TempDir tmp;
  std::unique_ptr<EnvVar> appdata;
  SawbladeProcessor proc;
  std::unique_ptr<juce::AudioProcessorEditor> base;
  SawbladeEditor* ed = nullptr;

  Fixture() {
    const fs::path factory = tmp.dir / "factory";
    fs::copy(SAWBLADE_PRESETS_DIR, factory, fs::copy_options::recursive);
    fs::remove_all(factory / "captures");
    const fs::path nc = factory / "styles" / "uk_death_bolt_thrower.json";
    std::string text;
    {
      std::ifstream in(nc);
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const auto pos = text.find("\"license\": \"t3k\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 16, "\"license\": \"cc-by-nc\"");
    std::ofstream(nc) << text;
    fs::create_directories(tmp.dir / "appdata");
    std::ofstream(tmp.dir / "appdata" / "settings.json") << json{{"factoryPresetDir", factory.string()}}.dump();
    appdata = std::make_unique<EnvVar>("SAWBLADE_APPDATA", (tmp.dir / "appdata").string());
    writeUserPreset(tmp.dir / "appdata" / "presets", "User one", 0.2);
    writeUserPreset(tmp.dir / "appdata" / "presets", "User two", 0.8);
    proc.prepareToPlay(48000.0, 512);
    proc.waitForLoader();
    base.reset(proc.createEditorAndMakeActive());
    ed = dynamic_cast<SawbladeEditor*>(base.get());
    REQUIRE(ed != nullptr);
    ed->setSize(SawbladeEditor::kDesignWidth, SawbladeEditor::kDesignHeight);
  }
  ~Fixture() { base.reset(); }

  PresetBrowser& browser() { return ed->browser(); }
  juce::Button* topButton(const juce::String& title) {
    for (auto* b : all<juce::Button>(*ed))
      if (b->getTitle() == title) return b;
    return nullptr;
  }
  int indexOf(const std::string& name) {
    const auto& e = browser().library().entries();
    for (int i = 0; i < static_cast<int>(e.size()); ++i)
      if (e[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
  }
};

bool anyLabelContains(juce::Component& root, const juce::String& text) {
  for (auto* l : all<juce::Label>(root))
    if (l->isVisible() && l->getText().contains(text)) return true;
  return false;
}

}  // namespace

TEST_CASE("browser: the preset selector opens it, the banks and categories filter the list", "[editor][browser]") {
  Fixture f;
  CHECK_FALSE(f.ed->browserOpen());
  juce::Button* sel = nullptr;
  for (auto* b : all<juce::Button>(*f.ed))
    if (b->getTitle() == "Preset") sel = b;
  REQUIRE(sel != nullptr);
  click(*sel);
  REQUIRE(f.ed->browserOpen());
  f.browser().scanBlocking();
  auto& lib = f.browser().library();
  CHECK(lib.entries().size() == 13);  // 11 factory + 2 user
  CHECK(f.browser().visible().size() == 13);

  f.browser().selectBankRow(1);  // Factory
  CHECK(f.browser().visible().size() == 11);
  f.browser().selectBankRow(2);  // Classic
  CHECK(f.browser().visible().size() == 4);
  f.browser().selectBankRow(3);  // Styles
  CHECK(f.browser().visible().size() == 6);
  f.browser().selectBankRow(4);  // Matched
  CHECK(f.browser().visible().size() == 1);
  f.browser().selectBankRow(5);  // User
  CHECK(f.browser().visible().size() == 2);
  f.browser().selectBankRow(1);
  const auto cats = f.browser().categoryRows();
  CHECK(cats[0] == "All categories  (11)");
  bool grind = false;
  for (const auto& c : cats) grind = grind || c == "Grind  (1)";
  CHECK(grind);
  f.browser().selectCategory("Death metal");
  CHECK(f.browser().visible().size() == 3);  // studio_split, uk death, barbaric
  f.browser().setSearch("bolt");
  CHECK(f.browser().visible().size() == 1);
  f.browser().setSearch("");
  f.browser().selectCategory("");
  CHECK(f.browser().visible().size() == 11);

  // close
  click(*f.browser().buttonTitled(juce::String::fromUTF8("\xe2\x80\xb9 BACK")));
  CHECK_FALSE(f.ed->browserOpen());
}

TEST_CASE("browser: selecting a preset shows its info; factory presets are read-only in the footer", "[editor][browser]") {
  Fixture f;
  f.ed->setBrowserOpen(true);
  f.browser().scanBlocking();
  f.browser().selectBankRow(1);
  const int uk = f.indexOf("UK Death (Bolt Thrower-style)");
  REQUIRE(uk >= 0);
  f.browser().selectEntry(uk);
  const auto text = f.browser().infoPanel().plainText();
  CHECK(text.contains("UK Death (Bolt Thrower-style)"));
  CHECK(text.contains("DEATH METAL"));
  CHECK(text.contains("STYLES"));
  CHECK(text.contains("@"));  // a creator
  CHECK(text.contains("cc-by-nc"));
  CHECK(f.browser().infoPanel().nonCommercialTags() >= 1);
  CHECK_FALSE(f.browser().buttonTitled("DELETE")->isEnabled());
  CHECK_FALSE(f.browser().buttonTitled("RENAME")->isEnabled());
  CHECK(f.browser().buttonTitled("SAVE AS")->isEnabled());
  CHECK_FALSE(f.browser().deleteSelected());
  CHECK_FALSE(f.browser().renameSelected("x"));

  f.browser().selectBankRow(5);
  f.browser().selectEntry(f.indexOf("User one"));
  CHECK(f.browser().buttonTitled("DELETE")->isEnabled());
  CHECK(f.browser().buttonTitled("RENAME")->isEnabled());
}

TEST_CASE("browser: loading, stepping with the top-bar buttons, save as and delete", "[editor][browser]") {
  Fixture f;
  f.ed->setBrowserOpen(true);
  f.browser().scanBlocking();
  f.browser().selectBankRow(5);  // User: User one, User two
  REQUIRE(f.browser().visible().size() == 2);
  REQUIRE(f.browser().loadEntry(f.indexOf("User two")));
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User two");
  CHECK(f.proc.currentPreset().blend == 0.8);

  auto* prev = f.topButton(juce::String::fromUTF8("\xe2\x80\xb9"));
  auto* next = f.topButton(juce::String::fromUTF8("\xe2\x80\xba"));
  REQUIRE(prev != nullptr);
  REQUIRE(next != nullptr);
  CHECK(prev->isEnabled());
  click(*prev);  // User two -> User one (the list is sorted by file name)
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User one");
  click(*next);
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User two");
  click(*next);  // wraps
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User one");

  // Save As, then Save over it, rename, delete (a fake trash)
  REQUIRE(f.browser().saveAs("From the test", "Prog", false));
  CHECK(fs::exists(f.tmp.dir / "appdata" / "presets" / "From the test.json"));
  CHECK(f.browser().currentFileName() == "From the test.json");
  CHECK(f.browser().library().entries()[static_cast<std::size_t>(f.browser().selectedEntry())].category == "Prog");
  CHECK_FALSE(f.browser().saveAs("From the test", "Prog", false));  // collision
  CHECK(f.browser().message().contains("exists"));
  CHECK(f.browser().saveCurrent());
  REQUIRE(f.browser().renameSelected("Renamed from the test"));
  CHECK(fs::exists(f.tmp.dir / "appdata" / "presets" / "Renamed from the test.json"));
  f.browser().setTrashFunction([](const fs::path& p) { return fs::remove(p); });
  REQUIRE(f.browser().deleteSelected());
  CHECK_FALSE(fs::exists(f.tmp.dir / "appdata" / "presets" / "Renamed from the test.json"));
  CHECK(f.browser().library().entries().size() == 13);
}

TEST_CASE("A/B button: shows A or B, switches the sound, replaced by browser loads on the active slot", "[editor][browser][ab]") {
  Fixture f;
  auto* ab = f.topButton("A/B compare");
  REQUIRE(ab != nullptr);
  CHECK(ab->isEnabled());
  CHECK(ab->getButtonText() == "A");
  f.ed->setBrowserOpen(true);
  f.browser().scanBlocking();
  REQUIRE(f.browser().loadEntry(f.indexOf("User one")));  // on A
  REQUIRE(f.proc.waitForLoader());
  click(*ab);  // -> B (a copy of A)
  CHECK(ab->getButtonText() == "B");
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User one");
  REQUIRE(f.browser().loadEntry(f.indexOf("User two")));  // replaces B only
  REQUIRE(f.proc.waitForLoader());
  click(*ab);  // -> A
  REQUIRE(f.proc.waitForLoader());
  CHECK(ab->getButtonText() == "A");
  CHECK(f.proc.currentPreset().name == "User one");
  click(*ab);  // -> B
  REQUIRE(f.proc.waitForLoader());
  CHECK(f.proc.currentPreset().name == "User two");
}

TEST_CASE("browser: screenshot with a factory category selected and the info panel on a capture with an NC tag", "[editor][browser][screenshot]") {
  Fixture f;
  f.ed->setBrowserOpen(true);
  f.browser().scanBlocking();
  f.browser().selectBankRow(1);
  f.browser().selectCategory("Death metal");
  f.browser().selectEntry(f.indexOf("UK Death (Bolt Thrower-style)"));
  auto* ab = f.topButton("A/B compare");
  REQUIRE(f.browser().loadEntry(f.indexOf("User one")));
  REQUIRE(f.proc.waitForLoader());
  click(*ab);  // the compare button shows B
  REQUIRE(f.proc.waitForLoader());
  CHECK(ab->getButtonText() == "B");
  f.browser().selectEntry(f.indexOf("UK Death (Bolt Thrower-style)"));
  const juce::Image img = f.ed->createComponentSnapshot(f.ed->getLocalBounds(), true, 1.0f);
  REQUIRE(img.getWidth() == 1280);
  savePng(img, "sawblade_browser_1x.png");
  CHECK(f.browser().infoPanel().nonCommercialTags() >= 1);
  CHECK(anyLabelContains(*f.ed, "PRESETS"));
}
