#include "ExportNotes.h"

#include <cstdio>
#include <vector>

// A line-by-line port of match/sawblade_match/export/notes.py (v0.4M, NOTES_VERSION 1). The Python reads the preset as a JSON
// dict with `d.get(key, default)`; the same is done here on the JSON (the Preset overload goes through toJson, which writes
// every member), so defaults and missing members behave identically.

namespace sawblade::plugin {

using json = nlohmann::json;

const char* const kExportNotesDisclaimer =
    "Derived from TONE3000 captures; for personal use only. dB figures are digital (dBFS), "
    "so match levels by ear / meter on the device.";

namespace {

const char* const kBefore = "before NAM";
const char* const kAfter = "after NAM";
const char* const kNothing = "Nothing to add: the trained model (and its IR, if any) contains the whole chain.";

// Python truthiness of a JSON value.
bool truthy(const json& v) {
  if (v.is_null()) return false;
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer()) return v.get<long long>() != 0;
  if (v.is_number()) return v.get<double>() != 0.0;
  if (v.is_string()) return !v.get_ref<const std::string&>().empty();
  return !v.empty();  // array / object
}

// dict.get(key): the member, or null (also when `j` is not an object).
json get(const json& j, const char* key) {
  if (j.is_object()) {
    const auto it = j.find(key);
    if (it != j.end()) return *it;
  }
  return json();
}
bool has(const json& j, const char* key) { return j.is_object() && j.contains(key); }
// dict.get(key, default) for a value that is then wrapped in float(...).
double getNum(const json& j, const char* key, double def) {
  const json v = get(j, key);
  return v.is_number() ? v.get<double>() : def;
}
// dict.get(key) or {} (a falsy member counts as absent).
json objectOrEmpty(const json& j, const char* key) {
  json v = get(j, key);
  return truthy(v) && v.is_object() ? v : json::object();
}

std::string fixed(double x, const char* fmt) {
  char buf[64];
  std::snprintf(buf, sizeof buf, fmt, x);
  return buf;
}

// notes.py _g: fixed notation, then strip trailing zeros and a trailing '.'; "" -> "0".
std::string g(double x, int nd = 2) {
  char fmt[16];
  std::snprintf(fmt, sizeof fmt, "%%.%df", nd);
  std::string s = fixed(x, fmt);
  while (!s.empty() && s.back() == '0') s.pop_back();
  while (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? std::string("0") : s;
}
// "{x:+.1f}"
std::string signed1(double x) { return fixed(x, "%+.1f"); }

// Python str() of a JSON scalar.
std::string pyStr(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_null()) return "None";
  return v.dump();
}

std::string baseName(std::string f) {
  for (char& c : f)
    if (c == '\\') c = '/';
  const auto slash = f.rfind('/');
  return slash == std::string::npos ? f : f.substr(slash + 1);
}

json captureInfo(const json& capIn) {
  const json cap = truthy(capIn) ? capIn : json::object();
  json src = get(cap, "source");
  if (!truthy(src)) src = json::object();
  json out = json::object();
  const json file = get(cap, "file");
  const std::string fname = truthy(file) ? baseName(pyStr(file)) : std::string();
  if (!fname.empty()) out["file"] = fname;
  const std::pair<const char*, const char*> map[] = {{"title", "title"}, {"creator", "creator"}, {"license", "license"},
                                                     {"provider", "provider"}, {"toneId", "id"}, {"url", "url"}};
  for (const auto& [dst, srcKey] : map) {
    const json v = get(src, srcKey);
    if (!v.is_null()) out[dst] = v;
  }
  json mic = get(src, "mic");
  if (!truthy(mic)) mic = get(src, "mics");
  if (truthy(mic)) out["mic"] = mic;
  return out;
}

std::string capLabel(const json& ci) {
  const auto str = [&](const char* k) { return truthy(get(ci, k)) ? pyStr(get(ci, k)) : std::string(); };
  std::string name = str("title");
  if (name.empty()) name = str("file");
  if (name.empty()) name = "unnamed IR";
  std::vector<std::string> bits;
  if (truthy(get(ci, "file")) && truthy(get(ci, "title")) && get(ci, "file") != get(ci, "title")) bits.push_back("file " + str("file"));
  if (truthy(get(ci, "creator"))) bits.push_back("by " + str("creator"));
  if (truthy(get(ci, "license"))) bits.push_back(str("license"));
  if (!get(ci, "toneId").is_null()) bits.push_back((truthy(get(ci, "provider")) ? str("provider") : std::string("source")) + " id " + pyStr(get(ci, "toneId")));
  if (truthy(get(ci, "mic"))) {
    const json m = get(ci, "mic");
    std::string s = "mic ";
    if (m.is_array()) {
      for (std::size_t i = 0; i < m.size(); ++i) s += (i ? ", " : "") + pyStr(m[i]);
    } else {
      s += pyStr(m);
    }
    bits.push_back(s);
  }
  if (bits.empty()) return name;
  std::string joined;
  for (std::size_t i = 0; i < bits.size(); ++i) joined += (i ? "; " : "") + bits[i];
  return name + " (" + joined + ")";
}

json eqBands(const json& bands) {
  json out = json::array();
  if (!bands.is_array()) return out;
  for (const json& b : bands) {
    const json en = get(b, "enabled");
    if (en.is_boolean() && !en.get<bool>()) continue;  // `b.get("enabled", True) is False`
    const json t = get(b, "type");
    json e = {{"type", t}, {"freqHz", getNum(b, "freq", 0.0)}, {"q", getNum(b, "q", 0.707)}};
    if (t == "highPass" || t == "lowPass") e["slopeDbPerOct"] = 12;
    else e["gainDb"] = getNum(b, "gainDb", 0.0);
    out.push_back(std::move(e));
  }
  return out;
}

std::string eqLine(const json& e) {
  const json t = get(e, "type");
  if (e.contains("slopeDbPerOct")) {
    const char* name = t == "highPass" ? "high-pass" : "low-pass";
    return std::string(name) + " " + g(e["freqHz"].get<double>()) + " Hz, " + e["slopeDbPerOct"].dump() + " dB/oct (Q " + g(e["q"].get<double>(), 3) + ")";
  }
  std::string name = pyStr(t);
  if (t == "peak") name = "peak";
  else if (t == "lowShelf") name = "low shelf";
  else if (t == "highShelf") name = "high shelf";
  return name + " " + g(e["freqHz"].get<double>()) + " Hz, " + signed1(e["gainDb"].get<double>()) + " dB, Q " + g(e["q"].get<double>(), 3);
}

json stage(const char* name, const char* position, json settings, std::string hardware) {
  return {{"stage", name}, {"position", position}, {"inModel", false}, {"settings", std::move(settings)}, {"hardware", std::move(hardware)}};
}

json gateStage(const json& gate) {
  const double thr = getNum(gate, "thresholdDb", -55.0);
  const double hyst = getNum(gate, "hysteresisDb", 6.0);
  json s = {{"mode", has(gate, "mode") ? get(gate, "mode") : json("gate")}, {"thresholdDb", thr}, {"closeThresholdDb", thr - hyst},
            {"hysteresisDb", hyst}, {"attackMs", getNum(gate, "attackMs", 0.5)}, {"holdMs", getNum(gate, "holdMs", 20.0)},
            {"releaseMs", getNum(gate, "releaseMs", 60.0)}, {"rangeDb", getNum(gate, "rangeDb", -90.0)},
            {"keyedOn", "DI (guitar signal before any pedal/amp)"}};
  const bool expander = s["mode"] == "expander";
  if (expander) s["ratio"] = getNum(gate, "ratio", 4.0);
  if (truthy(get(gate, "keyHighPassHz"))) s["keyHighPassHz"] = getNum(gate, "keyHighPassHz", 0.0);
  if (truthy(get(gate, "releaseCurve"))) s["releaseCurve"] = get(gate, "releaseCurve");
  std::string h = std::string(expander ? "Expander" : "Gate") + " FIRST in the chain, keyed on the guitar (DI) before any pedal: open at " + g(thr) +
                  " dB, close at " + g(s["closeThresholdDb"].get<double>()) + " dB (hysteresis " + g(hyst) + " dB), attack " + g(s["attackMs"].get<double>()) +
                  " ms, hold " + g(s["holdMs"].get<double>()) + " ms, release " + g(s["releaseMs"].get<double>()) + " ms, range " + g(s["rangeDb"].get<double>()) +
                  " dB";
  if (s.contains("ratio")) h += ", ratio " + g(s["ratio"].get<double>()) + ":1";
  if (s.contains("keyHighPassHz")) h += ", key high-pass " + g(s["keyHighPassHz"].get<double>()) + " Hz";
  return stage("gate", kBefore, std::move(s), h + ".");
}

json cabStage(const json& cab, const std::string& irName) {
  const json cm = get(cab, "mode");
  json s = {{"cabMode", cm}, {"normalize", has(cab, "normalize") ? get(cab, "normalize") : json(true)}};
  std::string h;
  if (cm == "irMix") {
    const json a = captureInfo(get(cab, "irA")), b = captureInfo(get(cab, "irB"));
    const double mix = getNum(cab, "mix", 0.5);
    s["irA"] = a;
    s["irB"] = b;
    s["mix"] = mix;
    // offsetSamplesB / invertB: core hook of v0.4M B2.1; read when present.
    for (const char* k : {"offsetSamplesB", "invertB"})
      if (has(cab, k)) s[k] = get(cab, k);
    h = "Load the cab IR: two mic IRs mixed into one, " + g((1 - mix) * 100) + "% A + " + g(mix * 100) + "% B (A = " + capLabel(a) + "; B = " + capLabel(b) + ")";
    std::vector<std::string> extra;
    if (truthy(get(cab, "offsetSamplesB"))) extra.push_back("B offset " + pyStr(get(cab, "offsetSamplesB")) + " samples");
    if (truthy(get(cab, "invertB"))) extra.push_back("B polarity inverted");
    if (!extra.empty()) {
      h += "; ";
      for (std::size_t i = 0; i < extra.size(); ++i) h += (i ? ", " : "") + extra[i];
    }
  } else {
    const json ir = captureInfo(get(cab, "ir"));
    s["ir"] = ir;
    h = "Load the cab IR " + capLabel(ir);
  }
  if (!irName.empty()) {
    s["exportedIr"] = irName;
    h += ". Easiest: load the exported " + irName +
         " instead; it already holds this cab (and the post EQ) as one IR, mono 48 kHz 32-bit float, loaded WITHOUT loudness normalisation";
  }
  return stage("cab", kAfter, std::move(s), h + ".");
}

json postEqStage(const json& bands, bool folded, const std::string& irName) {
  std::string h = "Post EQ after the cab: ";
  for (std::size_t i = 0; i < bands.size(); ++i) h += (i ? "; " : "") + eqLine(bands[i]);
  if (folded)
    h += ". Already folded into the exported IR (" + (irName.empty() ? std::string("the .ir.wav") : irName) +
         "); add it again only if you do not use that IR";
  return stage("postEq", kAfter, {{"bands", bands}, {"foldedIntoExportedIr", folded}}, h + ".");
}

json compStage(const json& c, double outGainDb, bool gainBefore) {
  const double thr = getNum(c, "thresholdDb", -12.0);
  json s = {{"thresholdDb", thr}, {"thresholdReference", "pre-headroom chain level (the 6 dB sum headroom is not counted)"},
            {"ratio", getNum(c, "ratio", 2.0)}, {"attackMs", getNum(c, "attackMs", 10.0)}, {"releaseMs", getNum(c, "releaseMs", 100.0)},
            {"kneeDb", getNum(c, "kneeDb", 6.0)}, {"makeupDb", getNum(c, "makeupDb", 0.0)}, {"detector", "peak, feed-forward, soft knee"}};
  // The compressor sees the level before the output gain; the no-cab export has that gain baked in before the IR.
  const double thrOut = thr + (gainBefore ? outGainDb : 0.0);
  s["thresholdDbFsOut"] = thrOut;
  std::string h = "Bus compressor LAST (after the cab / post EQ): threshold " + g(thr) + " dB re the chain's pre-headroom level = " + g(thrOut) +
                  " dB re 0 dBFS at the exported output, ratio " + g(s["ratio"].get<double>()) + ":1, attack " + g(s["attackMs"].get<double>()) +
                  " ms, release " + g(s["releaseMs"].get<double>()) + " ms, knee " + g(s["kneeDb"].get<double>()) + " dB, make-up " +
                  signed1(s["makeupDb"].get<double>()) + " dB, peak detector";
  if (gainBefore && outGainDb != 0.0) h += " (the output gain of " + signed1(outGainDb) + " dB is already inside the model)";
  return stage("busComp", kAfter, std::move(s), h + ".");
}

// Tolerant string member of foreign JSON.
std::string strOf(const json& j, const char* key, const std::string& def = {}) {
  const json v = get(j, key);
  return v.is_string() ? v.get<std::string>() : def;
}

}  // namespace

json buildExportNotesFromJson(const json& preset, const std::string& mode, bool dropComp, const std::string& namName, const std::string& irName) {
  const bool nocab = mode == "nocab";
  json stages = json::array();

  const json gate = get(preset, "gate");
  if (gate.is_object() && (!has(gate, "enabled") || truthy(get(gate, "enabled")))) stages.push_back(gateStage(gate));  // the gate is never trained

  const json cab = objectOrEmpty(preset, "cab");
  const json eq = eqBands(get(preset, "postEq"));
  const double outGain = getNum(objectOrEmpty(preset, "output"), "gainDb", 0.0);
  if (nocab) {
    const json en = get(cab, "enabled");
    if (!(en.is_boolean() && !en.get<bool>())) stages.push_back(cabStage(cab, irName));
    if (!eq.empty()) stages.push_back(postEqStage(eq, true, irName));
  }
  const json comp = objectOrEmpty(preset, "busComp");
  if (truthy(get(comp, "enabled")) && (nocab || dropComp)) stages.push_back(compStage(comp, outGain, /*gainBefore=*/nocab));

  bool haveGate = false, haveCab = false, havePostEq = false, haveComp = false;
  for (const json& st : stages) {
    const std::string n = st["stage"].get<std::string>();
    haveGate = haveGate || n == "gate";
    haveCab = haveCab || n == "cab";
    havePostEq = havePostEq || n == "postEq";
    haveComp = haveComp || n == "busComp";
  }
  std::vector<std::string> parts;
  if (haveGate) parts.push_back("gate");
  parts.push_back(!namName.empty() ? "NAM (" + namName + ")" : std::string("NAM model"));
  if (haveCab) {
    parts.push_back(!irName.empty() && havePostEq ? "cab IR + post EQ (" + irName + ")" : !irName.empty() ? "cab IR (" + irName + ")" : std::string("cab IR"));
    if (havePostEq && irName.empty()) parts.back() = "cab IR -> post EQ";
  } else if (havePostEq) {
    parts.push_back("post EQ");
  }
  if (haveComp) parts.push_back("bus comp");
  std::string order = "Loader order: ";
  for (std::size_t i = 0; i < parts.size(); ++i) order += (i ? " -> " : "") + parts[i];

  json notes = {{"version", kExportNotesVersion}, {"mode", mode}, {"stages", stages}, {"loaderOrder", order}};
  if (stages.empty()) notes["message"] = kNothing;
  // Which dynamics set the rig (and so the model) follows. Only written for "live": a record rig has no key, so notes of
  // presets without dynamicsMode stay identical to the exporter's (the parity fixtures).
  if (strOf(preset, "dynamicsMode") == "live") notes["dynamics"] = "live";
  return notes;
}

json buildExportNotes(const Preset& preset, const std::string& mode, bool dropComp, const std::string& namName, const std::string& irName) {
  return buildExportNotesFromJson(toJson(resolveDynamics(preset)), mode, dropComp, namName, irName);
}

bool exportNotesUsable(const json& notes) {
  if (!notes.is_object()) return false;
  const json v = get(notes, "version");
  return v.is_number_integer() && v.get<long long>() == kExportNotesVersion && get(notes, "stages").is_array() && get(notes, "loaderOrder").is_string();
}

std::string formatNotesTxt(const json& notes, const std::string& presetName, const std::string& licenceNote) {
  const json mode = get(notes, "mode");
  std::string t = "Sawblade export notes" + (presetName.empty() ? std::string() : " - " + presetName) + " (" + pyStr(mode) + " export)\n\n" +
                  "Stages of the preset that are NOT in the trained model, in signal order:\n\n";
  const json stages = get(notes, "stages");
  if (!stages.is_array() || stages.empty()) t += strOf(notes, "message", kNothing) + "\n\n";
  if (stages.is_array()) {
    int i = 0;
    for (const json& st : stages) {
      ++i;
      t += std::to_string(i) + ". " + strOf(st, "stage") + " [" + strOf(st, "position") + "]\n   " + strOf(st, "hardware") + "\n\n";
    }
  }
  t += strOf(notes, "loaderOrder") + "\n\n" + (licenceNote.empty() ? std::string(kExportNotesDisclaimer) : licenceNote) + "\n";
  return t;
}

}  // namespace sawblade::plugin
