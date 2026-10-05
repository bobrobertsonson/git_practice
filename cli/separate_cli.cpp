#include "separate_cli.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#include "sawblade/separate_song.h"

namespace sawblade_cli {
namespace {

std::atomic<bool> g_sigint{false};
extern "C" void onSigint(int) { g_sigint.store(true); }

}  // namespace

int runSeparate(const std::string& tool, const std::string& song, const std::string& outDir,
                const std::string& modelId, const std::string& threadsArg) {
  using namespace sawblade;
  namespace fs = std::filesystem;
  SeparateSongOptions opt;
  if (modelId == "htdemucs_6s") opt.model = SeparationModel::Htdemucs6s;
  else if (modelId == "htdemucs") opt.model = SeparationModel::Htdemucs4s;
  else {
    std::cerr << tool << ": --model must be htdemucs_6s or htdemucs\n";
    return 2;
  }
  if (!threadsArg.empty()) {
    char* end = nullptr;
    const long t = std::strtol(threadsArg.c_str(), &end, 10);
    if (end != threadsArg.c_str() + threadsArg.size() || t < 1 || t > 256) {
      std::cerr << tool << ": --threads must be an integer in 1..256\n";
      return 2;
    }
    opt.threads = static_cast<int>(t);
  }

  CancelToken cancel;
  g_sigint = false;
  const auto prevHandler = std::signal(SIGINT, onSigint);
  std::atomic<bool> stopWatcher{false};
  std::thread watcher([&] {  // a signal handler cannot take the token's mutex; a watcher forwards the signal
    while (!stopWatcher) {
      if (g_sigint) {
        cancel.cancel();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  struct Cleanup {
    std::atomic<bool>& stop;
    std::thread& w;
    void (*prev)(int);
    ~Cleanup() {
      stop = true;
      if (w.joinable()) w.join();
      std::signal(SIGINT, prev);
    }
  } cleanup{stopWatcher, watcher, prevHandler};

  const bool tty = ::isatty(2) != 0;
  int lastPct = -1;
  const auto progress = [&](double f, double eta) {
    const int pct = static_cast<int>(f * 100.0 + 0.5);
    if (pct == lastPct && f < 1.0) return;
    lastPct = pct;
    char buf[96];
    if (eta >= 0.0) std::snprintf(buf, sizeof buf, "separating: %3d%%  (about %.0f s left)", pct, eta);
    else std::snprintf(buf, sizeof buf, "separating: %3d%%", pct);
    std::fprintf(stderr, tty ? "\r%s   " : "%s\n", buf);
    if (tty && f >= 1.0) std::fputc('\n', stderr);
    std::fflush(stderr);
  };

  try {
    const SeparateSongResult r = separateSong(song, opt, progress, cancel);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) throw std::runtime_error("cannot create " + outDir + ": " + ec.message());
    for (const auto& f : stemCacheFileNames(opt.model))
      fs::copy_file(r.stemsDir / f, fs::path(outDir) / f, fs::copy_options::overwrite_existing);
    if (r.cacheHit) std::cout << "cache hit: " << r.stemsDir.string() << "\n";
    else std::cout << "cache miss: separated with " << separationModelId(opt.model) << " in " << r.separateSeconds
                   << " s, cached in " << r.stemsDir.string() << "\n";
    std::cout << "stems: " << fs::absolute(outDir).string() << "\n";
    return 0;
  } catch (const ModelUnavailable& e) {
    std::cerr << tool << ": " << e.what() << "\n";
    return 3;
  } catch (const SeparationCancelled&) {
    std::cerr << tool << ": cancelled\n";
    return 130;
  } catch (const std::exception& e) {
    std::cerr << tool << ": error: " << e.what() << "\n";
    return 4;
  }
}

}  // namespace sawblade_cli
