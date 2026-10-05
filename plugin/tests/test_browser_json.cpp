// Capture browser (docs/specs/phase8_capture_browser.md): pure tests of the JSON contract parsers and of the
// slot <-> preset block mapping. No JUCE message loop.

#include <catch2/catch_test_macros.hpp>

#include "browser/SlotTarget.h"
#include "browser/T3kJson.h"
#include "sawblade/preset.h"

using namespace sawblade;
using namespace sawblade::plugin;

namespace {
const std::filesystem::path kPresets = std::filesystem::path(SAWBLADE_FIXTURES_DIR) / "presets";
std::string errOf(const std::string& s, auto parser) {
  std::string e;
  auto r = parser(s, e);
  REQUIRE_FALSE(r.has_value());
  return e;
}
}  // namespace

TEST_CASE("browser json: search/list records", "[browser][json]") {
  std::string e;
  const std::string doc = R"([
    {"tone_id": 12, "title": "Boss HM-2w Åä", "creator": "ebheron", "gear": "pedal", "format": "nam", "license": "cc-by-nc",
     "favorites_count": 7, "downloads_count": 90, "created_at": "2026-07-26T10:00:00Z", "models_count": 2, "a2_models_count": 2,
     "a1_models_count": 0, "irs_count": 0, "sizes": ["standard", "lite"], "url": "https://x/12", "passes": false, "status": "excluded",
     "reasons": ["licence"], "flags": ["x"], "future_field": {"a": 1}},
    {"tone_id": 13, "title": null, "license": null}])";
  auto r = t3k::parseRecords(doc, e);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  CHECK((*r)[0].toneId == 12);
  CHECK((*r)[0].title == "Boss HM-2w \xc3\x85\xc3\xa4");
  CHECK((*r)[0].license == "cc-by-nc");
  CHECK_FALSE((*r)[0].passes);
  CHECK((*r)[0].sizes.size() == 2);
  CHECK((*r)[0].reasons == std::vector<std::string>{"licence"});
  CHECK((*r)[1].title.empty());
  CHECK((*r)[1].license.empty());
  CHECK((*r)[1].passes);  // no verdict: shown

  CHECK(t3k::parseRecords("[]", e)->empty());
  CHECK_FALSE(errOf(R"([{"title": "no id"}])", t3k::parseRecords).empty());
  CHECK_FALSE(errOf(R"([{"tone_id": "7"}])", t3k::parseRecords).empty());
  CHECK_FALSE(errOf(R"([{"tone_id": 7, "title": 5}])", t3k::parseRecords).empty());
  CHECK_FALSE(errOf(R"({"tone_id": 7})", t3k::parseRecords).empty());
  CHECK_FALSE(errOf("", t3k::parseRecords).empty());
  CHECK_FALSE(errOf("not json at all", t3k::parseRecords).empty());
  CHECK_FALSE(errOf("[{\"tone_id\": 1", t3k::parseRecords).empty());
}

TEST_CASE("browser json: stderr notes before the document are skipped", "[browser][json]") {
  std::string e;
  auto r = t3k::parseRecords("note: hello\nanother note\n[\n {\"tone_id\": 3}\n]\n", e);
  REQUIRE(r.has_value());
  CHECK(r->size() == 1);
  CHECK(t3k::extractJson("only notes\n").empty());
}

TEST_CASE("browser json: models", "[browser][json]") {
  std::string e;
  auto m = t3k::parseModels(R"({"tone_id": 5, "architecture": "A2", "models": [{"model_id": 51, "name": "Std", "size": "standard"},
                                                                              {"model_id": 52, "name": "Lite", "size": null}], "extra": 1})", e);
  REQUIRE(m.has_value());
  CHECK(m->toneId == 5);
  CHECK(m->architecture == "A2");
  REQUIRE(m->models.size() == 2);
  CHECK(m->models[0].size == "standard");
  CHECK(m->models[1].size.empty());
  auto empty = t3k::parseModels(R"({"tone_id": 5, "architecture": "", "models": []})", e);
  REQUIRE(empty.has_value());
  CHECK(empty->models.empty());
  CHECK_FALSE(errOf(R"({"tone_id": 5})", t3k::parseModels).empty());
  CHECK_FALSE(errOf(R"({"tone_id": 5, "models": [{"name": "x"}]})", t3k::parseModels).empty());
  CHECK_FALSE(errOf(R"({"models": []})", t3k::parseModels).empty());
}

TEST_CASE("browser json: fetch result", "[browser][json]") {
  std::string e;
  const std::string doc = R"({"tone_id": 7, "model_id": 71, "path": "/c/a.nam", "sha256": "ff", "kind": "nam", "gear": "amp",
    "source": {"provider": "tone3000", "id": "7", "modelId": "71", "url": "https://x/7", "title": "T ü", "creator": "me", "license": "cc-by"}})";
  auto f = t3k::parseFetch(doc, e);
  REQUIRE(f.has_value());
  CHECK(f->path == "/c/a.nam");
  CHECK(f->source.id == "7");
  CHECK(f->source.modelId == "71");
  CHECK(f->source.license == "cc-by");
  CHECK(f->source.title == "T \xc3\xbc");
  // ids as integers are accepted too
  auto g = t3k::parseFetch(R"({"tone_id": 7, "model_id": 71, "path": "p", "sha256": "ff", "kind": "ir",
                               "source": {"provider": "tone3000", "id": 7, "modelId": 71}})", e);
  REQUIRE(g.has_value());
  CHECK(g->source.id == "7");
  CHECK(g->kind == "ir");
  CHECK_FALSE(errOf(R"({"tone_id": 7, "model_id": 71, "path": "p", "sha256": "ff", "kind": "wav", "source": {"provider": "t", "id": "7"}})", t3k::parseFetch).empty());
  CHECK_FALSE(errOf(R"({"tone_id": 7, "model_id": 71, "path": "p", "sha256": "ff", "kind": "nam"})", t3k::parseFetch).empty());
  CHECK_FALSE(errOf(R"({"tone_id": 7, "model_id": 71, "sha256": "ff", "kind": "nam", "source": {"provider": "t", "id": "7"}})", t3k::parseFetch).empty());
  CHECK_FALSE(errOf(R"({"tone_id": 7, "model_id": 71, "path": 4, "sha256": "ff", "kind": "nam", "source": {"provider": "t", "id": "7"}})", t3k::parseFetch).empty());
}

TEST_CASE("browser json: whoami, error object, login events", "[browser][json]") {
  std::string e;
  auto w = t3k::parseWhoAmI(R"({"id": 42, "username": "u", "display_name": "D"})", e);
  REQUIRE(w.has_value());
  CHECK(w->id == "42");
  CHECK(t3k::parseWhoAmI(R"({"id": "abc", "username": "u"})", e)->id == "abc");
  CHECK_FALSE(errOf("[]", t3k::parseWhoAmI).empty());

  auto er = t3k::parseErrorObject(R"({"error": "no é", "code": "license"})");
  REQUIRE(er.has_value());
  CHECK(er->code == "license");
  CHECK(t3k::parseErrorObject(R"({"error": "x"})")->code == "error");
  CHECK_FALSE(t3k::parseErrorObject(R"({"id": 1})").has_value());
  CHECK_FALSE(t3k::parseErrorObject("[]").has_value());
  CHECK_FALSE(t3k::parseErrorObject("garbage").has_value());

  auto d = t3k::parseLoginLine(R"({"event":"device_code","verification_uri":"https://u","verification_uri_complete":"https://u?c=1","user_code":"AB-12","expires_in":600})");
  REQUIRE(d.has_value());
  CHECK(d->kind == t3k::LoginEvent::Kind::DeviceCode);
  CHECK(d->userCode == "AB-12");
  CHECK(d->expiresIn == 600);
  CHECK(t3k::parseLoginLine(R"({"event":"logged_in"})")->kind == t3k::LoginEvent::Kind::LoggedIn);
  CHECK(t3k::parseLoginLine(R"({"event":"logged_in","username":"u","display_name":"D","id":"7","token_file":"/t"})")->kind == t3k::LoginEvent::Kind::LoggedIn);  // phase 11 extra keys
  CHECK(t3k::parseWhoAmI(R"({"id":"7","username":"u","display_name":"D","token_file":"/t"})", e)->username == "u");
  CHECK_FALSE(t3k::parseLoginLine("refresh_token=SECRET").has_value());
  CHECK_FALSE(t3k::parseLoginLine(R"({"event":"other"})").has_value());
  CHECK_FALSE(t3k::parseLoginLine(R"({"refresh_token":"SECRET"})").has_value());
}

TEST_CASE("browser slots: block mapping", "[browser][slots]") {
  const Preset p = loadPresetFile(kPresets / "golden_shared.json");
  std::string why;
  auto t = slotTargets(p, Slot::SawPedal, &why);
  REQUIRE(t.size() == 1);
  CHECK(t[0].path == 'a');
  CHECK(t[0].blockIndex == 0);
  t = slotTargets(p, Slot::SawAmp);
  REQUIRE(t.size() == 1);
  CHECK(t[0].blockIndex == 1);
  t = slotTargets(p, Slot::BodyPedal);  // slot "boost"
  REQUIRE(t.size() == 1);
  CHECK((t[0].path == 'b' && t[0].blockIndex == 0));
  t = slotTargets(p, Slot::BodyAmp);
  CHECK((t[0].path == 'b' && t[0].blockIndex == 1));
  t = slotTargets(p, Slot::Cab);
  REQUIRE(t.size() == 1);
  CHECK(t[0].kind == SlotTarget::Kind::CabShared);

  const Preset pp = loadPresetFile(kPresets / "golden_perpath.json");
  t = slotTargets(pp, Slot::Cab);
  REQUIRE(t.size() == 2);
  CHECK(t[0].kind == SlotTarget::Kind::CabA);
  CHECK(t[1].kind == SlotTarget::Kind::CabB);
  // slots set, no pedal: none, with the reason
  t = slotTargets(pp, Slot::SawPedal, &why);
  CHECK(t.empty());
  CHECK(why == "this preset has no pedal in the saw path");
  t = slotTargets(pp, Slot::BodyAmp);
  REQUIRE(t.size() == 1);
  CHECK(t[0].blockIndex == 1);  // the eq block is skipped

  // no slots at all: a single nam block is the amp, not a pedal; two nam blocks: first = pedal, last = amp
  const Preset lin = loadPresetFile(kPresets / "two_linear.json");
  CHECK(slotTargets(lin, Slot::SawPedal, &why).empty());
  CHECK(slotTargets(lin, Slot::SawAmp).size() == 1);
  Preset two = lin;
  two.a.blocks.push_back(two.a.blocks.front());
  two.a.blocks.back().id = "a2";
  CHECK(slotTargets(two, Slot::SawPedal).at(0).blockIndex == 0);
  CHECK(slotTargets(two, Slot::SawAmp).at(0).blockIndex == 1);
}

TEST_CASE("browser slots: withCapture substitutes only the capture", "[browser][slots]") {
  const Preset p = loadPresetFile(kPresets / "golden_shared.json");
  t3k::FetchResult f;
  f.toneId = 9;
  f.modelId = 91;
  f.path = "/tmp/new.nam";
  f.sha256 = "aa";
  f.kind = "nam";
  f.source = {"tone3000", "9", "91", "https://x/9", "T", "me", "cc-by"};
  std::string err;
  auto t = slotTargets(p, Slot::BodyPedal).at(0);
  auto q = withCapture(p, t, f, err);
  REQUIRE(q.has_value());
  const auto& nb = static_cast<const NamBlockParams&>(*q->b.blocks[0].params);
  CHECK(nb.model.file == "/tmp/new.nam");
  CHECK(nb.model.resolvedPath == "/tmp/new.nam");
  CHECK(nb.model.source == f.source);
  CHECK(nb.outputGainDb == 6.0);  // the block's gains are kept
  Preset back = *q;
  back.b.blocks[0] = p.b.blocks[0];
  CHECK(back == p);  // nothing else changed

  CHECK_FALSE(withCapture(p, t, [&] { auto g = f; g.kind = "ir"; return g; }(), err).has_value());
  CHECK_FALSE(err.empty());
  auto cab = slotTargets(p, Slot::Cab).at(0);
  CHECK_FALSE(withCapture(p, cab, f, err).has_value());
  auto irf = f;
  irf.kind = "ir";
  auto c = withCapture(p, cab, irf, err);
  REQUIRE(c.has_value());
  CHECK(c->cab.ir.file == "/tmp/new.nam");
  CHECK(c->cab.ir.source == f.source);
}
