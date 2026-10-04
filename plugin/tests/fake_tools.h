#pragma once

// A fake `sawblade-match` / `sawblade-export` for the runner and editor tests: a Python script written into a temp
// dir at test time, steered by a "<script name>.cfg" JSON next to it. It answers --help (with or without
// --progress-json), writes progress.json / the log lines / result.json + the resolved presets like the real tools do,
// and can block on "gates" until the test creates `release-<gate>` in the job's --out folder, which makes mid-run
// states deterministic. Nothing here is committed audio or a capture.
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "JobRunner.h"
#include "PresetMapping.h"
#include "sawblade/wav_io.h"

namespace fake_tools {

inline const char* kFakeTool = R"PY(#!/usr/bin/env python3
import json, os, shutil, signal, sys, time
here = os.path.dirname(os.path.abspath(__file__))
name = os.path.basename(__file__)
cfg = json.load(open(os.path.join(here, name + ".cfg")))
argv = sys.argv[1:]
if "--help" in argv:
    print("usage: " + name + " [-h] [--device D]" + (" [--progress-json PATH]" if cfg.get("progressJson") else ""))
    sys.exit(0)
if cfg.get("ignoreTerm"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

def opt(n):
    return argv[argv.index(n) + 1] if n in argv else None

out = opt("--out")
os.makedirs(out, exist_ok=True)
json.dump({"argv": argv}, open(os.path.join(out, "argv.json"), "w"))

def gate(g):
    if g not in cfg.get("gates", []):
        return
    path = os.path.join(out, "release-" + g)
    t0 = time.time()
    while not os.path.exists(path) and time.time() - t0 < 120:
        time.sleep(0.02)

def atomic(path, obj):
    tmp = path + ".tmp"
    json.dump(obj, open(tmp, "w"))
    os.replace(tmp, path)

if cfg["kind"] == "match":
    pj = opt("--progress-json")
    def progress(stage, frac, msg, eta, best):
        if pj:
            atomic(pj, {"stage": stage, "fraction": frac, "etaSeconds": eta, "bestErrorDb": best, "message": msg})
    print("[   0.1s] pool {'amps': 3}", flush=True)
    progress("stage 1: screening", 0.25, "screening 12 pairs", 120.0, None)
    print("[   0.2s] excerpt 1.0-7.0 s (loudest)", flush=True)
    gate("g1")
    if cfg.get("fail"):
        print("error: pool needs amps and cabs, got {}", file=sys.stderr, flush=True)
        sys.exit(3)
    print("[   1.0s] stage2 blend [1/3] A + B (screen loss 4.2)", flush=True)
    progress("stage 2: fine-tuning", 0.6, "refining 1/3", 60.0, 4.2)
    gate("g2")
    src = json.load(open(cfg["presetSrc"]))
    def preset(fname, nm):
        p = dict(src); p["name"] = nm
        json.dump(p, open(os.path.join(out, fname), "w"), indent=2)
    preset("best.preset.resolved.json", "match best")
    preset("alt1.preset.resolved.json", "match alt 1")
    preset("alt2.preset.resolved.json", "match alt 2")
    caps = lambda *t: {k: {"title": v} for k, v in t}
    result = {"schema": "sawblade.match_result", "version": 1,
      "best": {"stage": "refined", "topology": "blend", "loss": 3.21, "blend": 0.62,
               "captures": caps(("a_pedal", "HM-2 Chainsaw"), ("a_amp", "JCM800 2203"), ("b_pedal", "TS808"), ("b_amp", "5150III"), ("cab", "V30 4x12")),
               "preset": "best.preset.resolved.json"},
      "alternatives": [
        {"stage": "refined", "topology": "single", "loss": 3.9, "blend": 0.0,
         "captures": caps(("pedal", "HM-2 Chainsaw"), ("amp", "JCM800 2203"), ("cab", "V30 4x12")), "file": "alt1.preset.resolved.json"},
        {"stage": "screened", "topology": "single2", "loss": 4.4, "blend": 0.0,
         "captures": caps(("pedal1", "TS808"), ("pedal2", "HM-2 Chainsaw"), ("amp", "5150III"), ("cab", "V30 4x12")), "file": "alt2.preset.resolved.json"}]}
    json.dump(result, open(os.path.join(out, "result.json"), "w"), indent=2)
    print("[   2.0s] done in 0.0 min; results in " + out, flush=True)
else:
    ck = os.path.join(out, "checkpoint")
    os.makedirs(ck, exist_ok=True)
    def prog(epoch):
        atomic(os.path.join(ck, "progress.json"), {"progressVersion": 1, "epoch": epoch, "bestEpoch": epoch, "bestValEsr": 0.01,
               "elapsedTrainingS": 30.0 * epoch, "complete": False, "config": {"epochs": 10, "maxMinutes": None, "device": "cpu"}})
    print("export " + opt("--mode") + "/" + opt("--size") + " -> " + out, flush=True)
    prog(3)
    print("  epoch   3  val ESR 0.01000  (90 s)", flush=True)
    gate("g1")
    prog(7)
    print("  epoch   7  val ESR 0.00900  (210 s)", flush=True)
    gate("g2")
    open(os.path.join(out, "model.nam"), "w").write("{}")
    json.dump({"ok": True}, open(os.path.join(out, "export_report.json"), "w"))
    shutil.rmtree(ck)
    print("report: " + os.path.join(out, "export_report.json"), flush=True)
)PY";

// A valid, capture-free preset (Init) under another name: what the fake's "resolved presets" are made from.
inline std::filesystem::path writeSeedPreset(const std::filesystem::path& dir) {
  nlohmann::json j = nlohmann::json::parse(sawblade::plugin::presetToStateJson(sawblade::plugin::makeInitPreset()));
  j["name"] = "seed";
  const auto p = dir / "seed.preset.json";
  std::ofstream(p) << j.dump(2);
  return p;
}

struct Toolbox {
  std::filesystem::path root, match, exporter, pool, di, ref, presetSrc, jobs;
  sawblade::plugin::MatchSettings settings;

  explicit Toolbox(const std::filesystem::path& r) : root(r), settings(r / "settings.xml") {
    namespace fs = std::filesystem;
    fs::create_directories(root);
    jobs = root / "jobs";
    match = root / "bin" / "sawblade-match";
    exporter = root / "bin" / "sawblade-export";
    fs::create_directories(match.parent_path());
    for (const auto& p : {match, exporter}) {
      std::ofstream(p) << kFakeTool;
      fs::permissions(p, fs::perms::owner_all);
    }
    pool = root / "pool_manifest.json";
    std::ofstream(pool) << "{}";
    di = root / "di.wav";
    ref = root / "guitar.wav";
    sawblade::writeWavFloat32(di, 48000.0, std::vector<float>(480, 0.0f));
    sawblade::writeWavFloat32(ref, 48000.0, std::vector<float>(480, 0.0f));
    presetSrc = writeSeedPreset(root);
    settings.setMatchExecutable(match);
    settings.setExportExecutable(exporter);
    settings.setPoolManifest(pool);
  }
  void cfg(const char* tool, nlohmann::json c) const {
    c["presetSrc"] = presetSrc.string();
    std::ofstream((root / "bin" / (std::string(tool) + ".cfg"))) << c.dump();
  }
  void cfgMatch(nlohmann::json c) const {
    c["kind"] = "match";
    cfg("sawblade-match", std::move(c));
  }
  void cfgExport(nlohmann::json c) const {
    c["kind"] = "export";
    cfg("sawblade-export", std::move(c));
  }
  sawblade::plugin::MatchRequest request(std::optional<double> offset = 1234.5) const {
    sawblade::plugin::MatchRequest r;
    r.di = di;
    r.ref = ref;
    r.offsetMs = offset;
    r.referenceLabel = "Test Song (guitar stem: guitar.wav)";
    r.diLabel = "take-1";
    return r;
  }
};

inline void release(const std::filesystem::path& dir, const char* gate) { std::ofstream(dir / (std::string("release-") + gate)) << "1"; }

}  // namespace fake_tools
