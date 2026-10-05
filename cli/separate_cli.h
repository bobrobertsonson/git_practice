#pragma once

#include <string>
#include <vector>

// Shared by `tonerender --separate` and `sawblade-stems` (phase 5.1b). Only compiled with
// SAWBLADE_WITH_SEPARATOR.
namespace sawblade_cli {

// Exit codes: 0 ok, 2 usage, 3 model missing / wrong (the exact fetch command is on stderr), 4 other error,
// 130 cancelled (SIGINT). `args` are the song and out dir given by the caller plus the shared flags:
// --model htdemucs_6s|htdemucs (default htdemucs_6s), --threads N (default max(1, cores - 1)).
// Prints progress on stderr, "cache hit|miss: <dir>" on stdout, copies the stems into `outDir`.
int runSeparate(const std::string& tool, const std::string& song, const std::string& outDir,
                const std::string& model, const std::string& threads);

}  // namespace sawblade_cli
