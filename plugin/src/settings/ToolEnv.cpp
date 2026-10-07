#include "ToolEnv.h"

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
      out.push_back(s);
    }
  }
#endif
  for (const auto& kv : env) out.push_back(kv.first + "=" + kv.second);
  return out;
}

}  // namespace sawblade::plugin::settings
