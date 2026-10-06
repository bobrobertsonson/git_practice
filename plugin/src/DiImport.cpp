#include "DiImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <juce_audio_formats/juce_audio_formats.h>

namespace sawblade::plugin::di_import {
namespace fs = std::filesystem;

bool isImportableName(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

std::optional<double> parseSongTime(const std::string& text) {
  // m:ss[.f[f[f]]]
  std::size_t i = 0;
  const std::size_t n = text.size();
  auto digits = [&](std::size_t from) {
    std::size_t e = from;
    while (e < n && text[e] >= '0' && text[e] <= '9') ++e;
    return e;
  };
  std::size_t e = digits(i);
  if (e == i || e - i > 4 || e >= n || text[e] != ':') return std::nullopt;
  const double minutes = std::stod(text.substr(i, e - i));
  i = e + 1;
  e = digits(i);
  if (e - i != 2) return std::nullopt;
  const int seconds = std::stoi(text.substr(i, 2));
  if (seconds > 59) return std::nullopt;
  double ms = (minutes * 60.0 + seconds) * 1000.0;
  i = e;
  if (i < n) {
    if (text[i] != '.') return std::nullopt;
    ++i;
    e = digits(i);
    if (e == i || e - i > 3 || e != n) return std::nullopt;
    std::string frac = text.substr(i, e - i);
    while (frac.size() < 3) frac.push_back('0');
    ms += std::stoi(frac);
  }
  return ms;
}

std::string formatSongTime(double ms) {
  if (!(ms > 0.0)) ms = 0.0;
  const auto total = static_cast<std::int64_t>(std::llround(ms));
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld:%02lld.%03lld", static_cast<long long>(total / 60000), static_cast<long long>((total / 1000) % 60), static_cast<long long>(total % 1000));
  return buf;
}

const char* channelName(Channel c) {
  switch (c) {
    case Channel::Left: return "left";
    case Channel::Right: return "right";
    case Channel::Sum: return "sum";
  }
  return "left";
}

namespace {
std::unique_ptr<juce::AudioFormatReader> openReader(const fs::path& file) {
  juce::AudioFormatManager fm;
  fm.registerBasicFormats();  // WAV, AIFF, FLAC (+ whatever else the platform adds; the name is checked first)
  return std::unique_ptr<juce::AudioFormatReader>(fm.createReaderFor(juce::File(juce::String(file.string()))));
}
}  // namespace

Probe probe(const fs::path& file) {
  Probe p;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    p.error = "That file does not exist.";
    return p;
  }
  if (!isImportableName(file.string())) {
    p.error = "Not an audio file I can import: choose a WAV, AIFF or FLAC file.";
    return p;
  }
  auto r = openReader(file);
  if (!r) {
    p.error = "Cannot read " + file.filename().string() + " as a WAV, AIFF or FLAC file.";
    return p;
  }
  p.channels = static_cast<int>(r->numChannels);
  p.sampleRate = r->sampleRate;
  p.frames = r->lengthInSamples;
  if (p.channels < 1 || p.channels > 2) {
    p.error = "Only mono or stereo files can be imported (this one has " + std::to_string(p.channels) + " channels).";
  } else if (p.frames <= 0 || !(p.sampleRate > 0.0)) {
    p.error = "This file contains no audio.";
  }
  return p;
}

std::string rejectReason(const std::vector<float>& mono, double sampleRate) {
  if (mono.empty()) return "This file contains no audio.";
  float peak = 0.0f;
  int run = 0;
  std::size_t clipAt = 0;
  bool clipped = false;
  for (std::size_t i = 0; i < mono.size(); ++i) {
    const float a = std::fabs(mono[i]);
    peak = std::max(peak, a);
    if (a >= kClipLevel) {
      if (++run >= kClipRunSamples && !clipped) {
        clipped = true;
        clipAt = i + 1 - static_cast<std::size_t>(kClipRunSamples);
      }
    } else {
      run = 0;
    }
  }
  if (!(peak >= static_cast<float>(std::pow(10.0, kSilentPeakDb / 20.0)))) return "This file is silent (its loudest sample is below -60 dBFS): there is no guitar to match.";
  if (clipped)
    return "This DI is clipped (" + std::to_string(kClipRunSamples) + " or more samples in a row at full scale, first at " +
           formatSongTime(1000.0 * static_cast<double>(clipAt) / std::max(1.0, sampleRate)) + "): record it again with the input gain lower.";
  return {};
}

Outcome importFile(TakeRecorder& rec, const fs::path& file, const Options& opt, const std::atomic<bool>* cancel) {
  Outcome out;
  auto fail = [&](std::string m) {
    out.ok = false;
    out.error = std::move(m);
    return out;
  };
  const Probe pr = probe(file);
  if (!pr.ok()) return fail(pr.error);
  auto reader = openReader(file);
  if (!reader) return fail("Cannot read " + file.filename().string() + ".");
  const int ch = pr.channels;
  std::vector<float> mono;
  mono.resize(static_cast<std::size_t>(pr.frames));
  constexpr int kBlock = 65536;
  juce::AudioBuffer<float> buf(2, kBlock);
  for (std::int64_t pos = 0; pos < pr.frames; pos += kBlock) {
    if (cancel != nullptr && cancel->load()) return fail("Cancelled.");
    const int n = static_cast<int>(std::min<std::int64_t>(kBlock, pr.frames - pos));
    float* chans[2] = {buf.getWritePointer(0), buf.getWritePointer(1)};
    if (!reader->read(chans, ch, pos, n)) return fail("Cannot decode " + file.filename().string() + ".");
    float* dst = mono.data() + pos;
    if (ch == 1) {
      std::copy(chans[0], chans[0] + n, dst);
    } else if (opt.channel == Channel::Left) {
      std::copy(chans[0], chans[0] + n, dst);
    } else if (opt.channel == Channel::Right) {
      std::copy(chans[1], chans[1] + n, dst);
    } else {
      for (int i = 0; i < n; ++i) dst[i] = 0.5f * (chans[0][i] + chans[1][i]);  // the rig's own stereo -> mono sum
    }
  }
  if (const std::string why = rejectReason(mono, pr.sampleRate); !why.empty()) return fail(why);
  ImportedInfo info;
  info.present = true;
  info.source = file.filename().string();
  info.channel = ch == 1 ? "mono" : channelName(opt.channel);
  info.samePerformance = opt.samePerformance;
  if (opt.samePerformance) info.offsetMs = opt.offsetMs;
  std::string err;
  if (!rec.importTake(mono, pr.sampleRate, info, &out.takeName, &err)) return fail(err);
  out.ok = true;
  return out;
}

ImportJob::ImportJob(TakeRecorder& rec, fs::path file, Options opt) {
  thread_ = std::thread([this, &rec, f = std::move(file), opt] {
    outcome_ = importFile(rec, f, opt, &cancel_);
    done_.store(true, std::memory_order_release);
  });
}

ImportJob::~ImportJob() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
}

}  // namespace sawblade::plugin::di_import
