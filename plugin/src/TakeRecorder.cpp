#include "TakeRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "AppPaths.h"

namespace sawblade::plugin {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kHeaderBytes = 56;  // RIFF/WAVE + fmt (16 bytes) + fact + data chunk headers
constexpr std::size_t kWriteChunk = 16384;

std::size_t nextPow2(std::size_t v) {
  std::size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

std::string utcNowIso() {
  // ISO 8601 UTC with milliseconds: takes recorded within the same second still sort by when they began.
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + n, sizeof buf - n, ".%03dZ", static_cast<int>(ms));
  return buf;
}

std::string takeStamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "take-%Y%m%d-%H%M%S", &tm);
  return buf;
}

void put32(char* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
void put16(char* p, std::uint16_t v) {
  p[0] = static_cast<char>(v & 0xff);
  p[1] = static_cast<char>((v >> 8) & 0xff);
}

// 32-bit float mono WAV header (format tag 3) with the sizes for `frames` frames.
void makeHeader(char* h, std::uint32_t rate, std::uint64_t frames) {
  const std::uint64_t dataBytes = std::min<std::uint64_t>(frames * 4, 0xffffffffull - 64);
  std::memcpy(h, "RIFF", 4);
  put32(h + 4, static_cast<std::uint32_t>(dataBytes + kHeaderBytes - 8));
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 3);  // IEEE float
  put16(h + 22, 1);  // mono
  put32(h + 24, rate);
  put32(h + 28, rate * 4);
  put16(h + 32, 4);
  put16(h + 34, 32);
  std::memcpy(h + 36, "fact", 4);
  put32(h + 40, 4);
  put32(h + 44, static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, 0xffffffffull)));
  std::memcpy(h + 48, "data", 4);
  put32(h + 52, static_cast<std::uint32_t>(dataBytes));
}

// A file name stem that cannot escape the directory or upset the OS.
std::string sanitizeName(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        static_cast<unsigned char>(c) < 0x20)
      continue;
    out.push_back(c);
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() > 120) out.resize(120);
  return out;
}

void writeAtomic(const fs::path& p, const std::string& text) {
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << text;
    f.flush();
    if (!f) throw std::runtime_error("cannot write " + p.string());
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) throw std::runtime_error("cannot write " + p.string() + ": " + ec.message());
}

}  // namespace

// ---- sidecar ---------------------------------------------------------------------------------------------------
std::optional<TakeInfo> readTakeSidecar(const fs::path& jsonPath) {
  try {
    std::ifstream f(jsonPath);
    if (!f) return std::nullopt;
    const json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    TakeInfo t;
    t.name = jsonPath.stem().string();
    t.json = jsonPath;
    t.wav = jsonPath;
    t.wav.replace_extension(".wav");
    t.sampleRate = j.value("sampleRate", 0.0);
    t.channels = j.value("channels", 1);
    t.lengthSamples = j.value("lengthSamples", static_cast<std::int64_t>(0));
    t.overruns = j.value("overruns", static_cast<std::uint64_t>(0));
    t.droppedSamples = j.value("droppedSamples", static_cast<std::uint64_t>(0));
    t.createdUtc = j.value("createdUtc", std::string());
    if (auto it = j.find("playAlong"); it != j.end() && it->is_object()) {
      t.hasPlayAlong = true;
      t.running = it->value("running", false);
      t.stemSampleIndex = it->value("stemSampleIndex", static_cast<std::int64_t>(0));
      t.stemSampleRate = it->value("stemSampleRate", 0.0);
      t.songFolder = it->value("songFolder", std::string());
    }
    if (!(t.sampleRate > 0.0)) return std::nullopt;
    return t;
  } catch (...) {
    return std::nullopt;
  }
}

// ---- writer-side file state --------------------------------------------------------------------------------------
struct TakeRecorder::OpenTake {
  std::ofstream file;
  fs::path wav, json;
  std::string name, songFolder, createdUtc;
  std::uint64_t frames = 0, overrunBlocks = 0, dropped = 0;
  TakeStartInfo info;
  double rate = 0.0;
  bool failed = false;
};

TakeRecorder::TakeRecorder() : dir_(defaultTakesDir()) {}

TakeRecorder::~TakeRecorder() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

// ---- message thread ----------------------------------------------------------------------------------------------
void TakeRecorder::prepare(double sampleRate) {
  // A take in progress ends here: the audio thread is idle during prepareToPlay, so this thread runs the
  // audio side's state machine once (n = 0 only finishes the take).
  int expected = kArmed;
  arm_.compare_exchange_strong(expected, kIdle);
  expected = kRecording;
  arm_.compare_exchange_strong(expected, kStopping);
  process(nullptr, 0, nullptr);
  waitIdle(std::chrono::milliseconds(5000));

  const std::size_t want = nextPow2(static_cast<std::size_t>(std::ceil(std::max(1.0, sampleRate) * kMinRingSeconds)));
  if (!thread_.joinable()) {
    ring_.assign(want, 0.0f);
    capacity_ = want;
    mask_ = want - 1;
    rate_.store(sampleRate);
    thread_ = std::thread([this] { writerMain(); });
    return;
  }
  // Idle (waitIdle above): nothing is queued, so the writer does not touch the ring while it is reset.
  std::lock_guard<std::mutex> lk(m_);
  const bool sameSize = want == capacity_;
  if (!sameSize) {
    ring_.assign(want, 0.0f);
    capacity_ = want;
    mask_ = want - 1;
  }
  // The ring and event positions only ever grow (head == tail here), so they are not reset: the writer thread
  // reads them concurrently and a half-done reset could look like pending events.
  audioState_ = AudioState::Idle;
  gapSamples_ = 0;
  gapBlocks_ = 0;
  rate_.store(sampleRate);
}

void TakeRecorder::setTakesDir(const fs::path& dir) {
  std::lock_guard<std::mutex> lk(m_);
  dir_ = dir;
}
fs::path TakeRecorder::takesDir() const {
  std::lock_guard<std::mutex> lk(m_);
  return dir_;
}

bool TakeRecorder::start(const std::string& songFolder) {
  if (rate_.load() <= 0.0 || !thread_.joinable()) {
    setError("Recording needs the audio engine to be running.");
    return false;
  }
  std::string name;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (arm_.load() != kIdle) {
      error_ = "The previous take is still being finished.";
      return false;
    }
    const std::string base = takeStamp();
    name = base;
    std::error_code ec;
    for (int i = 2; fs::exists(dir_ / (name + ".wav"), ec) || fs::exists(dir_ / (name + ".json"), ec); ++i) name = base + "-" + std::to_string(i);
    pending_ = Pending{name, songFolder, true};
    current_ = name;
    error_.clear();
    recorded_.store(0);
    overruns_.store(0);
    droppedSamples_.store(0);
  }
  int expected = kIdle;
  if (!arm_.compare_exchange_strong(expected, kArmed)) return false;
  return true;
}

void TakeRecorder::stop() {
  int expected = kArmed;
  if (arm_.compare_exchange_strong(expected, kIdle)) {  // never began
    std::lock_guard<std::mutex> lk(m_);
    pending_.valid = false;
    current_.clear();
    return;
  }
  expected = kRecording;
  arm_.compare_exchange_strong(expected, kStopping);
  cv_.notify_all();
}

TakeRecorder::State TakeRecorder::state() const noexcept {
  switch (arm_.load(std::memory_order_acquire)) {
    case kArmed: return State::Armed;
    case kRecording: return State::Recording;
    case kStopping: return State::Finalizing;
    default: return State::Idle;
  }
}

std::string TakeRecorder::lastError() const {
  std::lock_guard<std::mutex> lk(m_);
  return error_;
}
std::string TakeRecorder::currentTakeName() const {
  std::lock_guard<std::mutex> lk(m_);
  return current_;
}
void TakeRecorder::setError(const std::string& msg) {
  std::lock_guard<std::mutex> lk(m_);
  error_ = msg;
}

bool TakeRecorder::waitIdle(std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (arm_.load(std::memory_order_acquire) == kIdle && head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return arm_.load() == kIdle;
}

std::vector<TakeInfo> TakeRecorder::listTakes() const {
  std::vector<TakeInfo> out;
  const fs::path dir = takesDir();
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    if (auto t = readTakeSidecar(it->path()); t && fs::exists(t->wav, ec)) out.push_back(std::move(*t));
  }
  std::sort(out.begin(), out.end(), [](const TakeInfo& a, const TakeInfo& b) {
    if (a.createdUtc != b.createdUtc) return a.createdUtc > b.createdUtc;
    return a.name > b.name;
  });
  return out;
}

bool TakeRecorder::renameTake(const std::string& name, const std::string& newName, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  const std::string nn = sanitizeName(newName);
  if (nn.empty()) return fail("That name is not usable.");
  if (name == nn) return true;
  const fs::path dir = takesDir();
  std::error_code ec;
  if (name == currentTakeName() && arm_.load() != kIdle) return fail("That take is still recording.");
  if (!fs::exists(dir / (name + ".json"), ec)) return fail("The take no longer exists.");
  if (fs::exists(dir / (nn + ".wav"), ec) || fs::exists(dir / (nn + ".json"), ec)) return fail("A take with that name already exists.");
  fs::rename(dir / (name + ".wav"), dir / (nn + ".wav"), ec);
  if (ec) return fail("Could not rename the file.");
  fs::rename(dir / (name + ".json"), dir / (nn + ".json"), ec);
  if (ec) {
    fs::rename(dir / (nn + ".wav"), dir / (name + ".wav"), ec);
    return fail("Could not rename the file.");
  }
  version_.fetch_add(1);
  return true;
}

bool TakeRecorder::removeTake(const std::string& name) {
  const fs::path dir = takesDir();
  if (name.empty() || name != sanitizeName(name)) return false;
  if (name == currentTakeName() && arm_.load() != kIdle) return false;
  std::error_code ec;
  const bool a = fs::remove(dir / (name + ".json"), ec);
  const bool b = fs::remove(dir / (name + ".wav"), ec);
  version_.fetch_add(1);
  return a || b;
}

// ---- audio thread ------------------------------------------------------------------------------------------------
bool TakeRecorder::pushEvent(const Event& e) noexcept {
  const auto t = evTail_.load(std::memory_order_relaxed);
  if (t - evHead_.load(std::memory_order_acquire) >= kEventCap) return false;
  events_[t & (kEventCap - 1)] = e;
  evTail_.store(t + 1, std::memory_order_release);
  return true;
}

void TakeRecorder::process(const float* x, int n, const TakeStartInfo* info) noexcept {
  const int a = arm_.load(std::memory_order_acquire);
  if (audioState_ == AudioState::Idle) {
    if (a != kArmed || eventSpace() < 3) return;
    int expected = kArmed;
    if (!arm_.compare_exchange_strong(expected, kRecording)) return;  // stop() cancelled it
    Event e;
    e.type = Event::Type::Start;
    e.pos = tail_.load(std::memory_order_relaxed);
    if (info != nullptr) e.info = *info;
    pushEvent(e);  // room was checked above and this thread is the only producer
    audioState_ = AudioState::Recording;
    gapSamples_ = 0;
    gapBlocks_ = 0;
  } else if (a == kStopping) {
    // Finish: at most one Gap and the End, room for both was reserved when the take began... but the queue may
    // have filled since, so check again and retry on the next call.
    if (eventSpace() < 2) return;
    const std::uint64_t pos = tail_.load(std::memory_order_relaxed);
    if (gapSamples_ > 0) {
      Event g;
      g.type = Event::Type::Gap;
      g.pos = pos;
      g.samples = gapSamples_;
      g.blocks = gapBlocks_;
      pushEvent(g);
      gapSamples_ = 0;
      gapBlocks_ = 0;
    }
    Event e;
    e.type = Event::Type::End;
    e.pos = pos;
    pushEvent(e);
    audioState_ = AudioState::Idle;
    return;
  }
  if (audioState_ != AudioState::Recording || n <= 0 || x == nullptr) return;

  const std::uint64_t t = tail_.load(std::memory_order_relaxed);
  const std::uint64_t used = t - head_.load(std::memory_order_acquire);
  const auto un = static_cast<std::uint64_t>(n);
  bool fits = capacity_ - used >= un;
  if (fits && gapSamples_ > 0) {
    // Room again: tell the writer where the silence belongs (before this block's samples).
    if (eventSpace() >= 3) {
      Event g;
      g.type = Event::Type::Gap;
      g.pos = t;
      g.samples = gapSamples_;
      g.blocks = gapBlocks_;
      pushEvent(g);
      gapSamples_ = 0;
      gapBlocks_ = 0;
    } else {
      fits = false;
    }
  }
  recorded_.fetch_add(un, std::memory_order_relaxed);
  if (!fits) {
    gapSamples_ += un;
    ++gapBlocks_;
    overruns_.fetch_add(1, std::memory_order_relaxed);
    droppedSamples_.fetch_add(un, std::memory_order_relaxed);
    return;
  }
  const std::size_t start = static_cast<std::size_t>(t) & mask_;
  const std::size_t first = std::min<std::size_t>(static_cast<std::size_t>(n), capacity_ - start);
  std::memcpy(ring_.data() + start, x, first * sizeof(float));
  if (first < static_cast<std::size_t>(n)) std::memcpy(ring_.data(), x + first, (static_cast<std::size_t>(n) - first) * sizeof(float));
  tail_.store(t + un, std::memory_order_release);
}

// ---- writer thread -----------------------------------------------------------------------------------------------
void TakeRecorder::writerMain() {
  std::unique_lock<std::mutex> lk(m_);
  while (!stop_) {
    lk.unlock();
    if (!stalled_.load(std::memory_order_relaxed)) writerPass();
    lk.lock();
    cv_.wait_for(lk, std::chrono::milliseconds(8), [this] { return stop_; });
  }
  lk.unlock();
  writerPass();  // whatever is left
  if (open_) endTake();
}

void TakeRecorder::writerPass() {
  // Read the data position first: every event stamped at or before it is then visible (the audio thread
  // publishes an event before the samples that follow it).
  const std::uint64_t t = tail_.load(std::memory_order_acquire);
  for (;;) {
    const auto eh = evHead_.load(std::memory_order_relaxed);
    if (eh == evTail_.load(std::memory_order_acquire)) break;
    const Event e = events_[eh & (kEventCap - 1)];
    if (e.pos > t) break;
    drainRing(e.pos);
    handleEvent(e);
    evHead_.store(eh + 1, std::memory_order_release);
  }
  drainRing(t);
}

void TakeRecorder::drainRing(std::uint64_t upTo) {
  std::uint64_t h = head_.load(std::memory_order_relaxed);
  while (h < upTo) {
    const std::size_t start = static_cast<std::size_t>(h) & mask_;
    const std::size_t n = std::min<std::uint64_t>({upTo - h, capacity_ - start, kWriteChunk});
    writeSamples(ring_.data() + start, n);
    h += n;
    head_.store(h, std::memory_order_release);
  }
}

void TakeRecorder::handleEvent(const Event& e) {
  switch (e.type) {
    case Event::Type::Start: beginTake(e); break;
    case Event::Type::Gap:
      if (open_) {
        open_->overrunBlocks += e.blocks;
        open_->dropped += e.samples;
        writeZeros(e.samples);
      }
      break;
    case Event::Type::End: endTake(); break;
  }
}

void TakeRecorder::failTake(const std::string& msg) {
  if (open_) open_->failed = true;
  setError(msg);
}

void TakeRecorder::beginTake(const Event& e) {
  Pending p;
  fs::path dir;
  {
    std::lock_guard<std::mutex> lk(m_);
    p = pending_;
    pending_.valid = false;
    dir = dir_;
  }
  open_ = std::make_unique<OpenTake>();
  open_->name = p.valid ? p.name : takeStamp();
  open_->songFolder = p.songFolder;
  open_->info = e.info;
  open_->rate = rate_.load();
  open_->createdUtc = utcNowIso();
  open_->wav = dir / (open_->name + ".wav");
  open_->json = dir / (open_->name + ".json");
  std::error_code ec;
  fs::create_directories(dir, ec);
  open_->file.open(open_->wav, std::ios::binary | std::ios::trunc);
  if (!open_->file) {
    failTake("Cannot create " + open_->wav.string());
    return;
  }
  char h[kHeaderBytes];
  makeHeader(h, static_cast<std::uint32_t>(std::lround(open_->rate)), 0);
  open_->file.write(h, kHeaderBytes);
  if (!open_->file) failTake("Cannot write " + open_->wav.string());
}

void TakeRecorder::writeSamples(const float* x, std::size_t n) {
  if (!open_ || open_->failed) return;
  open_->file.write(reinterpret_cast<const char*>(x), static_cast<std::streamsize>(n * sizeof(float)));
  if (!open_->file) {
    failTake("Disk write failed for " + open_->wav.string());
    return;
  }
  open_->frames += n;
}

void TakeRecorder::writeZeros(std::uint64_t n) {
  static const std::array<float, 4096> zeros{};
  while (n > 0) {
    const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, zeros.size()));
    writeSamples(zeros.data(), k);
    n -= k;
  }
}

void TakeRecorder::endTake() {
  if (!open_) {
    arm_.store(kIdle, std::memory_order_release);
    return;
  }
  std::unique_ptr<OpenTake> t = std::move(open_);
  if (!t->failed) {
    char h[kHeaderBytes];
    makeHeader(h, static_cast<std::uint32_t>(std::lround(t->rate)), t->frames);
    t->file.seekp(0);
    t->file.write(h, kHeaderBytes);
    t->file.flush();
    if (!t->file) t->failed = true;
  }
  t->file.close();
  if (!t->failed) {
    json j;
    j["version"] = 1;
    j["sampleRate"] = t->rate;
    j["channels"] = 1;
    j["lengthSamples"] = t->frames;
    j["overruns"] = t->overrunBlocks;
    j["droppedSamples"] = t->dropped;
    if (t->info.hasSong)
      j["playAlong"] = {{"running", t->info.running}, {"stemSampleIndex", t->info.stemSampleIndex}, {"stemSampleRate", t->info.stemSampleRate},
                        {"songFolder", t->songFolder}};
    else
      j["playAlong"] = nullptr;
    j["createdUtc"] = t->createdUtc;
    try {
      writeAtomic(t->json, j.dump(2) + "\n");
    } catch (const std::exception& e) {
      setError(e.what());
    }
  } else {
    std::error_code ec;
    fs::remove(t->wav, ec);
  }
  version_.fetch_add(1);
  arm_.store(kIdle, std::memory_order_release);
}

}  // namespace sawblade::plugin
