#include "ToolEnv.h"

#include <cctype>

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
  std::string l = line;
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* k : {"token", "secret", "t3k_", "bearer", "password", "authorization"})
    if (l.find(k) != std::string::npos) return true;
  std::size_t run = 0;
  for (char c : line) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '_' || c == '.' || c == '-') {
      if (++run >= 24) return true;
    } else {
      run = 0;
    }
  }
  for (std::size_t eq = line.find('='); eq != std::string::npos; eq = line.find('=', eq + 1)) {
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

}  // namespace sawblade::plugin::settings
