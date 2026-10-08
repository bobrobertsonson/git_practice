#include "sawblade/preset_calibrate.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "sawblade/preset.h"

namespace sawblade {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string lowerOf(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::vector<fs::path> jsonFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return files;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const std::string fn = lowerOf(it->path().filename().string());
    if (lowerOf(it->path().extension().string()) != ".json") continue;
    if (fn.size() >= 14 && fn.compare(fn.size() - 14, 14, ".resolved.json") == 0) continue;
    files.push_back(it->path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return lowerOf(a.filename().string()) < lowerOf(b.filename().string()); });
  return files;
}

bool readText(const fs::path& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

int BulkCalibrateResult::count(BulkCalibrateStatus s) const {
  return static_cast<int>(std::count_if(files.begin(), files.end(), [s](const BulkCalibrateEntry& e) { return e.status == s; }));
}

std::string atomicWriteText(const fs::path& file, const std::string& text) {
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o << text;
    o.flush();
    if (!o) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return "cannot write " + tmp.string();
    }
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return "cannot write " + file.string() + ": " + ec.message();
  }
  return {};
}

std::vector<fs::path> legacyPresetFiles(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const fs::path& f : jsonFiles(dir)) {
    try {
      if (loadPresetFile(f).calibrationMode == CalibrationMode::Legacy) out.push_back(f);
    } catch (const std::exception&) {
    }
  }
  return out;
}

BulkCalibrateResult calibrateLegacyPresets(const fs::path& dir, const AtomicWriteFn& write, const std::function<void(const BulkCalibrateEntry&)>& onFile) {
  BulkCalibrateResult res;
  const auto done = [&](BulkCalibrateEntry e) {
    if (onFile) onFile(e);
    res.files.push_back(std::move(e));
  };
  for (const fs::path& file : jsonFiles(dir)) {
    BulkCalibrateEntry e;
    e.file = file;
    json j;
    Preset before;
    try {
      std::string text;
      if (!readText(file, text)) throw std::runtime_error("cannot read " + file.string());
      j = json::parse(text);
      before = parsePreset(j, file.parent_path());
    } catch (const std::exception& ex) {  // a file that does not parse is left alone
      e.status = BulkCalibrateStatus::Unreadable;
      e.error = ex.what();
      done(std::move(e));
      continue;
    }
    if (before.calibrationMode == CalibrationMode::Calibrated) {
      e.status = BulkCalibrateStatus::AlreadyCalibrated;
      done(std::move(e));
      continue;
    }
    try {
      j["version"] = kPresetVersion;
      j["calibration"] = {{"mode", "calibrated"}};
      Preset expected = before;
      expected.calibrationMode = CalibrationMode::Calibrated;
      const Preset after = parsePreset(j, file.parent_path());
      if (after.calibrationMode != CalibrationMode::Calibrated || !(after == expected))
        throw std::runtime_error("the converted preset does not read back as the same preset");
      const std::string err = write(file, j.dump(2) + "\n");
      if (!err.empty()) throw std::runtime_error(err);
      e.status = BulkCalibrateStatus::Converted;
    } catch (const std::exception& ex) {  // reported; the rest still convert
      e.status = BulkCalibrateStatus::WriteFailed;
      e.error = ex.what();
    }
    done(std::move(e));
  }
  return res;
}

}  // namespace sawblade
