#include "sawblade/preset.h"

#include <algorithm>
#include <mutex>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "sawblade/block_registry.h"
#include "sawblade/sha256.h"

namespace sawblade {

using nlohmann::json;
namespace fs = std::filesystem;

bool operator==(const GateParams& a, const GateParams& b) {
  return a.enabled == b.enabled && a.thresholdDb == b.thresholdDb && a.hysteresisDb == b.hysteresisDb &&
         a.attackMs == b.attackMs && a.holdMs == b.holdMs && a.releaseMs == b.releaseMs && a.rangeDb == b.rangeDb &&
         a.mode == b.mode && a.ratio == b.ratio && a.keyHighPassHz == b.keyHighPassHz &&
         a.releaseCurve == b.releaseCurve;
}
bool operator==(const EqBand& a, const EqBand& b) {
  return a.type == b.type && a.freq == b.freq && a.gainDb == b.gainDb && a.q == b.q && a.enabled == b.enabled;
}

int ampIndex(const PathPreset& p) {
  const int n = static_cast<int>(p.blocks.size());
  for (int i = n - 1; i >= 0; --i)
    if (p.blocks[static_cast<std::size_t>(i)].slot == "amp") return i;
  for (int i = n - 1; i >= 0; --i)
    if (p.blocks[static_cast<std::size_t>(i)].type == "nam") return i;
  return -1;
}

bool Block::operator==(const Block& o) const {
  if (id != o.id || type != o.type || slot != o.slot || bypass != o.bypass) return false;
  if (!params || !o.params) return params == o.params;
  return params->equals(*o.params);
}

namespace {

constexpr double kGainLo = -120.0, kGainHi = 60.0;

const char* eqTypeName(EqType t) {
  switch (t) {
    case EqType::Peak: return "peak";
    case EqType::LowShelf: return "lowShelf";
    case EqType::HighShelf: return "highShelf";
    case EqType::HighPass: return "highPass";
    case EqType::LowPass: return "lowPass";
  }
  return "peak";
}

EqType eqTypeFromName(const std::string& s) {
  if (s == "lowShelf") return EqType::LowShelf;
  if (s == "highShelf") return EqType::HighShelf;
  if (s == "highPass") return EqType::HighPass;
  if (s == "lowPass") return EqType::LowPass;
  return EqType::Peak;
}

EqBand parseEqBand(const json& j, const std::string& path) {
  JsonObject o(j, path);
  EqBand b;
  b.type = eqTypeFromName(o.requireOneOf("type", {"peak", "lowShelf", "highShelf", "highPass", "lowPass"}));
  b.freq = o.requireNumber("freq", 1e-3, 96000.0);
  b.gainDb = o.number("gainDb", 0.0, -60.0, 60.0);
  b.q = o.number("q", 0.7071067811865476, 1e-3, 100.0);
  b.enabled = o.boolean("enabled", true);
  o.finish();
  return b;
}

std::vector<EqBand> parseEqBandList(JsonObject& o, const char* key) {
  std::vector<EqBand> out;
  if (const auto* arr = o.optionalArray(key, ParametricEq::kMaxBands))
    for (std::size_t i = 0; i < arr->size(); ++i) out.push_back(parseEqBand((*arr)[i], JsonObject::index(o.child(key), i)));
  return out;
}

json eqListJson(const std::vector<EqBand>& v) {
  json a = json::array();
  for (const auto& b : v) a.push_back(toJson(b));
  return a;
}

GateParams parseGate(JsonObject& root) {
  GateParams g;
  g.enabled = false;  // omitted object -> disabled
  auto o = root.optionalObject("gate");
  if (!o) return g;
  g.enabled = o->boolean("enabled", true);
  g.thresholdDb = o->number("thresholdDb", g.thresholdDb, -120.0, 0.0);
  g.hysteresisDb = o->number("hysteresisDb", g.hysteresisDb, 0.0, 60.0);
  g.attackMs = o->number("attackMs", g.attackMs, 0.01, 1000.0);
  g.holdMs = o->number("holdMs", g.holdMs, 0.0, 10000.0);
  g.releaseMs = o->number("releaseMs", g.releaseMs, 0.01, 10000.0);
  g.rangeDb = o->number("rangeDb", g.rangeDb, -120.0, 0.0);
  g.mode = o->oneOf("mode", "gate", {"gate", "expander"}) == "expander" ? GateMode::Expander : GateMode::Gate;
  g.ratio = o->number("ratio", g.ratio, 1.5, 10.0);
  g.keyHighPassHz = o->number("keyHighPassHz", g.keyHighPassHz, 0.0, 400.0);
  if (g.keyHighPassHz != 0.0 && g.keyHighPassHz < 40.0)
    throw PresetError(o->child("keyHighPassHz"), "must be 0 (off) or in [40, 400]");
  g.releaseCurve =
      o->oneOf("releaseCurve", "one-pole", {"one-pole", "linear-db"}) == "linear-db" ? GateReleaseCurve::LinearDb
                                                                                      : GateReleaseCurve::OnePole;
  o->finish();
  return g;
}

json toJson(const GateParams& g) {
  return {{"enabled", g.enabled}, {"thresholdDb", g.thresholdDb}, {"hysteresisDb", g.hysteresisDb},
          {"attackMs", g.attackMs}, {"holdMs", g.holdMs}, {"releaseMs", g.releaseMs}, {"rangeDb", g.rangeDb},
          {"mode", g.mode == GateMode::Expander ? "expander" : "gate"}, {"ratio", g.ratio},
          {"keyHighPassHz", g.keyHighPassHz},
          {"releaseCurve", g.releaseCurve == GateReleaseCurve::LinearDb ? "linear-db" : "one-pole"}};
}

BusCompParams parseBusComp(JsonObject& root) {
  BusCompParams c;
  auto o = root.optionalObject("busComp");
  if (!o) return c;
  c.enabled = o->boolean("enabled", c.enabled);
  c.thresholdDb = o->number("thresholdDb", c.thresholdDb, -80.0, 0.0);
  c.ratio = o->number("ratio", c.ratio, 1.0, 100.0);
  c.kneeDb = o->number("kneeDb", c.kneeDb, 0.0, 48.0);
  c.attackMs = o->number("attackMs", c.attackMs, 0.01, 1000.0);
  c.releaseMs = o->number("releaseMs", c.releaseMs, 1.0, 10000.0);
  c.makeupDb = o->number("makeupDb", c.makeupDb, -24.0, 48.0);
  o->finish();
  return c;
}

json toJson(const BusCompParams& c) {
  return {{"enabled", c.enabled}, {"thresholdDb", c.thresholdDb}, {"ratio", c.ratio}, {"kneeDb", c.kneeDb},
          {"attackMs", c.attackMs}, {"releaseMs", c.releaseMs}, {"makeupDb", c.makeupDb}};
}

Block parseBlock(const json& j, const std::string& path, const fs::path& baseDir, std::set<std::string>& ids) {
  JsonObject o(j, path);
  Block b;
  b.id = o.requireString("id");
  if (b.id.empty()) throw PresetError(o.child("id"), "must not be empty");
  if (!ids.insert(b.id).second) throw PresetError(o.child("id"), "duplicate block id \"" + b.id + "\"");
  b.type = o.requireString("type");
  b.slot = o.oneOf("slot", "", {"pedal", "boost", "amp", "fx"});
  b.bypass = o.boolean("bypass", false);
  const BlockType* t = BlockRegistry::instance().find(b.type);
  if (!t) throw PresetError(o.child("type"), "unknown block type \"" + b.type + "\"");
  b.params = t->parse(o, baseDir);
  if (!b.params) throw PresetError(path, "block type \"" + b.type + "\" produced no parameters");
  o.finish();
  return b;
}

json toJson(const Block& b) {
  json j = b.params ? b.params->toJson() : json::object();
  j["id"] = b.id;
  j["type"] = b.type;
  if (!b.slot.empty()) j["slot"] = b.slot;
  j["bypass"] = b.bypass;
  return j;
}

AmpControls parseAmpControls(JsonObject& path) {
  AmpControls c;
  auto o = path.optionalObject("ampControls");
  if (!o) return c;
  c.gain = o->number("gain", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.bass = o->number("bass", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.mid = o->number("mid", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.treble = o->number("treble", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.presence = o->number("presence", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.level = o->number("level", kAmpKnobDefault, kAmpKnobMin, kAmpKnobMax);
  c.gainStep = o->string("gainStep", "");
  if (o->has("gainStep") && c.gainStep.empty()) throw PresetError(o->child("gainStep"), "must not be empty");
  o->finish();
  return c;
}

json toJson(const AmpControls& c) {
  json j = {{"gain", c.gain}, {"bass", c.bass}, {"mid", c.mid},
            {"treble", c.treble}, {"presence", c.presence}, {"level", c.level}};
  if (!c.gainStep.empty()) j["gainStep"] = c.gainStep;
  return j;
}

PathPreset parsePath(JsonObject& o, const fs::path& baseDir, std::set<std::string>& ids) {
  PathPreset p;
  p.role = o.oneOf("role", "", {"saw", "body"});
  p.enabled = o.boolean("enabled", true);
  p.preEq = parseEqBandList(o, "preEq");
  if (const auto* arr = o.optionalArray("blocks", kMaxBlocksPerPath))
    for (std::size_t i = 0; i < arr->size(); ++i)
      p.blocks.push_back(parseBlock((*arr)[i], JsonObject::index(o.child("blocks"), i), baseDir, ids));
  p.eq = parseEqBandList(o, "eq");
  p.levelDb = o.number("levelDb", 0.0, kGainLo, kGainHi);
  p.invert = o.boolean("invert", false);
  p.ampControls = parseAmpControls(o);
  o.finish();
  return p;
}

json toJson(const PathPreset& p) {
  json blocks = json::array();
  for (const auto& b : p.blocks) blocks.push_back(toJson(b));
  json j = {{"enabled", p.enabled}, {"preEq", eqListJson(p.preEq)}, {"blocks", blocks},
            {"eq", eqListJson(p.eq)}, {"levelDb", p.levelDb}, {"invert", p.invert}};
  if (!p.role.empty()) j["role"] = p.role;
  if (!p.ampControls.isDefault()) j["ampControls"] = toJson(p.ampControls);
  return j;
}

AlignParams parseAlign(JsonObject& root) {
  AlignParams a;
  auto o = root.optionalObject("align");
  if (!o) return a;
  const std::string m = o->oneOf("mode", "auto", {"auto", "manual", "off"});
  a.mode = m == "manual" ? AlignMode::Manual : m == "off" ? AlignMode::Off : AlignMode::Auto;
  a.maxLagMs = o->number("maxLagMs", a.maxLagMs, 0.0, 50.0);
  a.delaySamplesB = o->integer("delaySamplesB", 0, -48000, 48000);
  a.invertB = o->boolean("invertB", false);
  o->finish();
  return a;
}

json toJson(const AlignParams& a) {
  return {{"mode", a.mode == AlignMode::Auto ? "auto" : a.mode == AlignMode::Manual ? "manual" : "off"},
          {"maxLagMs", a.maxLagMs}, {"delaySamplesB", a.delaySamplesB}, {"invertB", a.invertB}};
}

LevelMatch parseLevelMatch(JsonObject& root) {
  LevelMatch l;
  auto o = root.optionalObject("levelMatch");
  if (!o) return l;
  const std::string m = o->oneOf("mode", "off", {"auto", "manual", "off"});
  l.mode = m == "auto" ? LevelMatchMode::Auto : m == "manual" ? LevelMatchMode::Manual : LevelMatchMode::Off;
  l.trimADb = o->number("trimADb", 0.0, 0.0, kMaxLevelTrimDb);
  l.trimBDb = o->number("trimBDb", 0.0, 0.0, kMaxLevelTrimDb);
  o->finish();
  return l;
}

json toJson(const LevelMatch& l) {
  return {{"mode", l.mode == LevelMatchMode::Auto ? "auto" : l.mode == LevelMatchMode::Manual ? "manual" : "off"},
          {"trimADb", l.trimADb}, {"trimBDb", l.trimBDb}};
}

CabPreset parseCab(JsonObject& root, const fs::path& baseDir) {
  CabPreset c;
  JsonObject o = root.requireObject("cab");
  {
    const std::string m = o.requireOneOf("mode", {"shared", "perPath", "irMix"});
    c.mode = m == "shared" ? CabMode::Shared : m == "perPath" ? CabMode::PerPath : CabMode::IrMix;
  }
  c.enabled = o.boolean("enabled", true);
  c.normalize = o.boolean("normalize", true);
  auto cap = [&](const char* key) {
    const json* v = o.take(key);
    if (!v) throw PresetError(o.child(key), "required field is missing");
    return parseCapture(*v, o.child(key), baseDir);
  };
  if (c.mode == CabMode::Shared) {
    c.ir = cap("ir");
  } else {
    c.irA = cap("irA");
    c.irB = cap("irB");
    if (c.mode == CabMode::IrMix) c.mix = o.number("mix", 0.5, 0.0, 1.0);  // rejected (unknown key) in other modes
  }
  o.finish();
  return c;
}

json toJson(const CabPreset& c) {
  json j = {{"mode", c.mode == CabMode::Shared ? "shared" : c.mode == CabMode::PerPath ? "perPath" : "irMix"},
            {"enabled", c.enabled}, {"normalize", c.normalize}};
  if (c.mode == CabMode::Shared) {
    j["ir"] = toJson(c.ir);
  } else {
    j["irA"] = toJson(c.irA);
    j["irB"] = toJson(c.irB);
    if (c.mode == CabMode::IrMix) j["mix"] = c.mix;
  }
  return j;
}

}  // namespace

nlohmann::json toJson(const EqBand& b) {
  return {{"type", eqTypeName(b.type)}, {"freq", b.freq}, {"gainDb", b.gainDb}, {"q", b.q}, {"enabled", b.enabled}};
}

std::vector<EqBand> parseEqBands(const json& arr, const std::string& path) {
  if (!arr.is_array()) throw PresetError(path, "must be an array");
  if (arr.size() > static_cast<std::size_t>(ParametricEq::kMaxBands))
    throw PresetError(path, "too many bands (" + std::to_string(arr.size()) + ", max " + std::to_string(ParametricEq::kMaxBands) + ")");
  std::vector<EqBand> out;
  for (std::size_t i = 0; i < arr.size(); ++i) out.push_back(parseEqBand(arr[i], JsonObject::index(path, i)));
  return out;
}

std::vector<LadderRung> parseLadder(const json& arr, const std::string& path) {
  if (!arr.is_array()) throw PresetError(path, "must be an array");
  if (arr.size() < 2 || arr.size() > static_cast<std::size_t>(kMaxLadderRungsInPreset))
    throw PresetError(path, "a gain ladder has 2 to " + std::to_string(kMaxLadderRungsInPreset) + " rungs (omit it for none)");
  std::vector<LadderRung> out;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    JsonObject o(arr[i], JsonObject::index(path, i));
    LadderRung r;
    r.modelId = o.requireString("modelId");
    if (r.modelId.empty()) throw PresetError(o.child("modelId"), "must not be empty");
    r.gain = o.requireNumber("gain", 0.0, 100.0);
    r.name = o.string("name", "");
    o.finish();
    for (const auto& e : out) {
      if (e.modelId == r.modelId) throw PresetError(o.child("modelId"), "duplicate rung \"" + r.modelId + "\"");
      if (e.gain == r.gain) throw PresetError(o.child("gain"), "duplicate gain value");
    }
    out.push_back(std::move(r));
  }
  std::stable_sort(out.begin(), out.end(), [](const LadderRung& a, const LadderRung& b) { return a.gain < b.gain; });
  return out;
}

Capture parseCapture(const json& j, const std::string& path, const fs::path& baseDir, bool allowLadder) {
  JsonObject o(j, path);
  Capture c;
  c.file = o.requireString("file");
  if (c.file.empty()) throw PresetError(o.child("file"), "must not be empty");
  const fs::path f(c.file);
  c.resolvedPath = f.is_absolute() ? f : baseDir / f;
  if (o.has("sha256")) {
    c.sha256 = o.requireString("sha256");
    bool ok = c.sha256.size() == 64;
    for (char& ch : c.sha256) {
      if (!std::isxdigit(static_cast<unsigned char>(ch))) ok = false;
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (!ok) throw PresetError(o.child("sha256"), "must be 64 hexadecimal characters");
  }
  if (auto s = o.optionalObject("source")) {
    CaptureSource src;
    src.provider = s->requireString("provider");
    src.id = s->requireString("id");
    src.url = s->string("url", "");
    src.title = s->string("title", "");
    src.creator = s->string("creator", "");
    src.license = s->string("license", "");
    src.modelId = s->string("modelId", "");
    s->finish();
    c.source = src;
  }
  if (allowLadder && o.has("ladder")) c.ladder = parseLadder(*o.take("ladder"), o.child("ladder"));
  o.finish();
  return c;
}

nlohmann::json toJson(const Capture& c) {
  json j = {{"file", c.file}};
  if (!c.sha256.empty()) j["sha256"] = c.sha256;
  if (c.source) {
    json s = {{"provider", c.source->provider}, {"id", c.source->id}};
    const std::pair<const char*, const std::string*> opt[] = {{"modelId", &c.source->modelId}, {"url", &c.source->url},
        {"title", &c.source->title}, {"creator", &c.source->creator}, {"license", &c.source->license}};
    for (const auto& [k, v] : opt)
      if (!v->empty()) s[k] = *v;
    j["source"] = s;
  }
  if (!c.ladder.empty()) {
    json l = json::array();
    for (const auto& r : c.ladder) {
      json e = {{"modelId", r.modelId}, {"gain", r.gain}};
      if (!r.name.empty()) e["name"] = r.name;
      l.push_back(e);
    }
    j["ladder"] = l;
  }
  return j;
}

bool NamBlockParams::equals(const BlockParams& other) const {
  const auto* p = dynamic_cast<const NamBlockParams*>(&other);
  return p && inputGainDb == p->inputGainDb && outputGainDb == p->outputGainDb &&
         normalizeLoudness == p->normalizeLoudness && makeupDb == p->makeupDb && model == p->model;
}
nlohmann::json NamBlockParams::toJson() const {
  nlohmann::json j = {{"inputGainDb", inputGainDb}, {"outputGainDb", outputGainDb}, {"normalizeLoudness", normalizeLoudness},
                      {"model", sawblade::toJson(model)}};
  if (makeupDb != 0.0) j["makeupDb"] = makeupDb;  // v3; absent = 0
  return j;
}
bool EqBlockParams::equals(const BlockParams& other) const {
  const auto* p = dynamic_cast<const EqBlockParams*>(&other);
  return p && bands == p->bands;
}
nlohmann::json EqBlockParams::toJson() const { return {{"bands", eqListJson(bands)}}; }

Preset parsePreset(const json& j, const fs::path& baseDir) {
  JsonObject r(j, "");
  Preset p;
  if (r.requireString("schema") != "sawblade.preset") throw PresetError("schema", "must be exactly \"sawblade.preset\"");
  {
    const json* v = j.contains("version") ? &j["version"] : nullptr;
    if (!v) throw PresetError("version", "required field is missing");
    if (!v->is_number_integer()) throw PresetError("version", "must be an integer");
    const auto ver = v->get<long long>();
    if (ver > kPresetVersion)
      throw PresetError("version", "unsupported preset version " + std::to_string(ver) + " (this reader supports up to " +
                                       std::to_string(kPresetVersion) + ")");
    if (ver < 1) throw PresetError("version", "must be >= 1");
    r.take("version");
  }
  p.name = r.requireString("name");
  p.notes = r.string("notes", "");
  p.category = r.string("category", "");
  if (auto in = r.optionalObject("input")) {
    p.inputGainDb = in->number("gainDb", 0.0, kGainLo, kGainHi);
    in->finish();
  }
  p.gate = parseGate(r);
  {
    JsonObject paths = r.requireObject("paths");
    std::set<std::string> ids;
    JsonObject a = paths.requireObject("a");
    p.a = parsePath(a, baseDir, ids);
    JsonObject b = paths.requireObject("b");
    p.b = parsePath(b, baseDir, ids);
    paths.finish();
  }
  p.align = parseAlign(r);
  p.blend = r.number("blend", 0.5, 0.0, 1.0);
  p.blendLaw = r.oneOf("blendLaw", "linear", {"linear", "constantLoudness"}) == "linear" ? BlendLaw::Linear
                                                                                          : BlendLaw::ConstantLoudness;
  p.levelMatch = parseLevelMatch(r);
  p.cab = parseCab(r, baseDir);
  p.postEq = parseEqBandList(r, "postEq");
  p.busComp = parseBusComp(r);
  if (auto out = r.optionalObject("output")) {
    p.outputGainDb = out->number("gainDb", 0.0, kGainLo, kGainHi);
    // v3: a trim without its hash cannot be checked, so it is read as "not measured".
    const double trim = out->number("autoTrimDb", 0.0, -kMaxAutoTrimDb, kMaxAutoTrimDb);
    const std::string hash = out->string("autoTrimHash", "");
    if (!hash.empty()) {
      p.autoTrimDb = trim;
      p.autoTrimHash = hash;
    }
    out->finish();
  }
  // Plugin UI state (docs/PRESET_SCHEMA.md "playAlong"): not tone, so it is accepted and ignored here and
  // never written back. The matcher and the NAM export read presets through this parser.
  (void)r.optionalObject("playAlong");
  // Likewise the export panel's last settings (docs/PRESET_SCHEMA.md "export").
  (void)r.optionalObject("export");
  r.finish();
  return p;
}

nlohmann::json toJson(const Preset& p) {
  json j = {{"schema", p.schema},
          {"version", p.version},
          {"name", p.name},
          {"notes", p.notes},
          {"input", {{"gainDb", p.inputGainDb}}},
          {"gate", toJson(p.gate)},
          {"paths", {{"a", toJson(p.a)}, {"b", toJson(p.b)}}},
          {"align", toJson(p.align)},
          {"blend", p.blend},
          {"blendLaw", p.blendLaw == BlendLaw::Linear ? "linear" : "constantLoudness"},
          {"levelMatch", toJson(p.levelMatch)},
          {"cab", toJson(p.cab)},
          {"postEq", eqListJson(p.postEq)},
          {"busComp", toJson(p.busComp)},
          {"output", {{"gainDb", p.outputGainDb}}}};
  if (!p.autoTrimHash.empty()) {
    j["output"]["autoTrimDb"] = p.autoTrimDb;
    j["output"]["autoTrimHash"] = p.autoTrimHash;
  }
  if (!p.category.empty()) j["category"] = p.category;
  return j;
}

namespace {
// Intentionally leaked: the plugin's Settings::shared() clears this override from its destructor during static
// teardown (or when a DAW unloads the plugin), and that may run after this translation unit's statics are destroyed.
// Locking a destroyed std::mutex throws on macOS (libc++: "mutex lock failed: Invalid argument").
struct CacheOverrideState {
  std::mutex m;
  std::optional<fs::path> root;
};
CacheOverrideState& cacheOverrideState() {
  static CacheOverrideState* const s = new CacheOverrideState;
  return *s;
}
}  // namespace

void setCaptureCacheRootOverride(std::optional<fs::path> root) {
  CacheOverrideState& st = cacheOverrideState();
  std::lock_guard<std::mutex> lk(st.m);
  if (root && root->empty()) root.reset();
  st.root = std::move(root);
}

fs::path captureCacheRoot() {
  {
    CacheOverrideState& st = cacheOverrideState();
    std::lock_guard<std::mutex> lk(st.m);
    if (st.root) return *st.root;
  }
  if (const char* e = std::getenv("SAWBLADE_CACHE_DIR"); e != nullptr && *e != '\0') return fs::path(e);
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  return (home != nullptr && *home != '\0' ? fs::path(home) : fs::path(".")) / ".cache" / "sawblade" / "captures";
}

namespace {
// Ids come from preset JSON and become path components: only plain tokens (letters, digits, '_' and '-', no "..") are used.
bool safeToken(const std::string& t) {
  if (t.empty() || t.size() > 64 || t.find("..") != std::string::npos) return false;
  for (char ch : t)
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) return false;
  return true;
}
bool hasTone3000Ids(const Capture& c) {
  return c.source && c.source->provider == "tone3000" && !c.source->id.empty() && !c.source->modelId.empty();
}
bool isTone3000(const Capture& c) { return hasTone3000Ids(c) && safeToken(c.source->id) && safeToken(c.source->modelId); }
}  // namespace

fs::path locateCapture(const Capture& c) {
  std::error_code ec;
  if (fs::exists(c.resolvedPath, ec) || !isTone3000(c)) return c.resolvedPath;
  std::string ext = c.resolvedPath.extension().string();
  for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const bool ir = ext == ".wav" || ext == ".flac";
  const fs::path cached = captureCacheRoot() / c.source->id / (c.source->modelId + (ir ? ".wav" : ".nam"));
  return fs::exists(cached, ec) ? cached : c.resolvedPath;
}

std::string captureNotFoundMessage(const Capture& c, const std::string& jsonPath) {
  std::string msg = jsonPath + ": file not found: " + c.resolvedPath.string();
  if (isTone3000(c)) msg += " (not in the capture cache either; run: sawblade-t3k resolve <preset file>)";
  else if (hasTone3000Ids(c)) msg += " (the capture cache was not tried: source.id / source.modelId must be plain letters, digits, '_' or '-')";
  return msg;
}

void verifyCapture(const Capture& c, const std::string& jsonPath) {
  const fs::path p = locateCapture(c);
  if (!fs::exists(p)) throw CaptureError(jsonPath, captureNotFoundMessage(c, jsonPath));
  if (c.sha256.empty()) return;
  const std::string got = sha256File(p);
  if (got != c.sha256)
    throw CaptureError(jsonPath, jsonPath + ": sha256 mismatch for " + p.string() + " (expected " + c.sha256 +
                                     ", got " + got + ")");
}

Preset loadPresetFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open preset file: " + path.string());
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) throw std::runtime_error("read error on preset file: " + path.string());
  json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) throw PresetError("", "invalid JSON in " + path.string());
  return parsePreset(j, fs::absolute(path).parent_path());
}

}  // namespace sawblade
