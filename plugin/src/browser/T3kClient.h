#pragma once

// Typed wrapper over T3kRunner: builds the argv of each `sawblade-t3k` command, turns the RunResult into
// parsed structs or an ErrorInfo. All callbacks run on the message thread and never after destruction.

#include <functional>
#include <memory>
#include <string>
#include <variant>

#include "T3kJson.h"
#include "T3kRunner.h"

namespace sawblade::plugin {

template <class T>
struct Reply {
  bool ok = false;
  T value{};
  t3k::ErrorInfo error;
  bool superseded = false;
};

class T3kClient {
 public:
  enum class Source { Favorites, Search, Pool };
  using Records = std::vector<t3k::CaptureRecord>;

  explicit T3kClient(std::function<std::string()> executable) : runner_(std::move(executable)) {}

  void whoami(std::function<void(Reply<t3k::WhoAmI>)> cb);
  // gear: "pedal" | "amp" | "ir" ("" = any). `query` is required for Source::Search.
  void list(Source source, const std::string& query, const std::string& gear, int limit, std::function<void(Reply<Records>)> cb);
  void models(std::int64_t toneId, std::function<void(Reply<t3k::ModelsResult>)> cb);
  void fetch(std::int64_t toneId, std::int64_t modelId, std::function<void(Reply<t3k::FetchResult>)> cb);
  // onEvent: message thread, parsed events only (everything else the child prints is discarded, never kept).
  void login(std::function<void(const t3k::LoginEvent&)> onEvent, std::function<void(Reply<bool>)> done);
  void cancelAll() { runner_.cancelAll(); }

  // Per-job timeouts (ms); settable for tests.
  int queryTimeoutMs = 30000, fetchTimeoutMs = 120000;

  // Argument lists (tests).
  static std::vector<std::string> listArgs(Source source, const std::string& query, const std::string& gear, int limit);

 private:
  template <class T>
  void run(T3kRunner::Job job, std::function<std::optional<T>(const std::string&, std::string&)> parse, std::function<void(Reply<T>)> cb);
  T3kRunner runner_;
};

// What went wrong, as one line for the user: "<message> (<code>)".
std::string describe(const t3k::ErrorInfo& e);

}  // namespace sawblade::plugin
