// Developer tool (built with the tests, not installed): writes the small-signal magnitude response
// of a modeled-pedal block as CSV (freq_hz,mag_db).
//   pedal_fr --type pedal.hm --param low=10 --param high=10 --param distortion=10 --out x.csv
// The block is built through the BlockRegistry exactly like a preset block, so parameter ranges
// and defaults are the preset's. Method: impulse at -90 dBFS, 65536 samples, fs = 48 kHz.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "pedal_fr_util.h"
#include "sawblade/block_registry.h"

using namespace sawblade;

int main(int argc, char** argv) {
  std::string type, out;
  nlohmann::json params = nlohmann::json::object();
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
    } else if (a == "--param") {
      const std::string kv = next();
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::cerr << "pedal_fr: --param expects key=value\n";
        return 2;
      }
      const std::string val = kv.substr(eq + 1);
      char* end = nullptr;
      const double num = std::strtod(val.c_str(), &end);
      // A value that does not parse fully as a number is passed as a JSON string (enums).
      if (!val.empty() && end == val.c_str() + val.size()) params[kv.substr(0, eq)] = num;
      else params[kv.substr(0, eq)] = val;
    } else {
      std::cerr << "usage: pedal_fr --type pedal.hm|pedal.ts|pedal.hmx|pedal.eye [--param key=value]... --out file.csv\n";
      return 2;
    }
  }
  if (type.empty() || out.empty()) {
    std::cerr << "usage: pedal_fr --type pedal.hm|pedal.ts|pedal.hmx|pedal.eye [--param key=value]... --out file.csv\n";
    return 2;
  }
  try {
    const BlockType* t = BlockRegistry::instance().find(type);
    if (!t) {
      std::cerr << "pedal_fr: unknown block type " << type << "\n";
      return 2;
    }
    nlohmann::json j = {{"id", "x"}, {"type", type}, {"params", params}};
    JsonObject o(j, "block");
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
    const auto db = test::smallSignalResponseDb(*proc);
    std::ofstream f(out);
    if (!f) {
      std::cerr << "pedal_fr: cannot write " << out << "\n";
      return 1;
    }
    f << "freq_hz,mag_db\n";
    for (std::size_t k = 1; k < db.size(); ++k)
      f << (static_cast<double>(k) * test::kFrFs / static_cast<double>(test::kFrN)) << "," << db[k] << "\n";
    std::cerr << "pedal_fr: " << type << " latency " << proc->latencySamples() << " samples; wrote " << out << "\n";
  } catch (const std::exception& e) {
    std::cerr << "pedal_fr: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
