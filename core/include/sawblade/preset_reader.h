#pragma once

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace sawblade {

// Thrown for any preset problem (schema, type, range, unknown key, version, duplicate id...).
// what() is "<json path>: <message>"; the path is also available on its own.
class PresetError : public std::runtime_error {
 public:
  PresetError(std::string jsonPath, const std::string& message)
      : std::runtime_error((jsonPath.empty() ? std::string("$") : jsonPath) + ": " + message),
        path_(std::move(jsonPath)) {}
  const std::string& jsonPath() const noexcept { return path_; }

 private:
  std::string path_;
};

// Strict reader for one JSON object: every accessor type- and range-checks and throws
// PresetError with the full JSON path; finish() rejects keys nobody asked for.
// Used by the preset parser and by block-type parse hooks in the registry.
class JsonObject {
 public:
  JsonObject(const nlohmann::json& j, std::string path);

  const std::string& path() const noexcept { return path_; }
  std::string child(std::string_view key) const;                 // path of a member
  static std::string index(const std::string& path, std::size_t i);  // "path[i]"

  bool has(std::string_view key) const { return j_->contains(key); }

  // Optional members take a default; require* members throw when absent.
  double number(std::string_view key, double def, double lo, double hi);
  double requireNumber(std::string_view key, double lo, double hi);
  int integer(std::string_view key, int def, int lo, int hi);
  int requireInteger(std::string_view key, int lo, int hi);
  bool boolean(std::string_view key, bool def);
  std::string string(std::string_view key, const std::string& def);
  std::string requireString(std::string_view key);
  // Value must be one of `options`.
  std::string oneOf(std::string_view key, const std::string& def, std::initializer_list<const char*> options);
  std::string requireOneOf(std::string_view key, std::initializer_list<const char*> options);

  const nlohmann::json* optionalArray(std::string_view key, std::size_t maxSize);
  const nlohmann::json& requireArray(std::string_view key, std::size_t maxSize);
  // Nested objects (marks the key as consumed; the caller must finish() the returned reader).
  JsonObject requireObject(std::string_view key);
  std::optional<JsonObject> optionalObject(std::string_view key);

  // Marks `key` as consumed and returns it (nullptr if absent) without checking its type.
  const nlohmann::json* take(std::string_view key);

  // Throws PresetError naming the first member that was never consumed.
  void finish();

 private:
  const nlohmann::json* j_;
  std::string path_;
  std::set<std::string, std::less<>> seen_;
};

}  // namespace sawblade
