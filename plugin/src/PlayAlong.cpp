#include "PlayAlong.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

#ifdef SAWBLADE_WITH_SEPARATOR
#include "sawblade/separate_song.h"
#endif

namespace sawblade::plugin {
namespace {

double clampd(double v, double lo, double hi, double fallback) {
  if (!std::isfinite(v)) return fallback;
  return std::clamp(v, lo, hi);
}

const char* modeName(GuitarMode m) {
  switch (m) {
    case GuitarMode::Muted: return "mute";
    case GuitarMode::Ghost: return "ghost";
    case GuitarMode::Full: return "full";
  }
  return "mute";
}

std::string prettyLoadError(std::string e, const std::string& folder) {
  if (e.find("no .wav or .flac") != std::string::npos) return "No .wav or .flac stems found in " + folder;
  if (e.rfind("stems: ", 0) == 0) e.erase(0, 7);
  return e;
}

// The separation thread runs at lowered priority so a host's audio and UI keep priority (ONNX Runtime's pool
// threads are created from it and inherit the setting). Best effort.
void lowerThreadPriority() {
#if defined(__linux__)
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#elif defined(__APPLE__)
  (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

std::string songNameOf(const std::string& file) { return std::filesystem::path(file).stem().string(); }

constexpr double kAbsGateLufs = -70.0, kRelGateLu = -10.0, kBs1770Offset = -0.691;

}  // namespace

// ---- settings <-> JSON ----------------------------------------------------------------------------
nlohmann::json playAlongToJson(const PlayAlongSettings& s) {
  nlohmann::json j = {{"folder", s.folder},
                      {"offsetMs", s.offsetMs},
                      {"loop", {{"on", s.loopOn}}},
                      {"countIn", {{"on", s.countIn}, {"bpm", s.bpm}}},
                      {"guitarMode", modeName(s.guitarMode)},
                      {"backingLevelDb", s.levelDb},
                      {"otherRole", s.keepOther ? "other" : "guitar"},
                      {"hostSync", s.hostSync}};
  // Optional keys (5.1b): present only when they differ from the defaults, so older sessions stay as they were.
  if (!s.songFile.empty()) j["songFile"] = s.songFile;
  if (s.fourStemModel) j["separationModel"] = separationModelId(SeparationModel::Htdemucs4s);
  if (s.loopAMs >= 0.0) j["loop"]["aMs"] = s.loopAMs;
  if (s.loopBMs >= 0.0) j["loop"]["bMs"] = s.loopBMs;
  return j;
}

PlayAlongSettings playAlongFromJson(const nlohmann::json& j) {
  using nlohmann::json;
  PlayAlongSettings s;
  if (!j.is_object()) return s;
  auto num = [](const json& o, const char* k, double& dst, double lo, double hi) {
    if (auto it = o.find(k); it != o.end() && it->is_number()) dst = clampd(it->get<double>(), lo, hi, dst);
  };
  auto flag = [](const json& o, const char* k, bool& dst) {
    if (auto it = o.find(k); it != o.end() && it->is_boolean()) dst = it->get<bool>();
  };
  if (auto it = j.find("folder"); it != j.end() && it->is_string()) s.folder = it->get<std::string>();
  if (auto it = j.find("songFile"); it != j.end() && it->is_string()) s.songFile = it->get<std::string>();
  if (auto it = j.find("separationModel"); it != j.end() && it->is_string())
    s.fourStemModel = it->get<std::string>() == separationModelId(SeparationModel::Htdemucs4s);
  if (!s.songFile.empty()) s.folder.clear();  // exclusive: a song file wins over a stale folder
  num(j, "offsetMs", s.offsetMs, -kOffsetLimitMs, kOffsetLimitMs);
  if (auto it = j.find("loop"); it != j.end() && it->is_object()) {
    flag(*it, "on", s.loopOn);
    num(*it, "aMs", s.loopAMs, 0.0, 1e9);
    num(*it, "bMs", s.loopBMs, 0.0, 1e9);
  }
  if (auto it = j.find("countIn"); it != j.end() && it->is_object()) {
    flag(*it, "on", s.countIn);
    num(*it, "bpm", s.bpm, 30.0, 300.0);
  }
  if (auto it = j.find("guitarMode"); it != j.end() && it->is_string()) {
    const auto v = it->get<std::string>();
    if (v == "mute") s.guitarMode = GuitarMode::Muted;
    else if (v == "ghost") s.guitarMode = GuitarMode::Ghost;
    else if (v == "full") s.guitarMode = GuitarMode::Full;
  }
  num(j, "backingLevelDb", s.levelDb, kBackingLevelMinDb, kBackingLevelMaxDb);
  if (auto it = j.find("otherRole"); it != j.end() && it->is_string()) s.keepOther = it->get<std::string>() == "other";
  flag(j, "hostSync", s.hostSync);
  return s;
}

bool isSongFileName(const std::string& path) { return isAudioFileName(path); }

double suggestedBackingLevelDb(std::optional<double> rigLufs, std::optional<double> backingLufs) {
  if (!backingLufs) return kBackingLevelDefaultDb;
  const double rig = rigLufs ? *rigLufs : kRigReferenceLufs;
  return std::clamp(rig - *backingLufs, kBackingLevelMinDb, kBackingLevelMaxDb);
}

// ---- RigLoudness ------------------------------------------------------------------------------------
void RigLoudness::prepare(double sampleRate) {
  const KWeighting k = designKWeighting(sampleRate);
  shelf_ = Bq{k.shelf};
  hp_ = Bq{k.highpass};
  hopN_ = std::max(1, static_cast<int>(std::lround(kHopSeconds * sampleRate)));
  hopPos_ = 0;
  acc_ = 0.0;
  decay_ = std::exp(-kHopSeconds / kMemorySeconds);
  absE_ = absW_ = relE_ = relW_ = 0.0;
  est_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
}

void RigLoudness::process(const float* x, int n) noexcept {
  for (int i = 0; i < n; ++i) {
    const double y = hp_.step(shelf_.step(static_cast<double>(x[i])));
    acc_ += y * y;
    if (++hopPos_ < hopN_) continue;
    const double ms = acc_ / hopN_;
    hopPos_ = 0;
    acc_ = 0.0;
    const double blockLufs = kBs1770Offset + 10.0 * std::log10(2.0 * ms + 1e-30);  // two equal channels
    if (blockLufs <= kAbsGateLufs) continue;
    absE_ = absE_ * decay_ + ms;
    absW_ = absW_ * decay_ + 1.0;
    const double relGate = kBs1770Offset + 10.0 * std::log10(2.0 * absE_ / absW_) + kRelGateLu;
    if (blockLufs <= relGate) continue;
    relE_ = relE_ * decay_ + ms;
    relW_ = relW_ * decay_ + 1.0;
    if (relW_ >= kMinGatedSeconds / kHopSeconds)
      est_.store(kBs1770Offset + 10.0 * std::log10(2.0 * relE_ / relW_), std::memory_order_relaxed);
  }
}

std::optional<double> RigLoudness::lufs() const noexcept {
  const double v = est_.load(std::memory_order_relaxed);
  if (std::isnan(v)) return std::nullopt;
  return v;
}

// ---- PlayAlong --------------------------------------------------------------------------------------
PlayAlong::PlayAlong() { thread_ = std::thread([this] { loaderMain(); }); }

PlayAlong::~PlayAlong() {
  {  // the separation worker first: it hands its result to the loader
    std::lock_guard<std::mutex> lk(sepM_);
    sepStop_ = true;
    sepPending_.reset();
    ++sepId_;
    if (sepRunningToken_) sepRunningToken_->cancel();
  }
  sepCv_.notify_all();
  if (sepThread_.joinable()) sepThread_.join();
  {
    std::lock_guard<std::mutex> lk(loaderM_);
    stop_ = true;
  }
  loaderCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PlayAlong::prepare(double sampleRate, int maxBlock, int maxRigLatencySamples) {
  {
    std::lock_guard<std::mutex> lk(prepareM_);
    rate_.store(sampleRate);
    player_.prepare({sampleRate, std::max(1, maxBlock)}, std::max(0, maxRigLatencySamples));
    loud_.prepare(sampleRate);
    followMode_ = !standalone();
    player_.setTransportMode(followMode_ ? TransportMode::HostFollow : TransportMode::FreeRun);
    appliedLatency_ = -1;  // re-applied by the next process()
  }
  applyAll();
  bool wanted, user;
  std::string folder;
  {
    std::lock_guard<std::mutex> lk(loaderM_);
    wanted = loadWanted_;
    user = wantedUser_;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    folder = settings_.folder;
  }
  if (!folder.empty() && (wanted || loadedRate_.load() != sampleRate)) requestLoad(wanted && user);
}

// ---- commands --------------------------------------------------------------------------------------
void PlayAlong::push(const PlayAlongCmd& c) {
  std::lock_guard<std::mutex> lk(producerM_);
  if (!queue_.push(c)) {
    dropped_.fetch_add(1);
    resync_.store(true);
  }
}

void PlayAlong::resyncIfNeeded() {
  if (resync_.exchange(false)) applyAll();  // a push dropped again inside re-arms the flag
}

void PlayAlong::pushLoop(const PlayAlongSettings& s, double rate) {
  if (s.loopOn && s.loopAMs >= 0.0 && s.loopBMs > s.loopAMs && rate > 0.0)
    push({PlayAlongCmd::Type::SetLoop, std::llround(s.loopAMs * 0.001 * rate), std::llround(s.loopBMs * 0.001 * rate), 0.0});
  else
    push({PlayAlongCmd::Type::ClearLoop, 0, 0, 0.0});
}

void PlayAlong::applyOffset(double ms) {
  const double rate = rate_.load();
  if (rate > 0.0) player_.setStartOffsetSamples(-static_cast<std::int64_t>(std::llround(ms * 0.001 * rate)));
}

void PlayAlong::applyAll() {
  PlayAlongSettings s;
  {
    std::lock_guard<std::mutex> lk(m_);
    s = settings_;
  }
  push({PlayAlongCmd::Type::GuitarMode, static_cast<std::int64_t>(s.guitarMode), 0, 0.0});
  push({PlayAlongCmd::Type::Level, 0, 0, s.levelDb});
  push({PlayAlongCmd::Type::CountIn, s.countIn ? 1 : 0, 0, s.bpm});
  push({PlayAlongCmd::Type::HostSync, s.hostSync ? 1 : 0, 0, 0.0});
  applyOffset(s.offsetMs);
  pushLoop(s, rate_.load());
}

void PlayAlong::play() { push({PlayAlongCmd::Type::Play, 0, 0, 0.0}); }
void PlayAlong::pause() { push({PlayAlongCmd::Type::Pause, 0, 0, 0.0}); }
void PlayAlong::seekSamples(std::int64_t p) { push({PlayAlongCmd::Type::Seek, p, 0, 0.0}); }

void PlayAlong::setLoopMs(double aMs, double bMs, bool on) {
  PlayAlongSettings s;
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.loopAMs = aMs;
    settings_.loopBMs = bMs;
    settings_.loopOn = on;
    s = settings_;
  }
  pushLoop(s, rate_.load());
}

void PlayAlong::setCountIn(bool on, double bpm) {
  bpm = clampd(bpm, 30.0, 300.0, kDefaultBpm);
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.countIn = on;
    settings_.bpm = bpm;
  }
  push({PlayAlongCmd::Type::CountIn, on ? 1 : 0, 0, bpm});
}

void PlayAlong::setGuitarMode(GuitarMode m) {
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.guitarMode = m;
  }
  push({PlayAlongCmd::Type::GuitarMode, static_cast<std::int64_t>(m), 0, 0.0});
}

void PlayAlong::setLevelDb(double db) {
  db = clampd(db, kBackingLevelMinDb, kBackingLevelMaxDb, kBackingLevelDefaultDb);
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.levelDb = db;
  }
  push({PlayAlongCmd::Type::Level, 0, 0, db});
}

void PlayAlong::setOffsetMs(double ms) {
  ms = clampd(ms, -kOffsetLimitMs, kOffsetLimitMs, 0.0);
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.offsetMs = ms;
  }
  applyOffset(ms);
}

void PlayAlong::setHostSync(bool on) {
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.hostSync = on;
  }
  push({PlayAlongCmd::Type::HostSync, on ? 1 : 0, 0, 0.0});
}

void PlayAlong::setKeepOther(bool keep) {
  bool reload;
  {
    std::lock_guard<std::mutex> lk(m_);
    reload = settings_.keepOther != keep;
    settings_.keepOther = keep;
  }
  if (reload) requestLoad(false);
}

void PlayAlong::loadFolder(const std::string& folder, bool userInitiated) {
  if (!classifyStemFolder(std::filesystem::path(folder)).ok) {
    std::lock_guard<std::mutex> lk(m_);
    status_.notice = "This folder is not a set of separated stems. Choose the song file (mp3, wav, flac, m4a) instead.";
    return;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_.folder = folder;
    settings_.songFile.clear();
  }
  requestLoad(userInitiated);
}

void PlayAlong::loadSong(const std::string& path, bool userInitiated) {
  std::error_code ec;
  if (!isSongFileName(path) || std::filesystem::is_directory(std::filesystem::path(path), ec)) {
    loadFolder(path, userInitiated);
    return;
  }
  std::string prevFolder, prevSong;
  {
    std::lock_guard<std::mutex> lk(m_);
    prevFolder = settings_.folder;
    prevSong = settings_.songFile;
    settings_.songFile = path;
    settings_.folder.clear();
  }
  PlayAlongSettings s = settings();
  {
    std::lock_guard<std::mutex> lk(sepM_);
    sepPrevFolder_ = prevFolder;
    sepPrevSong_ = prevSong;
  }
  requestSong(s, s.keepOther ? OtherRole::Other : OtherRole::Guitar, userInitiated, /*allowSeparate=*/true);
}

void PlayAlong::setFourStemModel(bool fourStem) {
  bool changed, haveSong;
  {
    std::lock_guard<std::mutex> lk(m_);
    changed = settings_.fourStemModel != fourStem;
    settings_.fourStemModel = fourStem;
    haveSong = !settings_.songFile.empty();
  }
  if (changed && haveSong) requestLoad(false, /*allowSeparate=*/true);  // the user picked another model
}

void PlayAlong::setSongDecoder(SongDecoder d) {
  std::lock_guard<std::mutex> lk(sepM_);
  decoder_ = std::move(d);
}

void PlayAlong::cancelSeparation() {
  std::string prevFolder, prevSong;
  {
    std::lock_guard<std::mutex> lk(sepM_);
    const bool active = sepPending_.has_value() || sepRunningToken_ != nullptr;
    if (!active) return;
    sepPending_.reset();
    ++sepId_;  // whatever the running job produces is dropped
    if (sepRunningToken_) sepRunningToken_->cancel();
    prevFolder = sepPrevFolder_;
    prevSong = sepPrevSong_;
    std::lock_guard<std::mutex> lk2(m_);  // sepM_ -> m_, the same order as the job's hand-off
    settings_.folder = prevFolder;
    settings_.songFile = prevSong;
    status_ = LoadStatus{};
    status_.state = LoadStatus::State::Cancelled;
    status_.message = "Separation cancelled.";
  }
}

void PlayAlong::restore(const PlayAlongSettings& in) {
  PlayAlongSettings s = in;  // already sanitised by playAlongFromJson; clamp again for direct callers
  s.levelDb = clampd(s.levelDb, kBackingLevelMinDb, kBackingLevelMaxDb, kBackingLevelDefaultDb);
  s.offsetMs = clampd(s.offsetMs, -kOffsetLimitMs, kOffsetLimitMs, 0.0);
  s.bpm = clampd(s.bpm, 30.0, 300.0, kDefaultBpm);
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_ = s;
  }
  applyAll();
  requestLoad(false);  // the restored level is kept: no suggestion
}

PlayAlongSettings PlayAlong::settings() const {
  std::lock_guard<std::mutex> lk(m_);
  return settings_;
}

std::string PlayAlong::activeStemsDir() const {
  std::lock_guard<std::mutex> lk(m_);
  if (!settings_.songFile.empty())
    return (songDirFile_ == settings_.songFile && songDirFourStem_ == settings_.fourStemModel) ? songDir_ : std::string();
  return settings_.folder;
}

PlayAlong::LoadStatus PlayAlong::loadStatus() const {
  LoadStatus st;
  {
    std::lock_guard<std::mutex> lk(m_);
    st = status_;
  }
  if (st.state == LoadStatus::State::Separating) {
    st.separationFraction = sepFraction_.load(std::memory_order_relaxed);
    st.separationEtaSeconds = sepEta_.load(std::memory_order_relaxed);
  }
  return st;
}

PlayAlong::Snapshot PlayAlong::snapshot() const noexcept {
  Snapshot s;
  s.hasSet = sHasSet_.load(std::memory_order_relaxed);
  s.playing = sPlaying_.load(std::memory_order_relaxed);
  s.countingIn = sCounting_.load(std::memory_order_relaxed);
  s.atEnd = sAtEnd_.load(std::memory_order_relaxed);
  s.loopActive = sLoop_.load(std::memory_order_relaxed);
  s.following = sFollowing_.load(std::memory_order_relaxed);
  s.position = sPos_.load(std::memory_order_relaxed);
  s.length = sLen_.load(std::memory_order_relaxed);
  s.loopStart = sLoopA_.load(std::memory_order_relaxed);
  s.loopEnd = sLoopB_.load(std::memory_order_relaxed);
  s.sampleRate = rate_.load(std::memory_order_relaxed);
  return s;
}

bool PlayAlong::waitForLoader(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  {
    std::unique_lock<std::mutex> lk(sepM_);
    if (!sepIdleCv_.wait_until(lk, deadline, [this] { return !sepPending_ && !sepBusy_; })) return false;
  }
  std::unique_lock<std::mutex> lk(loaderM_);
  return idleCv_.wait_until(lk, deadline, [this] { return !pending_ && !busy_; });
}

// ---- loading --------------------------------------------------------------------------------------
void PlayAlong::requestLoad(bool user, bool allowSeparate) {
  PlayAlongSettings s;
  {
    std::lock_guard<std::mutex> lk(m_);
    s = settings_;
  }
  const OtherRole role = s.keepOther ? OtherRole::Other : OtherRole::Guitar;
  if (!s.songFile.empty()) {
    {
      std::lock_guard<std::mutex> lk(sepM_);
      sepPrevFolder_ = s.folder;  // nothing to go back to: a cancel keeps the settings as they are
      sepPrevSong_ = s.songFile;
    }
    requestSong(s, role, user, allowSeparate);
    return;
  }
  supersedeSeparation();  // a folder (or nothing) replaces a song file that is still being separated
  if (s.folder.empty()) {
    std::lock_guard<std::mutex> lk(m_);
    status_ = LoadStatus{};
    return;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    status_ = LoadStatus{};
    status_.state = LoadStatus::State::Loading;
  }
  submitLoad(s.folder, "", role, user, /*cacheHit=*/false);
}

// The load part of requestLoad: hands `dir` to the loader thread (or parks it until prepare()).
void PlayAlong::submitLoad(const std::string& dir, const std::string& name, OtherRole role, bool user, bool cacheHit) {
  std::lock_guard<std::mutex> lk(loaderM_);
  const double rate = rate_.load();
  if (rate <= 0.0) {  // not prepared yet: prepare() submits it
    loadWanted_ = true;
    wantedUser_ = user;
    return;
  }
  loadWanted_ = false;
  pending_ = Request{dir, name, rate, role, user, ++requestId_, cacheHit};
  loaderCv_.notify_one();
}

void PlayAlong::supersedeSeparation() {
  std::lock_guard<std::mutex> lk(sepM_);
  sepPending_.reset();
  ++sepId_;
  if (sepRunningToken_) sepRunningToken_->cancel();
}

// ---- separation --------------------------------------------------------------------------------------
void PlayAlong::requestSong(const PlayAlongSettings& s, OtherRole role, bool user, bool allowSeparate) {
  namespace fs = std::filesystem;
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(m_);
    std::error_code ec;
    if (songDirFile_ == s.songFile && songDirFourStem_ == s.fourStemModel && !songDir_.empty() &&
        fs::is_directory(fs::path(songDir_), ec))
      dir = songDir_;  // separated already in this session (KEEP KEYS, a rate change, a restore)
    status_ = LoadStatus{};
    status_.state = (dir.empty() && allowSeparate) ? LoadStatus::State::Separating : LoadStatus::State::Loading;
  }
  if (!dir.empty()) {
    supersedeSeparation();
    submitLoad(dir, songNameOf(s.songFile), role, user, /*cacheHit=*/true);
    return;
  }
  sepFraction_.store(0.0);
  sepEta_.store(-1.0);
  SepRequest r;
  r.file = s.songFile;
  r.fourStem = s.fourStemModel;
  r.role = role;
  r.user = user;
  r.allowSeparate = allowSeparate;
  r.token = std::make_shared<CancelToken>();
  {
    std::lock_guard<std::mutex> lk(sepM_);
    if (sepStop_) return;
    if (sepRunningToken_) sepRunningToken_->cancel();  // the older job is superseded
    r.id = ++sepId_;
    sepPending_ = std::move(r);
    if (!sepThread_.joinable()) sepThread_ = std::thread([this] { separationMain(); });
  }
  sepCv_.notify_one();
}

void PlayAlong::separationMain() {
  lowerThreadPriority();
  std::unique_lock<std::mutex> lk(sepM_);
  while (!sepStop_) {
    if (!sepPending_) {
      sepCv_.wait(lk);
      continue;
    }
    const SepRequest r = *sepPending_;
    sepPending_.reset();
    sepRunningToken_ = r.token;
    sepBusy_ = true;
    lk.unlock();
    runSeparation(r);
    lk.lock();
    sepRunningToken_.reset();
    sepBusy_ = false;
    sepIdleCv_.notify_all();
  }
}

void PlayAlong::runSeparation(const SepRequest& r) {
  namespace fs = std::filesystem;
  auto current = [&] { return r.id == sepId_; };  // call with sepM_ held
  auto fail = [&](const std::string& message, bool modelMissing, const std::string& fetch) {
    std::lock_guard<std::mutex> lk(sepM_);
    if (!current()) return;
    std::lock_guard<std::mutex> lk2(m_);
    status_ = LoadStatus{};
    status_.state = LoadStatus::State::Failed;
    status_.message = message;
    status_.modelMissing = modelMissing;
    status_.fetchCommand = fetch;
  };
#ifdef SAWBLADE_WITH_SEPARATOR
  try {
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(r.file), ec)) {
      fail("Song file not found: " + r.file, false, "");
      return;
    }
    SeparateSongOptions o;
    o.model = r.fourStem ? SeparationModel::Htdemucs4s : SeparationModel::Htdemucs6s;
    o.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);  // leave the host room
    o.cacheOnly = !r.allowSeparate;
    {
      std::lock_guard<std::mutex> lk(sepM_);
      o.decoder = decoder_;
    }
    const SeparateSongResult res = separateSong(
        fs::path(r.file), o,
        [this, &r](double f, double eta) {
          if (r.id != sepId_.load(std::memory_order_relaxed)) return;  // superseded: the newer job owns the numbers
          sepFraction_.store(f, std::memory_order_relaxed);
          sepEta_.store(eta, std::memory_order_relaxed);
        },
        *r.token);
    // Hand the cache directory to the loader under sepM_, so a concurrent cancelSeparation() either
    // happens before (and the result is dropped) or after (and finds nothing left to cancel).
    std::lock_guard<std::mutex> lk(sepM_);
    if (!current()) return;
    {
      std::lock_guard<std::mutex> lk2(m_);
      songDir_ = res.stemsDir.string();
      songDirFile_ = r.file;
      songDirFourStem_ = r.fourStem;
      status_ = LoadStatus{};
      status_.state = LoadStatus::State::Loading;
    }
    sepRunningToken_.reset();  // nothing to cancel any more
    submitLoad(res.stemsDir.string(), songNameOf(r.file), r.role, r.user, res.cacheHit);
  } catch (const NotCached&) {
    std::lock_guard<std::mutex> lk(sepM_);
    if (!current()) return;
    std::lock_guard<std::mutex> lk2(m_);
    status_ = LoadStatus{};
    status_.state = LoadStatus::State::NotSeparated;
    status_.message = "Song not separated yet - CHOOSE SONG FILE to separate it";
  } catch (const SeparationCancelled&) {
    // Superseded or cancelled: whoever did that has set the status already.
  } catch (const ModelUnavailable& e) {
    fail(e.status.message, true, separationModelInstallCommand(e.status.model));
  } catch (const std::exception& e) {
    fail(std::string("Separation failed: ") + e.what(), false, "");
  } catch (...) {
    fail("Separation failed.", false, "");
  }
#else
  (void)fs::path();
  fail("Separation is not available in this build.", false, "");
#endif
}

void PlayAlong::loaderMain() {
  std::unique_lock<std::mutex> lk(loaderM_);
  while (!stop_) {
    if (!pending_) {
      if (loaderCv_.wait_for(lk, std::chrono::milliseconds(500)) == std::cv_status::timeout && !pending_ && !stop_) {
        lk.unlock();
        player_.collectGarbage();  // retired stem sets are freed here, never on the audio thread
        resyncIfNeeded();
        lk.lock();
      }
      continue;
    }
    const Request r = *pending_;
    pending_.reset();
    busy_ = true;
    lk.unlock();
    runLoad(r);
    player_.collectGarbage();
    lk.lock();
    busy_ = false;
    idleCv_.notify_all();
  }
}

void PlayAlong::runLoad(const Request& r) {
  namespace fs = std::filesystem;
  std::unique_ptr<StemSet> set;
  std::string error;
  try {
    std::error_code ec;
    if (!fs::is_directory(fs::path(r.folder), ec)) error = "Song folder not found: " + r.folder;
    else set = std::make_unique<StemSet>(loadStemDirectory(fs::path(r.folder), r.rate, r.role));
  } catch (const std::exception& e) {
    error = prettyLoadError(e.what(), r.folder);
  } catch (...) {
    error = "Could not load the stems in " + r.folder;
  }
  {
    std::lock_guard<std::mutex> lk(loaderM_);
    if (r.id != requestId_) return;  // superseded by a newer request: its status is Loading already
  }
  if (!set) {
    std::lock_guard<std::mutex> lk(m_);
    status_ = LoadStatus{};
    status_.state = LoadStatus::State::Failed;
    status_.message = error;
    return;
  }

  LoadStatus st;
  st.state = LoadStatus::State::Ready;
  st.songName = r.name;
  if (st.songName.empty()) st.songName = fs::path(r.folder).filename().string();
  if (st.songName.empty()) st.songName = fs::path(r.folder).parent_path().filename().string();
  st.cacheHit = r.cacheHit;
  st.warnings = set->warnings;
  st.backingLufs = set->backingLoudnessLufs;
  st.lengthSeconds = static_cast<double>(set->length) / r.rate;
  st.otherMappedToGuitar = set->otherMappedToGuitar;
  st.hasGuitarStem = set->present[static_cast<std::size_t>(StemKind::Guitar)];
  {
    std::lock_guard<std::mutex> lk(prepareM_);
    if (rate_.load() != r.rate) return;  // the host rate changed meanwhile: prepare() requested a reload
    try {
      player_.setStemSet(std::move(set));
    } catch (...) {
      return;
    }
  }
  loadedRate_.store(r.rate);
  if (r.user) {
    // One-time suggestion: the backing at the rig's loudness. Never adjusted again automatically.
    const double level = suggestedBackingLevelDb(loud_.lufs(), st.backingLufs);
    st.suggestedLevelDb = level;
    {
      std::lock_guard<std::mutex> lk(m_);
      settings_.levelDb = level;
    }
    push({PlayAlongCmd::Type::Level, 0, 0, level});
    // A replacement set is adopted only while the transport is stopped.
    if (standalone()) push({PlayAlongCmd::Type::Pause, 0, 0, 0.0});
  }
  std::lock_guard<std::mutex> lk(m_);
  status_ = std::move(st);
}

// ---- audio thread -------------------------------------------------------------------------------
void PlayAlong::applyCommand(const PlayAlongCmd& c) noexcept {
  using T = PlayAlongCmd::Type;
  switch (c.type) {
    case T::Play: player_.play(); break;
    case T::Pause: player_.pause(); break;
    case T::Seek: player_.seek(c.a); break;
    case T::SetLoop:
      wantLoop_ = true;
      loopA_ = c.a;
      loopB_ = c.b;
      player_.setLoop(c.a, c.b);  // retried every block until a set is adopted and the loop fits
      break;
    case T::ClearLoop:
      wantLoop_ = false;
      player_.clearLoop();
      break;
    case T::CountIn: player_.setCountIn(c.a != 0 ? 1 : 0, c.d); break;
    case T::GuitarMode: player_.setGuitarMode(static_cast<GuitarMode>(c.a)); break;
    case T::Level: player_.setMasterLevelDb(c.d); break;
    case T::HostSync: hostSync_ = c.a != 0; break;
  }
}

void PlayAlong::prepareBlock(const HostTransport& host) noexcept {
  PlayAlongCmd c;
  while (queue_.pop(c)) applyCommand(c);

  const int lat = rigLatency_.load(std::memory_order_relaxed);
  if (lat != appliedLatency_) {
    player_.setRigLatencySamples(lat);
    appliedLatency_ = lat;
  }
  const bool follow = !standalone();
  if (follow != followMode_) {
    followMode_ = follow;
    player_.setTransportMode(follow ? TransportMode::HostFollow : TransportMode::FreeRun);
  }
  // Plugin: the backing is off until host sync is enabled (the player then just fades to silence).
  if (follow) player_.setHostPosition(host.sample, hostSync_ && host.playing);
  blockPrepared_ = true;
}

TakeStartInfo PlayAlong::takeStartInfo(const HostTransport& host) const noexcept {
  TakeStartInfo info;
  info.hasSong = player_.hasStemSet();
  if (!info.hasSong) return info;
  info.stemSampleRate = rate_.load(std::memory_order_relaxed);
  const bool follow = !standalone();
  // Playhead p plays stem sample p - offset. In plugin mode the playhead is the host position.
  const std::int64_t playhead = follow ? host.sample : player_.position();
  info.running = follow ? (hostSync_ && host.playing) : (player_.isPlaying() && !player_.isCountingIn());
  info.stemSampleIndex = playhead - player_.appliedStartOffsetSamples();
  return info;
}

void PlayAlong::process(const float* rig, float* bl, float* br, int n, const HostTransport& host) noexcept {
  loud_.process(rig, n);
  if (!blockPrepared_) prepareBlock(host);
  blockPrepared_ = false;
  const bool follow = !standalone();

  player_.process(bl, br, n);

  if (wantLoop_ && !player_.loopActive() && player_.hasStemSet()) player_.setLoop(loopA_, loopB_);

  sHasSet_.store(player_.hasStemSet(), std::memory_order_relaxed);
  sPlaying_.store(player_.isPlaying(), std::memory_order_relaxed);
  sCounting_.store(player_.isCountingIn(), std::memory_order_relaxed);
  sAtEnd_.store(player_.atEnd(), std::memory_order_relaxed);
  sLoop_.store(player_.loopActive(), std::memory_order_relaxed);
  sFollowing_.store(follow && hostSync_, std::memory_order_relaxed);
  sPos_.store(player_.position(), std::memory_order_relaxed);
  sLen_.store(player_.playheadLength(), std::memory_order_relaxed);
  sLoopA_.store(player_.loopStart(), std::memory_order_relaxed);
  sLoopB_.store(player_.loopEnd(), std::memory_order_relaxed);
}

}  // namespace sawblade::plugin
