#pragma once

// A fake `sawblade-match` / `sawblade-export` for the runner and editor tests: a Python script written into a temp
// dir at test time, steered by a "<script name>.cfg" JSON next to it. It answers --help (with or without
// --progress-json), writes progress.json / the log lines / result.json + the resolved presets like the real tools do,
// and can block on "gates" until the test creates `release-<gate>` in the job's --out folder (an export's run folder,
// which the exporter reports as `outDir`), which makes mid-run states deterministic. The "export" kind follows the
// phase 12 contract: --exports-root / --resume / --di builtin / --allow-inexact / --require-accept, --progress-json in
// the documented shape, exit 0 / 2 (cfg "exit") / 130 (SIGINT: the checkpoint stays) / 1 (cfg "fail"), signals.json
// listing the signals it got. Nothing here is committed audio or a capture.
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
import hashlib, json, os, shutil, signal, sys, time
here = os.path.dirname(os.path.abspath(__file__))
name = os.path.basename(__file__)
cfg = json.load(open(os.path.join(here, name + ".cfg")))
argv = sys.argv[1:]
if "--help" in argv:
    print("usage: " + name + " [-h] [--device D]" + (" [--progress-json PATH]" if cfg.get("progressJson") else "")
          + (" [--quick | --thorough]" if cfg.get("quickThorough") else ""))
    sys.exit(0)
if cfg.get("ignoreTerm"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
T0 = time.time()

def opt(n):
    return argv[argv.index(n) + 1] if n in argv else None

# match: --out is the job folder. export (phase 12): the run folder is --resume <dir>, else
# <--exports-root>/<preset stem>-<mode>-<size>-<ts> (the real exporter names it itself); --out still wins if given.
# v0.6 contract (docs/specs/v0_6-a2_everywhere.md decision 14): --arch a2|a1 (default a2), --size full|lite for a2
# (default full) or standard|lite|feather for a1 (default standard); anything else exits 64 with a message.
ARCH = opt("--arch") or "a2"
SIZE = opt("--size") or ("full" if ARCH == "a2" else "standard")
if cfg["kind"] == "export":
    if ARCH not in ("a2", "a1") or SIZE not in (("full", "lite") if ARCH == "a2" else ("standard", "lite", "feather")):
        print("error: --size %s is not valid for --arch %s" % (SIZE, ARCH), file=sys.stderr, flush=True)
        sys.exit(64)
if cfg["kind"] == "export" and opt("--out") is None:
    if opt("--resume"):
        out = opt("--resume")
    else:
        stem0 = os.path.splitext(os.path.basename(argv[0]))[0].replace(".preset", "")
        out = os.path.join(opt("--exports-root"), "%s-%s-%s-%d" % (stem0, opt("--mode"), SIZE, int(time.time() * 1000)))
else:
    out = opt("--out")
os.makedirs(out, exist_ok=True)
json.dump({"argv": argv}, open(os.path.join(out, "argv.json"), "w"))

# The signals this process received, in order (the exporter's SIGINT = cancel contract is tested through this).
STOP = [False]
def on_signal(n, frame):
    p = os.path.join(out, "signals.json")
    got = json.load(open(p)) if os.path.exists(p) else []
    got.append(signal.Signals(n).name)
    json.dump(got, open(p, "w"))
    if not (cfg.get("ignoreInt") and n == signal.SIGINT):
        STOP[0] = True
if cfg["kind"] == "export":
    signal.signal(signal.SIGINT, on_signal)
    if not cfg.get("ignoreTerm"):
        signal.signal(signal.SIGTERM, on_signal)

if cfg.get("grandchild"):
    import subprocess
    gc = subprocess.Popen(["sleep", "120"])
    open(os.path.join(out, "grandchild.pid"), "w").write(str(gc.pid))

# The pass of a match run: "quick" / "thorough" if the flag was given, else a single run (phase 6a behaviour).
pas = "quick" if "--quick" in argv else ("thorough" if "--thorough" in argv else "single")
gates = cfg.get("gates" + pas.capitalize(), cfg.get("gates", [])) if pas != "single" else cfg.get("gates", [])

def gate(g):
    if g not in gates:
        return
    path = os.path.join(out, "release-" + g)
    t0 = time.time()
    while not os.path.exists(path) and not STOP[0] and time.time() - t0 < 120:
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
    if cfg.get("fail") or (pas == "thorough" and cfg.get("failThorough")):
        print("error: pool needs amps and cabs, got {}", file=sys.stderr, flush=True)
        sys.exit(3)
    print("[   1.0s] stage2 blend [1/3] A + B (screen loss 4.2)", flush=True)
    progress("stage 2: fine-tuning", 0.6, "refining 1/3", 60.0, 4.2)
    gate("g2")
    src = json.load(open(cfg["presetSrc"]))
    label = {"single": "match", "quick": "quick", "thorough": "refined"}[pas]
    loss = {"single": (3.21, 3.9, 4.4), "quick": (4.0, 4.6, 5.1), "thorough": (2.9, 3.4, 3.8)}[pas]
    def preset(fname, nm, delta=0.0):
        p = json.loads(json.dumps(src)); p["name"] = nm
        if delta:  # the thorough best differs from the seed by this many dB on both path levels
            for k in ("a", "b"):
                p["paths"][k]["levelDb"] = p["paths"][k].get("levelDb", 0.0) + delta
        json.dump(p, open(os.path.join(out, fname), "w"), indent=2)
    preset("best.preset.resolved.json", label + " best", cfg.get("thoroughLevelDb", 0.0) if pas == "thorough" else 0.0)
    preset("alt1.preset.resolved.json", label + " alt 1")
    preset("alt2.preset.resolved.json", label + " alt 2")
    caps = lambda *t: {k: {"title": v} for k, v in t}
    result = {"schema": "sawblade.match_result", "version": 1,
      "best": {"stage": "refined", "topology": "blend", "loss": loss[0], "blend": 0.62,
               "captures": caps(("a_pedal", "HM-2 Chainsaw"), ("a_amp", "JCM800 2203"), ("b_pedal", "TS808"), ("b_amp", "5150III"), ("cab", "V30 4x12")),
               "preset": "best.preset.resolved.json"},
      "alternatives": [
        {"stage": "refined", "topology": "single", "loss": loss[1], "blend": 0.0,
         "captures": caps(("pedal", "HM-2 Chainsaw"), ("amp", "JCM800 2203"), ("cab", "V30 4x12")), "file": "alt1.preset.resolved.json"},
        {"stage": "screened", "topology": "single2", "loss": loss[2], "blend": 0.0,
         "captures": caps(("pedal1", "TS808"), ("pedal2", "HM-2 Chainsaw"), ("amp", "5150III"), ("cab", "V30 4x12")), "file": "alt2.preset.resolved.json"}]}
    json.dump(result, open(os.path.join(out, "result.json"), "w"), indent=2)
    print("[   2.0s] done in 0.0 min; results in " + out, flush=True)
else:
    # sawblade-export, as far as the plugin cares: --progress-json in the phase 12 shape, the checkpoint folder, exit
    # 0 / 2 / 130 / 1, SIGINT = stop and keep the checkpoint, a .nam with a metadata.sawblade block, export_report.json.
    mode, size, arch = opt("--mode"), SIZE, ARCH
    preset_file = argv[0]
    pj = opt("--progress-json")
    epochs = cfg.get("epochs", 10)
    nc = bool(cfg.get("nonCommercial"))
    ck = os.path.join(out, "checkpoint")
    os.makedirs(ck, exist_ok=True)
    stem = os.path.splitext(os.path.basename(preset_file))[0].replace(".preset", "") + ("-nc" if nc else "") + "-" + mode + "-" + size
    state = {"epoch": 0, "best": None}
    def prog_json(stage, frac, msg="", eta=-1, resumable=None):
        if not pj:
            return
        atomic(pj, {"stage": stage, "fraction": frac, "etaSeconds": eta, "epoch": state["epoch"], "epochs": epochs, "arch": arch,
                    "bestEsr": state["best"], "message": msg, "outDir": os.path.abspath(out),
                    "resumable": os.path.exists(os.path.join(ck, "last.ckpt")) if resumable is None else resumable,
                    "elapsedSeconds": round(time.time() - T0, 1)})
    def ckpt(epoch, interrupted=False):
        state["epoch"] = epoch
        state["best"] = 0.012 - 0.0005 * epoch
        open(os.path.join(ck, "last.ckpt"), "w").write("weights")
        atomic(os.path.join(ck, "progress.json"), {"progressVersion": 1, "epoch": epoch, "bestEpoch": epoch, "bestValEsr": state["best"],
               "elapsedTrainingS": 30.0 * epoch, "complete": False, "interrupted": interrupted,
               "config": {"epochs": epochs, "maxMinutes": None, "device": "cpu"}})
    def cancelled():
        ckpt(state["epoch"], interrupted=True)
        prog_json("cancelled", 0.1 + 0.8 * state["epoch"] / epochs, "interrupted", -1, True)
        print("interrupted: checkpoint kept", flush=True)
        sys.exit(130)
    def frac(epoch):
        return 0.1 + 0.8 * epoch / epochs
    print("export " + arch + "/" + mode + "/" + size + " -> " + out, flush=True)
    if opt("--resume"):
        rp = json.load(open(os.path.join(ck, "progress.json")))
        state["epoch"], state["best"] = rp["epoch"], rp.get("bestValEsr")
        print("resuming from epoch %d" % state["epoch"], flush=True)
    prog_json("plan", 0.01, "plan")
    prog_json("signal", 0.04, "signal")
    prog_json("render", 0.08, "render")
    if cfg.get("fail"):
        print("error: the preset has no cab", file=sys.stderr, flush=True)
        prog_json("error", 0.08, "the preset has no cab")
        sys.exit(1)
    for e, g in ((3, "g1"), (7, "g2")):
        if e <= state["epoch"]:
            continue
        ckpt(e)
        prog_json("train", frac(e), "epoch %d" % e, 42.0)
        print("  epoch %3d  val ESR %.5f  (%d s)" % (e, state["best"], 30 * e), flush=True)
        gate(g)
        if STOP[0]:
            cancelled()
    ckpt(epochs)
    prog_json("validate", 0.95, "validating", 5.0)
    sha = hashlib.sha256(open(preset_file, "rb").read()).hexdigest()
    def write_nam(path):
        json.dump({"version": "0.5.4", "architecture": "WaveNet", "config": {}, "weights": [0.0],
                   "metadata": {"name": stem, "sawblade": {"exporter": "sawblade-export", "preset": {"name": stem, "sha256": sha},
                                "exportMode": mode, "arch": arch, "size": size, "nonCommercial": nc, "attribution": [], "licenceNote": "for your own use"}}},
                  open(path, "w"))
    # a1: <stem>.nam. a2: the container plus the standalone Full and Lite files; the primary is the one --size names.
    files = {}
    if arch == "a2":
        files = {"container": stem + ".a2.nam", "full": stem + ".a2_full.nam", "lite": stem + ".a2_lite.nam"}
        files["primary"] = files[size]
        for k in ("container", "full", "lite"):
            write_nam(os.path.join(out, files[k]))
    else:
        files = {"primary": stem + ".nam"}
        write_nam(os.path.join(out, files["primary"]))
    nam = os.path.join(out, files["primary"])
    code = int(cfg.get("exit", 0))
    ok = code != 2
    judged = arch == "a2" or size == "standard"
    status = cfg.get("acceptance") or (("met" if ok else "NOT MET") if judged else "not judged (non-standard size)")
    held, ltas = (0.0123, 0.31) if ok else (0.0345, 0.92)
    acc = {"status": status, "summary": "acceptance %s: held-out ESR %.4f (limit 0.02), DI-excerpt LTAS error %.2f dB (limit 0.5)" % (status, held, ltas),
           "heldOutEsr": held, "diLtasDb": ltas, "esrLimit": 0.02, "ltasLimitDb": 0.5}
    # a1: validation.acceptance. a2 (decision 14): one entry per standalone file, validation.{full,lite}.acceptance.
    validation = {"acceptance": acc} if arch == "a1" else {"full": {"acceptance": acc}, "lite": {"acceptance": dict(acc, heldOutEsr=held + 0.004)}}
    report = {"reportVersion": 1, "tool": "sawblade-export", "mode": mode, "arch": arch, "size": size, "files": files, "nonCommercial": nc,
              "preset": {"path": os.path.abspath(preset_file), "sha256": sha},
              "training": {"namFile": os.path.basename(nam), "epochsDone": epochs, "wallSeconds": cfg.get("trainWall", 100.0)},
              "validation": validation,
              "totalWallSeconds": cfg.get("wall", 150.0)}
    if cfg.get("exportNotes"):
        report["exportNotes"] = cfg["exportNotes"]
        if cfg.get("anagramProfile"):
            report["exportNotes"] = dict(cfg["exportNotes"], deviceProfiles={"anagram": cfg["anagramProfile"]})
    json.dump(report, open(os.path.join(out, "export_report.json"), "w"), indent=2)
    listen = cfg.get("listen")
    if listen:
        os.makedirs(os.path.join(out, "listen"), exist_ok=True)
        open(os.path.join(out, "listen", "ab_original_then_export." + listen), "w").write("audio")
    shutil.rmtree(ck)
    prog_json("done", 1.0, "done", 0, False)
    print("report: " + os.path.join(out, "export_report.json"), flush=True)
    sys.exit(code)
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
  // A matcher whose --help lists --quick / --thorough (and --progress-json): MATCH runs two passes.
  void cfgTwoPass(nlohmann::json c = nlohmann::json::object()) const {
    c["quickThorough"] = true;
    c["progressJson"] = true;
    cfgMatch(std::move(c));
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
