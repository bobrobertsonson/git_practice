#include "sawblade/preset_reader.h"

#include <cmath>
#include <sstream>

namespace sawblade {
namespace {

std::string fmt(double v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

}  // namespace

JsonObject::JsonObject(const nlohmann::json& j, std::string path) : j_(&j), path_(std::move(path)) {
  if (!j.is_object()) throw PresetError(path_, "must be an object");
}

std::string JsonObject::child(std::string_view key) const {
  return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
}

std::string JsonObject::index(const std::string& path, std::size_t i) { return path + "[" + std::to_string(i) + "]"; }

const nlohmann::json* JsonObject::take(std::string_view key) {
  auto it = j_->find(key);
  if (it == j_->end()) return nullptr;
  seen_.emplace(key);
  return &*it;
}

double JsonObject::requireNumber(std::string_view key, double lo, double hi) {
  const auto* v = take(key);
  if (!v) throw PresetError(child(key), "required field is missing");
  if (!v->is_number()) throw PresetError(child(key), "must be a number");
  const double d = v->get<double>();
  if (!std::isfinite(d) || d < lo || d > hi)
    throw PresetError(child(key), "value " + fmt(d) + " out of range [" + fmt(lo) + ", " + fmt(hi) + "]");
  return d;
}

double JsonObject::number(std::string_view key, double def, double lo, double hi) {
  return has(key) ? requireNumber(key, lo, hi) : def;
}

int JsonObject::requireInteger(std::string_view key, int lo, int hi) {
  const auto* v = take(key);
  if (!v) throw PresetError(child(key), "required field is missing");
  if (!v->is_number_integer()) throw PresetError(child(key), "must be an integer");
  const auto d = v->get<long long>();
  if (d < lo || d > hi)
    throw PresetError(child(key), "value " + std::to_string(d) + " out of range [" + std::to_string(lo) + ", " +
                                      std::to_string(hi) + "]");
  return static_cast<int>(d);
}

int JsonObject::integer(std::string_view key, int def, int lo, int hi) {
  return has(key) ? requireInteger(key, lo, hi) : def;
}

bool JsonObject::boolean(std::string_view key, bool def) {
  const auto* v = take(key);
  if (!v) return def;
  if (!v->is_boolean()) throw PresetError(child(key), "must be a boolean");
  return v->get<bool>();
}

std::string JsonObject::requireString(std::string_view key) {
  const auto* v = take(key);
  if (!v) throw PresetError(child(key), "required field is missing");
  if (!v->is_string()) throw PresetError(child(key), "must be a string");
  return v->get<std::string>();
}

std::string JsonObject::string(std::string_view key, const std::string& def) {
  return has(key) ? requireString(key) : def;
}

std::string JsonObject::requireOneOf(std::string_view key, std::initializer_list<const char*> options) {
  const std::string s = requireString(key);
  std::string list;
  for (const char* o : options) {
    if (s == o) return s;
    list += (list.empty() ? "\"" : ", \"") + std::string(o) + "\"";
  }
  throw PresetError(child(key), "invalid value \"" + s + "\"; expected one of " + list);
}

std::string JsonObject::oneOf(std::string_view key, const std::string& def, std::initializer_list<const char*> options) {
  return has(key) ? requireOneOf(key, options) : def;
}

const nlohmann::json* JsonObject::optionalArray(std::string_view key, std::size_t maxSize) {
  const auto* v = take(key);
  if (!v) return nullptr;
  if (!v->is_array()) throw PresetError(child(key), "must be an array");
  if (v->size() > maxSize) throw PresetError(child(key), "too many elements (" + std::to_string(v->size()) + ", max " + std::to_string(maxSize) + ")");
  return v;
}

const nlohmann::json& JsonObject::requireArray(std::string_view key, std::size_t maxSize) {
  const auto* v = optionalArray(key, maxSize);
  if (!v) throw PresetError(child(key), "required field is missing");
  return *v;
}

JsonObject JsonObject::requireObject(std::string_view key) {
  const auto* v = take(key);
  if (!v) throw PresetError(child(key), "required field is missing");
  return JsonObject(*v, child(key));
}

std::optional<JsonObject> JsonObject::optionalObject(std::string_view key) {
  const auto* v = take(key);
  if (!v) return std::nullopt;
  return JsonObject(*v, child(key));
}

void JsonObject::finish() {
  for (auto it = j_->begin(); it != j_->end(); ++it)
    if (!seen_.contains(it.key())) throw PresetError(child(it.key()), "unknown key");
}

}  // namespace sawblade
