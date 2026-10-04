// Shared helpers of the separator spike drivers (phase 5.1a). Not product code.
#pragma once
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <dr_wav.h>

namespace spike {

using Clock = std::chrono::steady_clock;

inline double secondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

inline double peakRssMb() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_maxrss) / 1024.0;  // Linux: kB
}

// One float32 stereo 44.1 kHz WAV; sample(ch, i) supplies the data.
inline void writeStereoFloat32(const std::filesystem::path& path, int frames,
                               const std::function<float(int, int)>& sample) {
  std::vector<float> inter(static_cast<std::size_t>(frames) * 2);
  for (int i = 0; i < frames; ++i) {
    inter[static_cast<std::size_t>(i) * 2] = sample(0, i);
    inter[static_cast<std::size_t>(i) * 2 + 1] = sample(1, i);
  }
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = 2;
  fmt.sampleRate = 44100;
  fmt.bitsPerSample = 32;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.string().c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write " + path.string());
  drwav_write_pcm_frames(&wav, static_cast<drwav_uint64>(frames), inter.data());
  drwav_uninit(&wav);
}

// --cancel-after S: a helper thread sets the flag S seconds after construction. The driver
// reports how long the engine took to return after the flag was set.
class CancelTimer {
 public:
  explicit CancelTimer(double afterS) {
    if (afterS < 0) return;
    armed_ = true;
    t_ = std::thread([this, afterS] {
      const auto t0 = Clock::now();
      while (secondsSince(t0) < afterS) {
        if (stop_.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      setAt_ = Clock::now();
      flag_.store(true);
      fired_.store(true);
    });
  }
  ~CancelTimer() {
    stop_.store(true);
    if (t_.joinable()) t_.join();
  }
  const std::atomic<bool>* flag() const { return armed_ ? &flag_ : nullptr; }
  bool fired() const { return fired_.load(); }
  // Seconds from the flag being set until `now`. Only meaningful when fired().
  double secondsSinceSet() const { return secondsSince(setAt_); }

 private:
  bool armed_ = false;
  std::atomic<bool> flag_{false}, fired_{false}, stop_{false};
  Clock::time_point setAt_{};
  std::thread t_;
};

}  // namespace spike
