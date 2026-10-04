// Developer tool (built with the tests, not installed): small-signal magnitude response (freq_hz,mag_db)
// or THD sweep of a modeled-pedal block as CSV.
//   pedal_fr --type pedal.hm --param low=10 --param high=10 --param mode=custom --out x.csv
//   pedal_fr --preset presets/modeled/chainsaw/classic_buzzsaw.json [--block a1] --out x.csv
//   pedal_fr --type pedal.muff --param clip=led --thd --out x.csv     (input_dbfs,thd_db,h2_dbc)
// The block is built through the BlockRegistry exactly like a preset block, so parameter ranges
// and defaults are the preset's. A --param value that does not parse as a number is passed as a
// string (enums). --preset takes the block's params from a preset file (first circuit block, i.e.
// pedal.hm / pedal.muff, unless --block names an id); --param overrides on top; --type then comes
// from the block. FR method: impulse at -90 dBFS, 65536 samples, fs = 48 kHz. THD: a 500 Hz sine
// from -40 to 0 dBFS in 2 dB steps (1 s each, 0.25 s discarded), harmonics 2-20.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "pedal_fr_util.h"
#include "sawblade/block_registry.h"
#include "sawblade/preset.h"

using namespace sawblade;

namespace {
const char* kUsage =
    "usage: pedal_fr (--type pedal.hm|pedal.muff|pedal.ts | --preset FILE [--block ID]) [--param key=value]... [--thd] --out file.csv\n";
}

int main(int argc, char** argv) {
  std::string type, out, presetFile, blockId;
  bool thd = false;
  nlohmann::json overrides = nlohmann::json::object();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "pedal_fr: missing value after " << a << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--type") {
      type = next();
    } else if (a == "--out") {
      out = next();
    } else if (a == "--preset") {
      presetFile = next();
    } else if (a == "--block") {
      blockId = next();
    } else if (a == "--thd") {
      thd = true;
    } else if (a == "--param") {
      const std::string kv = next();
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::cerr << "pedal_fr: --param expects key=value\n";
        return 2;
      }
      const std::string v = kv.substr(eq + 1);
      char* end = nullptr;
      const double d = std::strtod(v.c_str(), &end);
      if (!v.empty() && end == v.c_str() + v.size()) overrides[kv.substr(0, eq)] = d;
      else overrides[kv.substr(0, eq)] = v;
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (out.empty() || (type.empty() && presetFile.empty())) {
    std::cerr << kUsage;
    return 2;
  }
  try {
    nlohmann::json blockJson = {{"id", "x"}, {"params", nlohmann::json::object()}};
    if (!presetFile.empty()) {
      const Preset pr = loadPresetFile(presetFile);
      const Block* found = nullptr;
      for (const PathPreset* path : {&pr.a, &pr.b}) {
        for (const Block& b : path->blocks) {
          const bool match = blockId.empty() ? (b.type == "pedal.hm" || b.type == "pedal.muff") : b.id == blockId;
          if (match && !found) found = &b;
        }
      }
      if (!found) {
        std::cerr << "pedal_fr: no " << (blockId.empty() ? "circuit block" : "block " + blockId) << " in " << presetFile << "\n";
        return 2;
      }
      const nlohmann::json bj = found->params->toJson();
      type = found->type;
      blockJson["modelVersion"] = bj["modelVersion"];
      blockJson["params"] = bj["params"];
    }
    for (auto it = overrides.begin(); it != overrides.end(); ++it) blockJson["params"][it.key()] = it.value();
    blockJson["type"] = type;
    if (!blockJson.contains("modelVersion") && type == "pedal.hm") blockJson["modelVersion"] = 2;  // --type builds the current model

    const BlockType* t = BlockRegistry::instance().find(type);
    if (!t) {
      std::cerr << "pedal_fr: unknown block type " << type << "\n";
      return 2;
    }
    JsonObject o(blockJson, "block");
    Block b;
    b.id = "x";
    b.type = type;
    b.params = t->parse(o, {});
    o.take("id");
    o.take("type");
    o.finish();
    BlockBuildContext ctx;
    ctx.sampleRate = test::kFrFs;
    auto proc = t->create(b, ctx);
    std::ofstream f(out);
    if (!f) {
      std::cerr << "pedal_fr: cannot write " << out << "\n";
      return 1;
    }
    if (thd) {
      proc->prepare({test::kFrFs, 512});
      f << "input_dbfs,thd_db,h2_dbc\n";
      for (int db = -40; db <= 0; db += 2) {
        const auto r = test::thdPoint(*proc, db);
        f << db << "," << r.thdDb << "," << r.h2Dbc << "\n";
      }
    } else {
      const auto db = test::smallSignalResponseDb(*proc);
      f << "freq_hz,mag_db\n";
      for (std::size_t k = 1; k < db.size(); ++k)
        f << (static_cast<double>(k) * test::kFrFs / static_cast<double>(test::kFrN)) << "," << db[k] << "\n";
    }
    std::cerr << "pedal_fr: " << type << " latency " << proc->latencySamples() << " samples; wrote " << out << "\n";
  } catch (const std::exception& e) {
    std::cerr << "pedal_fr: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
