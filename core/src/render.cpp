#include "sawblade/render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

#include "sawblade/capture_cache.h"
#include "sawblade/nam_block.h"
#include "sawblade/resample.h"

namespace sawblade {
namespace {

constexpr int kMaxBlockSize = 65536;

double toDbfs(double lin) {
  return lin > 0.0 ? 20.0 * std::log10(lin) : -std::numeric_limits<double>::infinity();
}

SignalStats measure(const std::vector<float>& x) {
  double peak = 0.0, sum = 0.0;
  for (float v : x) {
    peak = std::max(peak, std::fabs(static_cast<double>(v)));
    sum += static_cast<double>(v) * v;
  }
  const double rms = x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
  return {toDbfs(peak), toDbfs(rms)};
}

void collectCapture(std::vector<CaptureAttribution>& out, const std::string& where, const Capture& c) {
  if (c.source) out.push_back({where, c.file, *c.source});
}

std::vector<CaptureAttribution> attributions(const Preset& p) {
  std::vector<CaptureAttribution> out;
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"paths.a", "paths.b"};
  for (int k = 0; k < 2; ++k)
    for (std::size_t i = 0; i < paths[k]->blocks.size(); ++i)
      if (const auto* nam = dynamic_cast<const NamBlockParams*>(paths[k]->blocks[i].params.get()))
        collectCapture(out, JsonObject::index(std::string(names[k]) + ".blocks", i) + ".model", nam->model);
  if (p.cab.mode == CabMode::Shared) {
    collectCapture(out, "cab.ir", p.cab.ir);
  } else {
    collectCapture(out, "cab.irA", p.cab.irA);
    collectCapture(out, "cab.irB", p.cab.irB);
  }
  return out;
}

const char* alignModeName(AlignMode m) {
  switch (m) {
    case AlignMode::Auto: return "auto";
    case AlignMode::Manual: return "manual";
    case AlignMode::Off: return "off";
  }
  return "auto";
}

// -infinity (digital silence) is not representable in JSON: report null.
nlohmann::json dbOrNull(double db) { return std::isfinite(db) ? nlohmann::json(db) : nlohmann::json(nullptr); }

struct NamRate {
  std::string where;
  double hz;
};

// Training rates of the NAM blocks that will actually run (not bypassed, on an enabled path).
// Models without a recorded rate are rate-agnostic and do not take part.
std::vector<NamRate> probeNamRates(const Preset& p, CaptureCache* cache) {
  std::vector<NamRate> out;
  const PathPreset* paths[2] = {&p.a, &p.b};
  const char* names[2] = {"paths.a", "paths.b"};
  for (int k = 0; k < 2; ++k) {
    if (!paths[k]->enabled) continue;
    for (std::size_t i = 0; i < paths[k]->blocks.size(); ++i) {
      const Block& b = paths[k]->blocks[i];
      const auto* nam = dynamic_cast<const NamBlockParams*>(b.params.get());
      if (!nam || b.bypass) continue;
      const std::string where = JsonObject::index(std::string(names[k]) + ".blocks", i);
      const std::string filePath = where + ".model.file";
      double hz = -1.0;
      try {
        if (cache) {
          hz = cache->namModel(nam->model, filePath)->expectedSampleRate();
        } else {
          hz = NamBlock::load(nam->model.resolvedPath, NamBlockConfig{})->expectedSampleRate();
        }
      } catch (const CaptureError& e) {
        throw RenderError(RenderErrorKind::Io, e.what(), e.jsonPath());
      } catch (const std::exception& e) {
        throw RenderError(RenderErrorKind::Io, e.what(), filePath);
      }
      if (hz > 0.0) out.push_back({where + " (" + b.id + ")", hz});
    }
  }
  return out;
}

std::string hzString(double hz) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", hz);
  return buf;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

RenderResult renderPreset(const Preset& preset, const AudioFile& in, const RenderOptions& opts) {
  if (opts.blockSize < 1 || opts.blockSize > kMaxBlockSize)
    throw RenderError(RenderErrorKind::Io, "block size must be in 1.." + std::to_string(kMaxBlockSize));
  if (in.channels < 1 || in.sampleRate <= 0.0 || in.interleaved.empty())
    throw RenderError(RenderErrorKind::Io, "input audio is empty");

  RenderResult r;
  r.presetName = preset.name;
  r.inputRate = in.sampleRate;
  r.blockSize = opts.blockSize;
  r.captures = attributions(preset);

  // Mono input (stereo and beyond: first channel, with a warning).
  const auto ch = static_cast<std::size_t>(in.channels);
  const std::size_t frames = in.interleaved.size() / ch;
  std::vector<float> x(frames);
  for (std::size_t i = 0; i < frames; ++i) x[i] = in.interleaved[i * ch];
  if (in.channels > 1)
    r.warnings.push_back("input has " + std::to_string(in.channels) + " channels; using the first (left) channel");
  r.input = measure(x);

  // Render rate.
  double rate = in.sampleRate;
  if (opts.renderRate) {
    rate = *opts.renderRate;
    if (!(rate >= 1000.0 && rate <= 768000.0))
      throw RenderError(RenderErrorKind::Preset, "render rate must be in 1000..768000 Hz");
  } else {
    const std::vector<NamRate> rates = probeNamRates(preset, opts.cache);
    if (!rates.empty()) {
      rate = rates.front().hz;
      bool agree = true;
      for (const auto& nr : rates) agree = agree && nr.hz == rate;
      if (!agree) {
        std::string msg = "NAM blocks expect different sample rates, so --render-rate auto is ambiguous (";
        for (std::size_t i = 0; i < rates.size(); ++i)
          msg += (i ? ", " : "") + rates[i].where + ": " + hzString(rates[i].hz) + " Hz";
        throw RenderError(RenderErrorKind::Preset, msg + "); set an explicit render rate");
      }
    }
  }
  r.renderRate = rate;
  r.outputRate = opts.outRate == OutRate::Render ? rate : in.sampleRate;
  r.sampleRate = r.outputRate;

  if (rate != in.sampleRate) {
    const auto tr = std::chrono::steady_clock::now();
    x = resample(x, in.sampleRate, rate);
    r.resampleSeconds += secondsSince(tr);
  }
  const std::size_t renderFrames = x.size();

  // Stage-based error mapping: anything wrong with the preset's own values (parse is done already;
  // Chain construction and prepare) is a Preset error; loading files is Io.
  std::unique_ptr<Chain> chain;
  try {
    ChainResources res = loadResources(preset, rate, opts.cache);
    try {
      chain = std::make_unique<Chain>(preset, std::move(res));
    } catch (const PresetError& e) {
      throw RenderError(RenderErrorKind::Preset, e.what(), e.jsonPath());
    } catch (const std::exception& e) {
      throw RenderError(RenderErrorKind::Preset, e.what());
    }
  } catch (const RenderError&) {
    throw;
  } catch (const PresetError& e) {
    throw RenderError(RenderErrorKind::Preset, e.what(), e.jsonPath());
  } catch (const CaptureError& e) {
    throw RenderError(RenderErrorKind::Io, e.what(), e.jsonPath());
  } catch (const std::exception& e) {
    throw RenderError(RenderErrorKind::Io, e.what());
  }

  auto t0 = std::chrono::steady_clock::now();
  try {
    chain->prepare({rate, opts.blockSize});
  } catch (const std::exception& e) {
    throw RenderError(RenderErrorKind::Preset,
                      std::string("preset cannot be prepared at ") + hzString(rate) + " Hz: " + e.what());
  }
  r.prepareSeconds = secondsSince(t0);
  r.info = chain->info();

  // Process N + latency samples (zeros after the input flush the tail) and drop the first
  // `latency` outputs, so the output is advanced by exactly the reported processing latency.
  const auto latency = static_cast<std::size_t>(chain->latencySamples());
  std::vector<float> buf(renderFrames + latency, 0.0f);
  std::copy(x.begin(), x.end(), buf.begin());
  t0 = std::chrono::steady_clock::now();
  for (std::size_t pos = 0; pos < buf.size(); pos += static_cast<std::size_t>(opts.blockSize)) {
    const auto n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(opts.blockSize), buf.size() - pos));
    chain->process(buf.data() + pos, buf.data() + pos, n);
  }
  r.renderSeconds = secondsSince(t0);
  r.realTimeFactor = r.renderSeconds / (static_cast<double>(renderFrames) / rate);
  r.samples.assign(buf.begin() + static_cast<std::ptrdiff_t>(latency), buf.end());

  if (rate != r.outputRate) {
    const auto tr = std::chrono::steady_clock::now();
    r.samples = resample(r.samples, rate, r.outputRate, resampledLength(frames, in.sampleRate, r.outputRate));
    r.resampleSeconds += secondsSince(tr);
  }

  if (opts.normalizePeakDbfs) {
    const double peak = std::pow(10.0, measure(r.samples).peakDbfs / 20.0);
    if (peak > 0.0) {
      const double g = std::pow(10.0, *opts.normalizePeakDbfs / 20.0) / peak;
      for (auto& v : r.samples) v = static_cast<float>(v * g);
      r.normalizeGainDb = toDbfs(g);
    } else {
      r.warnings.push_back("output is silent; peak normalization skipped");
    }
  }
  r.output = measure(r.samples);

  r.warnings.insert(r.warnings.end(), r.info.warnings.begin(), r.info.warnings.end());
  return r;
}

RenderResult renderFile(const std::filesystem::path& presetPath, const std::filesystem::path& inputWav,
                        const RenderOptions& opts) {
  Preset preset;
  try {
    preset = loadPresetFile(presetPath);
  } catch (const PresetError& e) {
    throw RenderError(RenderErrorKind::Preset, presetPath.string() + ": " + e.what());
  } catch (const std::exception& e) {
    throw RenderError(RenderErrorKind::Io, e.what());
  }
  AudioFile in;
  try {
    in = readWav(inputWav);
  } catch (const std::exception& e) {
    throw RenderError(RenderErrorKind::Io, e.what());
  }
  return renderPreset(preset, in, opts);
}

void writeRenderedWav(const std::filesystem::path& path, const RenderResult& r) {
  try {
    writeWavFloat32(path, r.sampleRate, r.samples);
  } catch (const std::exception& e) {
    throw RenderError(RenderErrorKind::Io, e.what());
  }
}

nlohmann::json reportJson(const RenderResult& r) {
  using nlohmann::json;
  const ChainInfo& i = r.info;
  json caps = json::array();
  for (const auto& c : r.captures) {
    json o = {{"where", c.where}, {"file", c.file}, {"provider", c.source.provider}, {"id", c.source.id}};
    if (!c.source.modelId.empty()) o["modelId"] = c.source.modelId;
    if (!c.source.url.empty()) o["url"] = c.source.url;
    if (!c.source.title.empty()) o["title"] = c.source.title;
    if (!c.source.creator.empty()) o["creator"] = c.source.creator;
    if (!c.source.license.empty()) o["license"] = c.source.license;
    caps.push_back(std::move(o));
  }
  auto stats = [](const SignalStats& s) { return json{{"peakDbfs", dbOrNull(s.peakDbfs)}, {"rmsDbfs", dbOrNull(s.rmsDbfs)}}; };
  return {
      {"preset", r.presetName},
      {"sampleRate", r.sampleRate},  // rate of the output file (== outputRate)
      {"inputRate", r.inputRate},
      {"renderRate", r.renderRate},  // latency fields below are in samples at this rate
      {"outputRate", r.outputRate},
      {"blockSize", r.blockSize},
      {"frames", r.samples.size()},
      {"latencySamples", i.latencySamples},
      {"pathLatency", {{"a", i.pathLatency[0]}, {"b", i.pathLatency[1]}}},
      {"compensationDelay", {{"a", i.compensationDelay[0]}, {"b", i.compensationDelay[1]}}},
      {"alignDelay", {{"a", i.alignDelay[0]}, {"b", i.alignDelay[1]}}},
      {"align", {{"mode", alignModeName(i.alignMode)},
                 {"resolved", {{"delaySamplesB", i.align.delaySamplesB},
                               {"invertB", i.align.invertB},
                               {"peakCorrelation", i.align.peakCorrelation}}}}},
      {"liveCompatible", i.liveCompatible},
      {"exportExactness", {{"withCab", i.exportExactness.withCab}, {"noCab", i.exportExactness.noCab}}},
      {"input", stats(r.input)},
      {"output", stats(r.output)},
      {"normalizeGainDb", r.normalizeGainDb},
      {"prepareSeconds", r.prepareSeconds},
      {"renderSeconds", r.renderSeconds},
      {"resampleSeconds", r.resampleSeconds},
      {"realTimeFactor", r.realTimeFactor},
      {"warnings", r.warnings},
      {"captures", caps},
  };
}

}  // namespace sawblade
