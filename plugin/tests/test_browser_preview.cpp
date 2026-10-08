// Capture browser: PreviewPlayer on the audio thread (alloc / lock harness), the preview render, the riff
// asset and its generator, and USE through the processor's normal loader.

#include <catch2/catch_approx.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "browser/PreviewPlayer.h"
#include "sawblade/sha256.h"
#include "sawblade/wav_io.h"
#include "browser/PreviewRender.h"
#include "browser/SlotTarget.h"
#include "processor_harness.h"

namespace {
const fs::path kAssets = SAWBLADE_PLUGIN_ASSETS_DIR;

std::vector<float> ramp(std::size_t n) {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = 0.5f * std::sin(0.05f * static_cast<float>(i)) + 0.25f;
  return v;
}

// Runs the player over `n` samples of a constant rig output in blocks of `block`; counts allocs / locks.
struct PlayerRun {
  std::vector<float> out;
  long allocs = 0, locks = 0;
};
PlayerRun runPlayer(PreviewPlayer& pl, std::size_t n, int block, float rig) {
  PlayerRun r;
  r.out.assign(n, rig);
  std::vector<float> r2(n, rig);
  for (std::size_t pos = 0; pos < n;) {
    const int len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), n - pos));
    float* ch[2] = {r.out.data() + pos, r2.data() + pos};
    {
      AllocGuard ag;
      LockGuard lg;
      pl.process(ch, 2, len);
      r.allocs += ag.count();
      r.locks += lg.count();
    }
    pos += static_cast<std::size_t>(len);
  }
  for (std::size_t i = 0; i < n; ++i) REQUIRE(r.out[i] == r2[i]);  // dual mono
  return r;
}
}  // namespace

TEST_CASE("preview player: plays the buffer after the fade-in, returns to the rig, no allocs or locks", "[browser][preview]") {
  PreviewPlayer pl;
  pl.prepare(48000.0);
  const auto buf = ramp(20000);
  const float rig = 0.1f;
  pl.start(buf, 48000.0);
  const auto r = runPlayer(pl, 30000, 256, rig);
  CHECK(r.allocs == 0);
  if (LockGuard::enabled()) CHECK(r.locks == 0);
  const int fade = 480;
  CHECK(r.out[0] == rig);  // the fade-in starts from the rig
  for (std::size_t i = static_cast<std::size_t>(fade); i < buf.size() - static_cast<std::size_t>(fade); ++i) REQUIRE(r.out[i] == buf[i]);
  for (std::size_t i = buf.size(); i < r.out.size(); ++i) REQUIRE(r.out[i] == rig);
  CHECK_FALSE(pl.playing());
  // the tail fades back to the rig without a hole: continuous
  for (std::size_t i = buf.size() - static_cast<std::size_t>(fade); i < buf.size(); ++i) REQUIRE(std::isfinite(r.out[i]));
}

TEST_CASE("preview player: block-size independent", "[browser][preview]") {
  const auto buf = ramp(9000);
  std::vector<std::vector<float>> outs;
  for (int block : {1, 7, 64, 480, 4096}) {
    PreviewPlayer pl;
    pl.prepare(48000.0);
    pl.start(buf, 48000.0);
    outs.push_back(runPlayer(pl, 12000, block, 0.2f).out);
  }
  for (std::size_t k = 1; k < outs.size(); ++k)
    for (std::size_t i = 0; i < outs[0].size(); ++i) REQUIRE(std::fabs(outs[k][i] - outs[0][i]) <= 1e-6f);
}

TEST_CASE("preview player: stop() fades out, a rate mismatch is ignored, a new start replaces", "[browser][preview]") {
  PreviewPlayer pl;
  pl.prepare(48000.0);
  const auto buf = ramp(48000);
  pl.start(buf, 48000.0);
  auto a = runPlayer(pl, 2000, 128, 0.0f);
  CHECK(pl.playing());
  pl.stop();
  std::vector<float> o(2000, 0.0f);
  float* ch[1] = {o.data()};
  pl.process(ch, 1, 2000);
  CHECK_FALSE(pl.playing());
  CHECK(o[0] != 0.0f);                // still sounding at the first sample of the fade
  CHECK(o[1999] == 0.0f);             // the rig (silent here) is back after the 10 ms fade
  CHECK(std::fabs(o[479]) < 0.01f);   // and the fade reaches (almost) zero
  // stop() before any start is harmless, and a stop() after a start cancels it
  PreviewPlayer p2;
  p2.prepare(48000.0);
  p2.stop();
  p2.start(buf, 48000.0);
  p2.stop();
  auto b = runPlayer(p2, 500, 100, 0.0f);
  for (float v : b.out) REQUIRE(v == 0.0f);
  // rate mismatch
  PreviewPlayer p3;
  p3.prepare(44100.0);
  p3.start(buf, 48000.0);
  auto c = runPlayer(p3, 500, 100, 0.0f);
  for (float v : c.out) REQUIRE(v == 0.0f);
  // a second preview replaces the first (and the retired buffer is freed off the audio thread)
  PreviewPlayer p4;
  p4.prepare(48000.0);
  p4.start(buf, 48000.0);
  runPlayer(p4, 3000, 256, 0.0f);
  p4.start(std::vector<float>(5000, 0.3f), 48000.0);
  auto d = runPlayer(p4, 2000, 256, 0.0f);
  CHECK(d.allocs == 0);
  CHECK(d.out[1000] == 0.3f);
  p4.collect();
}

TEST_CASE("preview player: through the processor, the preview replaces the rig output", "[browser][preview]") {
  TempDir td;
  Host h(48000.0, 256);
  h.load(writeIdentityPreset(td.dir, "id", 0));
  std::vector<float> x(4800, 0.0f), y;
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * std::sin(0.02f * static_cast<float>(i));
  h.run(x, y, {256});
  h.p.previewPlayer().start(std::vector<float>(20000, 0.5f), 48000.0);
  h.run(x, y, {256});
  CHECK(h.allocs == 0);
  if (LockGuard::enabled()) CHECK(h.locks == 0);
  CHECK(y[4000] == 0.5f);
  h.p.previewPlayer().stop();
}

TEST_CASE("preview render: fixture preset + fixture nam gives 6 s, finite, non-silent", "[browser][preview]") {
  const Preset p = loadPresetFile(kPresetDir / "two_linear.json");
  const AudioFile riff = embeddedPreviewRiff();
  CHECK(riff.sampleRate == 48000.0);
  CHECK(std::abs(static_cast<long>(riff.interleaved.size()) - 288000) <= 1);
  std::string err;
  for (double rate : {48000.0, 44100.0}) {
    const auto out = renderPreview(p, riff, rate, nullptr, err);
    INFO(err);
    REQUIRE_FALSE(out.empty());
    CHECK(std::abs(static_cast<double>(out.size()) - 6.0 * rate) <= 2.0);
    float peak = 0.0f;
    for (float v : out) {
      REQUIRE(std::isfinite(v));
      peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak > 0.1f);
    CHECK(peak < 1.0f);
  }
  Preset bad = p;
  bad.a.blocks[0] = [&] {
    t3k::FetchResult f;
    f.path = "/nonexistent/x.nam";
    f.kind = "nam";
    f.source.provider = "tone3000";
    f.source.id = "1";
    std::string e;
    return withCapture(p, slotTargets(p, Slot::SawAmp).at(0), f, e)->a.blocks[0];
  }();
  CHECK(renderPreview(bad, riff, 48000.0, nullptr, err).empty());
  CHECK_FALSE(err.empty());
}

TEST_CASE("preview riff: asset specification and deterministic generator", "[browser][preview]") {
  const AudioFile a = readWav(kAssets / "preview_riff.wav");
  CHECK(a.sampleRate == 48000.0);
  CHECK(a.channels == 1);
  CHECK(std::abs(static_cast<long>(a.interleaved.size()) - 288000) <= 1);
  float peak = 0.0f;
  for (float v : a.interleaved) peak = std::max(peak, std::fabs(v));
  const double db = 20.0 * std::log10(static_cast<double>(peak));
  CHECK(db <= -6.0);
  CHECK(db >= -15.0);

  TempDir td;
  const std::string py = std::string(SAWBLADE_PYTHON_EXE);
  const std::string script = SAWBLADE_RIFF_SCRIPT;
  auto gen = [&](const std::string& name) {
    const std::string cmd = "\"" + py + "\" \"" + script + "\" --out \"" + (td.dir / name).string() + "\" > /dev/null";
    return std::system(cmd.c_str());
  };
  REQUIRE(gen("one.wav") == 0);
  REQUIRE(gen("two.wav") == 0);
  auto slurp = [](const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  CHECK(slurp(td.dir / "one.wav") == slurp(td.dir / "two.wav"));
  CHECK(slurp(td.dir / "one.wav") == slurp(kAssets / "preview_riff.wav"));  // the committed asset is what the script makes
}

TEST_CASE("browser swap: USE on every piece kind goes through the loader and round-trips", "[browser][swap]") {
  const fs::path nam = kFixtures / "nam" / "linear_neg1_at_23.nam", ir = kFixtures / "ir" / "ir_b.wav";
  auto fetched = [&](const fs::path& f, const std::string& kind) {
    t3k::FetchResult r;
    r.toneId = 77;
    r.modelId = 771;
    r.path = f.string();
    r.sha256 = sawblade::sha256File(f);
    r.kind = kind;
    r.source = {"tone3000", "77", "771", "https://www.tone3000.com/tones/77", "Fetched \xc3\xbc", "creator", "cc-by"};
    return r;
  };
  struct Case {
    const char* preset;
    Slot slot;
    int targetIndex;
    bool ir;
  };
  const Case cases[] = {{"golden_shared.json", Slot::SawPedal, 0, false}, {"golden_shared.json", Slot::SawAmp, 0, false},
                        {"golden_shared.json", Slot::BodyPedal, 0, false}, {"golden_shared.json", Slot::BodyAmp, 0, false},
                        {"golden_shared.json", Slot::Cab, 0, true},        {"golden_perpath.json", Slot::Cab, 0, true},
                        {"golden_perpath.json", Slot::Cab, 1, true}};
  for (const auto& c : cases) {
    INFO(c.preset << " slot " << slotName(c.slot) << " target " << c.targetIndex);
    Host h(48000.0, 256);
    h.load(kPresetDir / c.preset);
    const Preset before = h.p.currentPreset();
    const auto ts = slotTargets(before, c.slot);
    REQUIRE(ts.size() > static_cast<std::size_t>(c.targetIndex));
    std::string err;
    auto np = withCapture(before, ts[static_cast<std::size_t>(c.targetIndex)], fetched(c.ir ? ir : nam, c.ir ? "ir" : "nam"), err);
    REQUIRE(np.has_value());
    h.p.loadPreset(*np);
    REQUIRE(h.p.waitForLoader());
    CHECK(h.p.status().error.empty());
    const Preset after = h.p.currentPreset();
    const Capture* got;
    const auto& t = ts[static_cast<std::size_t>(c.targetIndex)];
    Preset expect = before;
    switch (t.kind) {
      case SlotTarget::Kind::CabShared: got = &after.cab.ir; expect.cab.ir = *got; break;
      case SlotTarget::Kind::CabA: got = &after.cab.irA; expect.cab.irA = *got; break;
      case SlotTarget::Kind::CabB: got = &after.cab.irB; expect.cab.irB = *got; break;
      default: {
        auto& pth = t.path == 'a' ? after.a : after.b;
        got = &static_cast<const NamBlockParams&>(*pth.blocks[static_cast<std::size_t>(t.blockIndex)].params).model;
        auto& ep = t.path == 'a' ? expect.a : expect.b;
        ep.blocks[static_cast<std::size_t>(t.blockIndex)] = pth.blocks[static_cast<std::size_t>(t.blockIndex)];
      }
    }
    CHECK(got->file == (c.ir ? ir : nam).string());
    REQUIRE(got->source.has_value());
    CHECK(got->source->provider == "tone3000");
    CHECK(got->source->id == "77");
    CHECK(got->source->modelId == "771");
    CHECK(got->source->url == "https://www.tone3000.com/tones/77");
    CHECK(got->source->title == "Fetched \xc3\xbc");
    CHECK(got->source->creator == "creator");
    CHECK(got->source->license == "cc-by");
    CHECK(after == expect);  // every other block unchanged
    if (!c.ir) {  // the swapped block keeps its gains, bypass, slot; only `model` differs
      const auto& pb = t.path == 'a' ? before.a : before.b;
      const auto& pa = t.path == 'a' ? after.a : after.b;
      const auto& bb = pb.blocks[static_cast<std::size_t>(t.blockIndex)];
      const auto& ab = pa.blocks[static_cast<std::size_t>(t.blockIndex)];
      const auto& bp = static_cast<const NamBlockParams&>(*bb.params);
      const auto& ap = static_cast<const NamBlockParams&>(*ab.params);
      CHECK(ab.id == bb.id);
      CHECK(ab.slot == bb.slot);
      CHECK(ab.bypass == bb.bypass);
      CHECK(ap.inputGainDb == bp.inputGainDb);
      CHECK(ap.outputGainDb == bp.outputGainDb);
      CHECK(ap.normalizeLoudness == bp.normalizeLoudness);
      CHECK(ap.model.file != bp.model.file);
    }

    // the saved state keeps it
    juce::MemoryBlock mb;
    h.p.getStateInformation(mb);
    Host h2(48000.0, 256);
    h2.p.setStateInformation(mb.getData(), static_cast<int>(mb.getSize()));
    REQUIRE(h2.p.waitForLoader());
    CHECK(h2.p.status().error.empty());
    // (the saved state writes absolute capture paths, so compare the substituted capture, not the whole preset)
    const Preset restored = h2.p.currentPreset();
    const Capture* rgot;
    switch (t.kind) {
      case SlotTarget::Kind::CabShared: rgot = &restored.cab.ir; break;
      case SlotTarget::Kind::CabA: rgot = &restored.cab.irA; break;
      case SlotTarget::Kind::CabB: rgot = &restored.cab.irB; break;
      default: rgot = &static_cast<const NamBlockParams&>(*(t.path == 'a' ? restored.a : restored.b).blocks[static_cast<std::size_t>(t.blockIndex)].params).model;
    }
    CHECK(*rgot == *got);
  }
}

TEST_CASE("browser swap: a load failure surfaces through status().error and keeps the old preset", "[browser][swap]") {
  Host h(48000.0, 256);
  h.load(kPresetDir / "golden_shared.json");
  const Preset before = h.p.currentPreset();
  t3k::FetchResult f;
  f.path = "/nonexistent/none.nam";
  f.sha256 = "";
  f.kind = "nam";
  f.source = {"tone3000", "1", "2", "", "t", "c", "cc-by"};
  std::string err;
  auto np = withCapture(before, slotTargets(before, Slot::SawAmp).at(0), f, err);
  REQUIRE(np.has_value());
  h.p.loadPreset(*np);
  REQUIRE(h.p.waitForLoader());
  CHECK_FALSE(h.p.status().error.empty());
  CHECK(h.p.currentPreset() == before);
}

// ---- v0.8 I4b: the preview renders with the calibration playback uses -------------------------------------------------------------
namespace {
// Path A = one amp capture (linear identity with invented dBu metadata), path B off, no cab; v5 "calibrated".
Preset ampRig(const char* amp) {
  const json a = json::array({json{{"id", "a1"}, {"type", "nam"}, {"slot", "amp"}, {"model", {{"file", (kFixtures / "nam" / amp).string()}}}}});
  const json b = json::array({json{{"id", "b1"}, {"type", "nam"}, {"model", {{"file", (kFixtures / "nam" / "linear_identity.nam").string()}}}}});
  const json j = {{"schema", "sawblade.preset"}, {"version", 5}, {"name", "amp swap"}, {"calibration", {{"mode", "calibrated"}}},
                  {"paths", {{"a", {{"blocks", a}}}, {"b", {{"enabled", false}, {"blocks", b}}}}},
                  {"align", {{"mode", "off"}}}, {"blend", 0.0},
                  {"cab", {{"mode", "shared"}, {"enabled", false}, {"ir", {{"file", "(none)"}}}}}};
  return parsePreset(j, kFixtures);
}

double rmsDbOf(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return 10.0 * std::log10(s / static_cast<double>(std::max<std::size_t>(1, x.size())) + 1e-30);
}
}  // namespace

TEST_CASE("preview render: with calibration on an amp swap shows the planned gain difference playback has; off it is bit-identical (v0.8 I4b)",
          "[browser][preview][devicecal]") {
  AudioFile riff = embeddedPreviewRiff();
  for (float& v : riff.interleaved) v *= 0.1f;  // headroom: nothing must reach the preview's safety limit
  const Preset hi = ampRig("cal_amp_hi.nam"), lo = ampRig("cal_amp_lo.nam");  // input levels 12 and 18 dBu: at +12 dBu lo is planned 6 dB below hi
  ChainCalibration cal;
  cal.enabled = true;
  cal.device.dbu = 12.0;
  std::string err;

  const auto previewHi = renderPreview(hi, riff, 48000.0, nullptr, err, /*levelMatched=*/true, cal);
  const auto previewLo = renderPreview(lo, riff, 48000.0, nullptr, err, /*levelMatched=*/true, cal);
  REQUIRE_FALSE(previewHi.empty());
  REQUIRE_FALSE(previewLo.empty());
  const double previewDiff = rmsDbOf(previewLo) - rmsDbOf(previewHi);
  CHECK(previewDiff == Catch::Approx(-6.0206).margin(0.05));

  // Playback: the engine built with the same calibration runs the same riff.
  const auto played = [&](const Preset& p) {
    EngineCalibration ec;
    ec.chain = cal;
    auto e = Engine::build(p, 48000.0, 512, nullptr, ec);
    std::vector<float> in = riff.interleaved, out(in.size());
    for (std::size_t pos = 0; pos < in.size(); pos += 512) {
      const auto n = static_cast<int>(std::min<std::size_t>(512, in.size() - pos));
      e->process(in.data() + pos, out.data() + pos, n);
    }
    return out;
  };
  const double playDiff = rmsDbOf(played(lo)) - rmsDbOf(played(hi));
  CHECK(playDiff == Catch::Approx(previewDiff).margin(0.05));

  // Off (the default argument, and an explicit off): no planned gain, the very same samples as before I4b.
  const auto offHi = renderPreview(hi, riff, 48000.0, nullptr, err, true);
  const auto offLo = renderPreview(lo, riff, 48000.0, nullptr, err, true);
  CHECK(offHi == renderPreview(hi, riff, 48000.0, nullptr, err, true, ChainCalibration{}));
  CHECK(offLo == renderPreview(lo, riff, 48000.0, nullptr, err, true, ChainCalibration{}));
  CHECK(rmsDbOf(offLo) == Catch::Approx(rmsDbOf(offHi)).margin(1e-6));
  CHECK(offHi != previewHi);  // and calibration really did something
}
