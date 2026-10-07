#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Settings.h"

// v0.3.0.1 (docs/specs/v0_3_0_1-t3k_env_hotfix.md Task A): the ONE rule for the environment of every `sawblade-*` tool the plugin
// launches (ToolRunner, T3kTool, T3kRunner/T3kClient, JobRunner). A DAW started from the Dock has no shell exports, so the
// tool must never depend on the host's environment for the TONE3000 client id or the capture cache. Not real-time.
namespace sawblade::plugin::settings {

using ToolEnvMap = std::map<std::string, std::string>;

// PYTHONUNBUFFERED=1; TONE3000_CLIENT_ID = settings.effectiveTone3000ClientId() when known (never a t3k_cs_ value);
// SAWBLADE_CACHE_DIR = settings.effectiveCaptureCacheDir(). Thread-safe (Settings has its own mutex).
ToolEnvMap toolEnvironment(const Settings& settings);
// The same, from Settings::shared() (the plugin's own settings). Creates the shared instance on first use.
ToolEnvMap toolEnvironment();

// "" when `exe` can be launched through `/usr/bin/env KEY=VALUE... exe` (a '=' in the path would read as an assignment).
std::string toolPathProblem(const std::filesystem::path& exe);

// The argv that runs `exe args...` with `env` added to the host's environment: /usr/bin/env K=V... exe args... (POSIX; on
// Windows the tool starts with the host environment and no injection). Precondition: toolPathProblem(exe).empty().
std::vector<std::string> toolCommand(const std::filesystem::path& exe, const std::vector<std::string>& args, const ToolEnvMap& env);

// The host's environment with `env` applied on top, as "KEY=VALUE" strings (for posix_spawn). A host TONE3000_CLIENT_ID that holds a
// t3k_cs_ secret key is dropped when `env` has no valid id to replace it.
std::vector<std::string> mergedEnvironment(const ToolEnvMap& env);

// One credential filter for every tool line the plugin may show. True for: the keywords token / secret / t3k_ / bearer / password /
// authorization (any case), any run of 24+ characters of [A-Za-z0-9_.-] (JWTs, opaque codes), and `key=<16+ non-space characters>`.
bool looksLikeCredential(const std::string& line);
// `line` without trailing CR / spaces, cut to at most 300 bytes on a UTF-8 code-point boundary; "" if it is empty or looksLikeCredential.
std::string safeToolLine(const std::string& line);

}  // namespace sawblade::plugin::settings
