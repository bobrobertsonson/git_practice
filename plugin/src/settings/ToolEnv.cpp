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

namespace {
bool isB64(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }
bool isSp(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// True if s[i..i+n) all satisfy `ok` (and fit in s).
template <class F>
bool allOf(const std::string& s, std::size_t i, std::size_t n, F ok) {
  if (i + n > s.size()) return false;
  for (std::size_t k = 0; k < n; ++k)
    if (!ok(s[i + k])) return false;
  return true;
}

// Value-shaped checks on a (lower-cased, at most 4 KB) prefix. Plain loops, no recursion, no std::regex (its open-ended
// quantifiers recurse once per character: a long line would overflow a small stack, e.g. a 512 KB macOS secondary thread).
bool secretShaped(const std::string& l) {
  for (std::size_t p = l.find("t3k_cs_"); p != std::string::npos; p = l.find("t3k_cs_", p + 1))
    if (allOf(l, p + 7, 8, isB64)) return true;
  for (std::size_t p = l.find("eyj"); p != std::string::npos; p = l.find("eyj", p + 1))
    if (allOf(l, p + 3, 10, isB64)) return true;
  // bearer <1-16 spaces> <8 non-space>
  for (std::size_t p = l.find("bearer"); p != std::string::npos; p = l.find("bearer", p + 1)) {
    std::size_t q = p + 6, ws = 0;
    while (q < l.size() && isSp(l[q]) && ws < 17) ++q, ++ws;
    if (ws >= 1 && ws <= 16 && allOf(l, q, 8, [](char c) { return !isSp(c); })) return true;
  }
  // (token|secret|password|authorization|api_key) <0-16 spaces> [=:] <0-16 spaces> <value of 8+ with a digit, or 20+>
  for (const char* kw : {"token", "secret", "password", "authorization", "api_key", "api-key", "apikey"}) {
    const std::string k(kw);
    for (std::size_t p = l.find(k); p != std::string::npos; p = l.find(k, p + 1)) {
      std::size_t q = p + k.size(), ws = 0;
      while (q < l.size() && isSp(l[q]) && ws < 17) ++q, ++ws;
      if (ws > 16 || q >= l.size() || (l[q] != '=' && l[q] != ':')) continue;
      ++q;
      ws = 0;
      while (q < l.size() && isSp(l[q]) && ws < 17) ++q, ++ws;
      if (ws > 16) continue;
      std::size_t n = 0;
      bool digit = false;
      while (q + n < l.size() && !isSp(l[q + n])) digit = digit || std::isdigit(static_cast<unsigned char>(l[q + n])), ++n;
      if (n >= 8 && (digit || n >= 20)) return true;
    }
  }
  return false;
}
}  // namespace

bool looksLikeCredential(const std::string& line) {
  // Value-shaped patterns only: the CLI's own messages name "token", "t3k_pub_..." and "t3k_cs_..." and must stay readable.
  constexpr std::size_t kScan = 4096;
  std::string head = line.substr(0, kScan);
  for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (secretShaped(head)) return true;
  // an opaque code (also a sha256 hex digest, on purpose): 32+ of [A-Za-z0-9_-] holding both letters and digits
  std::size_t run = 0;
  bool letter = false, digit = false;
  for (char c : line) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (isB64(c)) {
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
    while (eq + 1 + n < line.size() && !isSp(line[eq + 1 + n])) ++n;
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
