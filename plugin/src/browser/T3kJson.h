#pragma once

// Pure parsing of the `sawblade-t3k --json` contract (docs/specs/phase8_capture_browser.md). No JUCE.
// Every parser returns an empty optional and fills `error` on malformed input; none throws.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/preset.h"

namespace sawblade::plugin::t3k {

struct ErrorInfo {
  std::string message;
  std::string code;  // CLI codes: license auth not_found network error; ours: launch timeout parse exit
};

struct CaptureRecord {
  std::int64_t toneId = 0;
  std::string title, creator, gear, format, license, createdAt, url, status;
  int favorites = 0, downloads = 0, modelsCount = 0, a2Count = 0, a1Count = 0, irsCount = 0;
  std::vector<std::string> sizes, reasons, flags;
  bool passes = true;  // a record without a verdict is shown
};

struct ModelInfo {
  std::int64_t modelId = 0;
  std::string name;
  std::string size;  // "" if null / absent
};

struct ModelsResult {
  std::int64_t toneId = 0;
  std::string architecture;
  std::vector<ModelInfo> models;
};

struct FetchResult {
  std::int64_t toneId = 0, modelId = 0;
  std::string path, sha256, kind, gear;  // kind: "nam" | "ir"
  CaptureSource source;
};

struct WhoAmI {
  std::string id, username, displayName;
};

struct LoginEvent {
  enum class Kind { DeviceCode, LoggedIn } kind = Kind::LoggedIn;
  std::string verificationUri, verificationUriComplete, userCode;
  int expiresIn = 0;
};

// The JSON document inside `text`: the whole text if it parses, otherwise the text from the last
// line that starts with '{' or '[' (stderr notes may precede it when the streams are merged).
// Empty string if there is none.
std::string extractJson(const std::string& text);

std::optional<std::vector<CaptureRecord>> parseRecords(const std::string& text, std::string& error);
std::optional<ModelsResult> parseModels(const std::string& text, std::string& error);
std::optional<FetchResult> parseFetch(const std::string& text, std::string& error);
std::optional<WhoAmI> parseWhoAmI(const std::string& text, std::string& error);
// {"error": "...", "code": "..."}; empty optional if the text is not an error object.
std::optional<ErrorInfo> parseErrorObject(const std::string& text);
// One line of `login --json-events`; empty optional for anything else (such a line is discarded).
std::optional<LoginEvent> parseLoginLine(const std::string& line);

}  // namespace sawblade::plugin::t3k
