#pragma once

// Test-harness lock counter (Linux/glibc). The plugin test executable is linked with
// -Wl,--wrap=pthread_mutex_lock (and trylock / rwlock variants); lock_guard.cpp counts the calls
// made by objects linked into the executable (our code, JUCE, std::mutex which is inline) on a
// thread while a LockGuard is armed. Together with AllocGuard this enforces the audio-thread rule
// "no allocation, no locks". Where the wrap is unavailable, enabled() is false and tests skip.
namespace sawblade::test {

class LockGuard {
 public:
  LockGuard() noexcept;
  ~LockGuard();
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
  long count() const noexcept;
  static bool enabled() noexcept;
};

}  // namespace sawblade::test
