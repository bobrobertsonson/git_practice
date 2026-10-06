#pragma once

// IMPORT DI... (v0.2.1 Task B): turns an audio file the user already has (a DI bounce) into a normal take. This is the
// shared, UI-free part: which files are accepted, the silent / clipped checks, decoding one channel to mono, and the call that
// writes the take (TakeRecorder::importTake). The dialog and the two views that start it are in ImportDialog.h.
//
// An import is a COPY: the original is only read, never moved, referenced or changed. The take is a 32-bit float mono WAV at the
// file's own sample rate (no resampling), exactly like a recorded take, with the same sidecar plus `imported`.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TakeRecorder.h"

namespace sawblade::plugin::di_import {

// A file is rejected as silent when its loudest sample (of the chosen channel) is below this: -60 dBFS = 0.001. A DI that
// quiet has no guitar to match (a real DI peaks at -30 .. -6 dBFS); the threshold is far below any playable level.
constexpr double kSilentPeakDb = -60.0;
// ... and as clipped when a run of at least kClipRunSamples consecutive samples has |x| >= kClipLevel. One sample at full scale is
// a peak, not clipping (a normalised 0 dBFS DI has them); a flat top of 4+ samples is a converter / digital clip. 0.999 also
// catches 16-bit full scale (+32767/32768 = 0.99997 and -32768/32768 = -1).
constexpr float kClipLevel = 0.999f;
constexpr int kClipRunSamples = 4;

// Accepted containers, by extension (any case): WAV, AIFF, FLAC.
bool isImportableName(const std::string& path);

// "m:ss.mmm" (also "m:ss", "m:ss.m", "m:ss.mm") <-> milliseconds. Minutes any number of digits, seconds 00..59 (two digits).
// nullopt for anything else (empty, negative, 1:75, 1.5, "abc").
std::optional<double> parseSongTime(const std::string& text);
std::string formatSongTime(double ms);  // always m:ss.mmm

enum class Channel { Left, Right, Sum };
const char* channelName(Channel c);  // "left" | "right" | "sum"

struct Probe {
  std::string error;                 // "" = readable
  int channels = 0;
  double sampleRate = 0.0;
  std::int64_t frames = 0;
  bool ok() const { return error.empty(); }
  double seconds() const { return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};
// Reads the header only (cheap, message thread): format, channels (1 or 2 accepted), rate, length.
Probe probe(const std::filesystem::path& file);

// Pure: "" if `mono` is a usable DI, else the one-line reason it is not (empty, silent, clipped).
std::string rejectReason(const std::vector<float>& mono, double sampleRate);

struct Options {
  Channel channel = Channel::Left;       // stereo files only
  bool samePerformance = false;
  std::optional<double> offsetMs;        // used only with samePerformance; none = "don't know"
};

struct Outcome {
  bool ok = false;
  std::string error;                     // one line, when !ok
  std::string takeName;                  // the new take, when ok
};

// Decodes `file`, picks the channel, checks it and writes the take. Blocking: call it off the message thread (ImportJob). `cancel`
// is polled between blocks.
Outcome importFile(TakeRecorder& rec, const std::filesystem::path& file, const Options& opt, const std::atomic<bool>* cancel = nullptr);

// importFile on its own thread. The destructor cancels and joins, so a dialog that goes away never leaves a writer behind.
class ImportJob {
 public:
  ImportJob(TakeRecorder& rec, std::filesystem::path file, Options opt);
  ~ImportJob();
  ImportJob(const ImportJob&) = delete;
  ImportJob& operator=(const ImportJob&) = delete;
  bool done() const { return done_.load(std::memory_order_acquire); }
  const Outcome& outcome() const { return outcome_; }  // valid once done()

 private:
  std::atomic<bool> cancel_{false}, done_{false};
  Outcome outcome_;
  std::thread thread_;
};

}  // namespace sawblade::plugin::di_import
