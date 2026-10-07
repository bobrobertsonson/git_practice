#include "ToolEnv.h"

#include <cctype>
#include <regex>

#if !defined(_WIN32)
#if defined(__APPLE__)
#include <crt_externs.h>
#define SAWBLADE_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define SAWBLADE_ENVIRON environ
#endif
#endif

namespace sawblade::plugin::settings {
namespace fs = std::filesystem;

ToolEnvMap toolEnvironment(const Settings& settings) {
  ToolEnvMap env;
  env["PYTHONUNBUFFERED"] = "1";
  if (const std::string id = settings.effectiveTone3000ClientId(); !id.empty() && !containsSecretKey(id)) env["TONE3000_CLIENT_ID"] = id;
  env["SAWBLADE_CACHE_DIR"] = settings.effectiveCaptureCacheDir().string();
  return env;
}

ToolEnvMap toolEnvironment() { return toolEnvironment(Settings::shared()); }

std::string toolPathProblem(const fs::path& exe) {
#if !defined(_WIN32)
  if (exe.string().find('=') != std::string::npos)  // `env` would read it as a KEY=VALUE assignment
    return "cannot run " + exe.string() + ": the path contains '='. Move the match venv to a folder without '=' in its name.";
#else
  (void)exe;
#endif
  return {};
}

std::vector<std::string> toolCommand(const fs::path& exe, const std::vector<std::string>& args, const ToolEnvMap& env) {
  std::vector<std::string> cmd;
#if !defined(_WIN32)
  cmd.push_back("/usr/bin/env");
  cmd.push_back("-u");  // BSD (macOS) and GNU env both take -u: a host secret key never reaches the tool, a valid id below replaces it
  cmd.push_back("TONE3000_CLIENT_ID");
  for (const auto& kv : env) cmd.push_back(kv.first + "=" + kv.second);
#else
  (void)env;
#endif
  cmd.push_back(exe.string());
  for (const auto& a : args) cmd.push_back(a);
  return cmd;
}

std::vector<std::string> mergedEnvironment(const ToolEnvMap& env) {
  std::vector<std::string> out;
#if !defined(_WIN32)
  if (char** e = SAWBLADE_ENVIRON) {
    for (; *e != nullptr; ++e) {
      const std::string s(*e);
      const auto eq = s.find('=');
      if (eq != std::string::npos && env.count(s.substr(0, eq)) != 0) continue;  // replaced below
      if (s.compare(0, 19, "TONE3000_CLIENT_ID=") == 0 && containsSecretKey(s.substr(19))) continue;  // never pass a secret key on
      out.push_back(s);
    }
  }
#endif
  for (const auto& kv : env) out.push_back(kv.first + "=" + kv.second);
  return out;
}

bool looksLikeCredential(const std::string& line) {
  // Value-shaped patterns only: the CLI's own messages name "token", "t3k_pub_..." and "t3k_cs_..." and must stay readable.
  static const std::regex kPatterns[] = {
      std::regex(R"(t3k_cs_[A-Za-z0-9_-]{8,})"),                                                    // a secret key value
      std::regex(R"(eyJ[A-Za-z0-9_-]{10,})"),                                                       // a JWT
      std::regex(R"(bearer\s+\S{8,})", std::regex::icase),                                          // an Authorization header value
      std::regex(R"((token|secret|password|authorization|api[_-]?key)\s*[=:]\s*\S{8,})", std::regex::icase)};
  for (const auto& re : kPatterns)
    if (std::regex_search(line, re)) return true;
  // an opaque code: 32+ of [A-Za-z0-9_-] holding both letters and digits
  std::size_t run = 0;
  bool letter = false, digit = false;
  for (char c : line) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '_' || c == '-') {
      ++run;
      letter = letter || std::isalpha(u);
      digit = digit || std::isdigit(u);
      if (run >= 32 && letter && digit) return true;
    } else {
      run = 0;
      letter = digit = false;
    }
  }
  // key=<long value>, except a path (/, ., ~)
  for (std::size_t eq = line.find('='); eq != std::string::npos; eq = line.find('=', eq + 1)) {
    if (eq + 1 < line.size() && (line[eq + 1] == '/' || line[eq + 1] == '.' || line[eq + 1] == '~')) continue;
    std::size_t n = 0;
    while (eq + 1 + n < line.size() && !std::isspace(static_cast<unsigned char>(line[eq + 1 + n]))) ++n;
    if (n >= 16) return true;
  }
  return false;
}

std::string safeToolLine(const std::string& line) {
  std::string l = line;
  while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' ')) l.pop_back();
  if (l.empty() || looksLikeCredential(l)) return {};
  constexpr std::size_t kMax = 300;
  if (l.size() > kMax) {
    std::size_t cut = kMax;
    while (cut > 0 && (static_cast<unsigned char>(l[cut]) & 0xC0) == 0x80) --cut;  // never split a code point
    l.resize(cut);
  }
  return l;
}

std::string lastSafeLine(const std::string& output) {
  std::size_t end = output.size();
  while (end > 0) {
    const std::size_t nl = output.rfind('\n', end - 1);
    const std::size_t begin = nl == std::string::npos ? 0 : nl + 1;
    const std::string line = output.substr(begin, end - begin);
    end = nl == std::string::npos ? 0 : nl;
    if (!line.empty() && (line.front() == '{' || line.front() == '[')) continue;
    if (std::string safe = safeToolLine(line); !safe.empty()) return safe;
  }
  return {};
}

}  // namespace sawblade::plugin::settings
