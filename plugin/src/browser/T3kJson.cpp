#include "T3kJson.h"

#include <nlohmann/json.hpp>

namespace sawblade::plugin::t3k {
namespace {
using json = nlohmann::json;

std::optional<json> parseDoc(const std::string& text, std::string& error) {
  const std::string body = extractJson(text);
  if (body.empty()) {
    error = "no JSON in the output";
    return std::nullopt;
  }
  json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    error = "malformed JSON";
    return std::nullopt;
  }
  return j;
}

// Optional string member: absent / null -> "", wrong type -> error.
bool optStr(const json& o, const char* key, std::string& out, std::string& error) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return true;
  if (it->is_string()) {
    out = it->get<std::string>();
    return true;
  }
  error = std::string("'") + key + "' is not a string";
  return false;
}
bool optInt(const json& o, const char* key, int& out, std::string& error) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return true;
  if (it->is_number()) {
    out = static_cast<int>(it->get<double>());
    return true;
  }
  error = std::string("'") + key + "' is not a number";
  return false;
}
bool reqInt(const json& o, const char* key, std::int64_t& out, std::string& error) {
  auto it = o.find(key);
  if (it == o.end() || !it->is_number_integer()) {
    error = std::string("missing or non-integer '") + key + "'";
    return false;
  }
  out = it->get<std::int64_t>();
  return true;
}
bool reqStr(const json& o, const char* key, std::string& out, std::string& error) {
  auto it = o.find(key);
  if (it == o.end() || !it->is_string()) {
    error = std::string("missing or non-string '") + key + "'";
    return false;
  }
  out = it->get<std::string>();
  return true;
}
// A string-or-number member rendered as text (ids may be either).
bool idText(const json& o, const char* key, std::string& out, std::string& error, bool required) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) {
    if (required) error = std::string("missing '") + key + "'";
    return !required;
  }
  if (it->is_string()) out = it->get<std::string>();
  else if (it->is_number_integer()) out = std::to_string(it->get<std::int64_t>());
  else {
    error = std::string("'") + key + "' is neither a string nor an integer";
    return false;
  }
  return true;
}
bool strList(const json& o, const char* key, std::vector<std::string>& out, std::string& error) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return true;
  if (!it->is_array()) {
    error = std::string("'") + key + "' is not a list";
    return false;
  }
  for (const auto& e : *it) {
    if (e.is_string()) out.push_back(e.get<std::string>());
    else if (e.is_number()) out.push_back(e.dump());
  }
  return true;
}

bool parseRecord(const json& o, CaptureRecord& r, std::string& error) {
  if (!o.is_object()) {
    error = "record is not an object";
    return false;
  }
  if (!reqInt(o, "tone_id", r.toneId, error)) return false;
  if (!optStr(o, "title", r.title, error) || !optStr(o, "creator", r.creator, error) || !optStr(o, "gear", r.gear, error) ||
      !optStr(o, "format", r.format, error) || !optStr(o, "license", r.license, error) || !optStr(o, "created_at", r.createdAt, error) ||
      !optStr(o, "url", r.url, error) || !optStr(o, "status", r.status, error))
    return false;
  if (!optInt(o, "favorites_count", r.favorites, error) || !optInt(o, "downloads_count", r.downloads, error) ||
      !optInt(o, "models_count", r.modelsCount, error) || !optInt(o, "a2_models_count", r.a2Count, error) ||
      !optInt(o, "a1_models_count", r.a1Count, error) || !optInt(o, "irs_count", r.irsCount, error))
    return false;
  if (!strList(o, "sizes", r.sizes, error) || !strList(o, "reasons", r.reasons, error) || !strList(o, "flags", r.flags, error)) return false;
  if (auto it = o.find("passes"); it != o.end() && !it->is_null()) {
    if (!it->is_boolean()) {
      error = "'passes' is not a boolean";
      return false;
    }
    r.passes = it->get<bool>();
  }
  return true;
}
}  // namespace

std::string extractJson(const std::string& text) {
  auto firstNonSpace = text.find_first_not_of(" \t\r\n");
  if (firstNonSpace == std::string::npos) return {};
  if (!json::parse(text, nullptr, false).is_discarded()) return text.substr(firstNonSpace);
  // The last line that starts a document, through the end.
  std::size_t best = std::string::npos, pos = 0;
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    const std::size_t lineEnd = eol == std::string::npos ? text.size() : eol;
    std::size_t s = text.find_first_not_of(" \t\r", pos);
    if (s != std::string::npos && s < lineEnd && (text[s] == '{' || text[s] == '[')) {
      if (!json::parse(text.substr(s), nullptr, false).is_discarded()) best = s;
    }
    pos = lineEnd + 1;
  }
  return best == std::string::npos ? std::string{} : text.substr(best);
}

std::optional<std::vector<CaptureRecord>> parseRecords(const std::string& text, std::string& error) {
  try {
    auto j = parseDoc(text, error);
    if (!j) return std::nullopt;
    if (!j->is_array()) {
      error = "expected a list of records";
      return std::nullopt;
    }
    std::vector<CaptureRecord> out;
    for (const auto& e : *j) {
      CaptureRecord r;
      if (!parseRecord(e, r, error)) return std::nullopt;
      out.push_back(std::move(r));
    }
    return out;
  } catch (...) {
    error = "unexpected JSON structure";
    return std::nullopt;
  }
}

std::optional<ModelsResult> parseModels(const std::string& text, std::string& error) {
  try {
    auto j = parseDoc(text, error);
    if (!j) return std::nullopt;
    if (!j->is_object()) {
      error = "expected an object";
      return std::nullopt;
    }
    ModelsResult m;
    if (!reqInt(*j, "tone_id", m.toneId, error) || !optStr(*j, "architecture", m.architecture, error)) return std::nullopt;
    auto it = j->find("models");
    if (it == j->end() || !it->is_array()) {
      error = "missing 'models' list";
      return std::nullopt;
    }
    for (const auto& e : *it) {
      if (!e.is_object()) {
        error = "model is not an object";
        return std::nullopt;
      }
      ModelInfo mi;
      if (!reqInt(e, "model_id", mi.modelId, error) || !optStr(e, "name", mi.name, error) || !optStr(e, "size", mi.size, error)) return std::nullopt;
      m.models.push_back(std::move(mi));
    }
    return m;
  } catch (...) {
    error = "unexpected JSON structure";
    return std::nullopt;
  }
}

std::optional<FetchResult> parseFetch(const std::string& text, std::string& error) {
  try {
    auto j = parseDoc(text, error);
    if (!j) return std::nullopt;
    if (!j->is_object()) {
      error = "expected an object";
      return std::nullopt;
    }
    FetchResult f;
    if (!reqInt(*j, "tone_id", f.toneId, error) || !reqInt(*j, "model_id", f.modelId, error) || !reqStr(*j, "path", f.path, error) ||
        !reqStr(*j, "sha256", f.sha256, error) || !reqStr(*j, "kind", f.kind, error) || !optStr(*j, "gear", f.gear, error))
      return std::nullopt;
    if (f.kind != "nam" && f.kind != "ir") {
      error = "unknown kind '" + f.kind + "'";
      return std::nullopt;
    }
    auto it = j->find("source");
    if (it == j->end() || !it->is_object()) {
      error = "missing 'source' object";
      return std::nullopt;
    }
    CaptureSource& s = f.source;
    if (!reqStr(*it, "provider", s.provider, error) || !idText(*it, "id", s.id, error, true) || !idText(*it, "modelId", s.modelId, error, false) ||
        !optStr(*it, "url", s.url, error) || !optStr(*it, "title", s.title, error) || !optStr(*it, "creator", s.creator, error) ||
        !optStr(*it, "license", s.license, error))
      return std::nullopt;
    return f;
  } catch (...) {
    error = "unexpected JSON structure";
    return std::nullopt;
  }
}

std::optional<WhoAmI> parseWhoAmI(const std::string& text, std::string& error) {
  try {
    auto j = parseDoc(text, error);
    if (!j) return std::nullopt;
    if (!j->is_object()) {
      error = "expected an object";
      return std::nullopt;
    }
    WhoAmI w;
    if (!idText(*j, "id", w.id, error, false) || !optStr(*j, "username", w.username, error) || !optStr(*j, "display_name", w.displayName, error))
      return std::nullopt;
    return w;
  } catch (...) {
    error = "unexpected JSON structure";
    return std::nullopt;
  }
}

std::optional<ErrorInfo> parseErrorObject(const std::string& text) {
  try {
    std::string ignore;
    auto j = parseDoc(text, ignore);
    if (!j || !j->is_object()) return std::nullopt;
    auto it = j->find("error");
    if (it == j->end() || !it->is_string()) return std::nullopt;
    ErrorInfo e;
    e.message = it->get<std::string>();
    if (auto c = j->find("code"); c != j->end() && c->is_string()) e.code = c->get<std::string>();
    if (e.code.empty()) e.code = "error";
    return e;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<LoginEvent> parseLoginLine(const std::string& line) {
  try {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    auto ev = j.find("event");
    if (ev == j.end() || !ev->is_string()) return std::nullopt;
    LoginEvent e;
    std::string ignore;
    if (ev->get<std::string>() == "logged_in") {
      e.kind = LoginEvent::Kind::LoggedIn;
      return e;
    }
    if (ev->get<std::string>() != "device_code") return std::nullopt;
    e.kind = LoginEvent::Kind::DeviceCode;
    if (!optStr(j, "verification_uri", e.verificationUri, ignore) || !optStr(j, "verification_uri_complete", e.verificationUriComplete, ignore) ||
        !optStr(j, "user_code", e.userCode, ignore) || !optInt(j, "expires_in", e.expiresIn, ignore))
      return std::nullopt;
    if (e.userCode.empty() && e.verificationUriComplete.empty()) return std::nullopt;
    return e;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace sawblade::plugin::t3k
