#include "settings/DeviceCalibration.h"

#include <cctype>
#include <locale>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace sawblade::plugin::settings {

const char* deviceMethodName(DeviceMethod m) noexcept {
  switch (m) {
    case DeviceMethod::Preset: return "preset";
    case DeviceMethod::Manual: return "manual";
    case DeviceMethod::Measured: return "measured";
  }
  return "manual";
}

namespace {
std::optional<DeviceMethod> methodFromName(std::string_view s) {
  if (s == "preset") return DeviceMethod::Preset;
  if (s == "manual") return DeviceMethod::Manual;
  if (s == "measured") return DeviceMethod::Measured;
  return std::nullopt;
}
}  // namespace

const std::vector<DevicePreset>& devicePresets() {
  static const std::vector<DevicePreset> table = {
      {"scarlett-4i4-3g", "Focusrite Scarlett 4i4 3rd gen, Inst", 12.5, false,
       "Focusrite Scarlett 4i4 3rd Gen user guide, instrument input max level at minimum gain: +12.5 dBu "
       "(https://userguides.focusrite.com/hc/en-gb/articles/23031514701842)"},
      {"scarlett-4i4-3g-pad", "Focusrite Scarlett 4i4 3rd gen, Inst + PAD", 14.0, true,
       "Focusrite Scarlett 4i4 3rd Gen user guide, instrument input max level at minimum gain with PAD: +14 dBu "
       "(https://fael-downloads-prod.focusrite.com/customer/prod/downloads/Scarlett%204i4%203rd%20Gen%20User%20Guide%20V2.pdf)"},
      {"scarlett-4i4-4g", "Focusrite Scarlett 4i4 4th gen, Inst", 12.0, false,
       "Focusrite Scarlett 4i4 4th Gen user guide, instrument input max level at minimum gain: +12 dBu "
       "(https://fael-downloads-prod.focusrite.com/customer/prod/downloads/scarlett_4i4_4th_gen_user_guide_v2-pdf-en.pdf)"},
  };
  return table;
}

const DevicePreset* findDevicePreset(std::string_view id) {
  for (const auto& p : devicePresets())
    if (id == p.id) return &p;
  return nullptr;
}

DbuCheck checkDeviceDbu(double dbu) {
  DbuCheck c;
  if (!std::isfinite(dbu)) {
    c.error = "not a number";
    return c;
  }
  if (dbu < kDbuMin || dbu > kDbuMax) {
    c.error = "outside the plausible range of -60 to +60 dBu";
    return c;
  }
  c.ok = true;
  if (dbu < kDbuWarnLow || dbu > kDbuWarnHigh) c.warning = "unusual for an instrument input (usually 0 to +24 dBu): check the figure";
  return c;
}

std::optional<double> parseDbuText(std::string_view text) {
  std::string t;
  for (char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch))) continue;
    t += ch == ',' ? '.' : ch;
  }
  // an optional trailing unit
  for (const char* unit : {"dbu", "DBU", "dBu", "dBU"}) {
    const std::string u(unit);
    if (t.size() > u.size() && t.compare(t.size() - u.size(), u.size(), u) == 0) {
      t.resize(t.size() - u.size());
      break;
    }
  }
  if (!t.empty() && t.front() == '+') t.erase(t.begin());
  if (t.empty()) return std::nullopt;
  // istringstream with the classic locale: locale-independent, and available for floating point on every libc++/libstdc++
  // (std::from_chars(double) is not on older libc++).
  std::istringstream in(t);
  in.imbue(std::locale::classic());
  double v = 0.0;
  in >> v;
  if (in.fail() || !in.eof() || !std::isfinite(v)) return std::nullopt;
  return v;
}

DeviceCalibrationRecord recordFromPreset(const DevicePreset& p, std::string date) {
  DeviceCalibrationRecord r;
  r.dbu = p.dbu;
  r.method = DeviceMethod::Preset;
  r.model = p.label;
  r.gainAtMinimum = true;
  r.pad = p.pad;
  r.air = false;  // Air off is part of the assumption (A1b: the max-input spec does not cover Air mode)
  r.date = std::move(date);
  return r;
}

DeviceCalibrationRecord recordFromValue(double dbu, DeviceMethod method, bool pad, bool air, std::string date) {
  DeviceCalibrationRecord r;
  r.dbu = dbu;
  r.method = method;
  r.gainAtMinimum = true;
  r.pad = pad;
  r.air = air;
  r.date = std::move(date);
  return r;
}

nlohmann::json recordToJson(const DeviceCalibrationRecord& r) {
  nlohmann::json j = {{"dbu", r.dbu}, {"method", deviceMethodName(r.method)}, {"model", r.model}, {"gainAtMinimum", r.gainAtMinimum},
                      {"pad", r.pad}, {"air", r.air}, {"date", r.date}};
  if (r.liveGateFloorDbfs) j["liveGateFloorDbfs"] = *r.liveGateFloorDbfs;
  if (r.driftBaselineDbfs) j["driftBaselineDbfs"] = *r.driftBaselineDbfs;
  return j;
}

std::optional<DeviceCalibrationRecord> recordFromJson(const nlohmann::json& j) {
  if (!j.is_object()) return std::nullopt;
  const auto num = [&](const char* k) -> std::optional<double> {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    const double v = it->get<double>();
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
  };
  const auto flag = [&](const char* k, bool def) {
    const auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : def;
  };
  const auto text = [&](const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  const auto dbu = num("dbu");
  if (!dbu || !checkDeviceDbu(*dbu).ok) return std::nullopt;
  DeviceCalibrationRecord r;
  r.dbu = *dbu;
  r.method = methodFromName(text("method")).value_or(DeviceMethod::Manual);
  r.model = text("model");
  r.gainAtMinimum = flag("gainAtMinimum", true);
  r.pad = flag("pad", false);
  r.air = flag("air", false);
  r.date = text("date");
  if (const auto f = num("liveGateFloorDbfs"); f && *f >= Gate::kFloorMinDb && *f <= Gate::kFloorMaxDb) r.liveGateFloorDbfs = *f;
  if (const auto d = num("driftBaselineDbfs"); d && *d >= drift::kMinDb && *d <= drift::kMaxDb) r.driftBaselineDbfs = *d;
  return r;
}

std::string todayDate() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[48];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

ChainCalibration chainCalibrationFor(bool calibratedInputLevels, const std::optional<DeviceCalibrationRecord>& record) {
  ChainCalibration c;
  c.enabled = calibratedInputLevels;
  if (record) c.device.dbu = record->dbu;
  return c;
}

EngineCalibration engineCalibrationFor(bool calibratedInputLevels, const std::optional<DeviceCalibrationRecord>& record) {
  EngineCalibration e;
  e.chain = chainCalibrationFor(calibratedInputLevels, record);
  if (calibratedInputLevels && record) e.gateFloorSeedDb = record->liveGateFloorDbfs;
  e.driftCheck = calibratedInputLevels && record.has_value();  // I3: the drift statistic runs only with calibration on and a record
  return e;
}

std::string uncalibratedNotice() { return "Interface not calibrated: assuming +12 dBu"; }

}  // namespace sawblade::plugin::settings
