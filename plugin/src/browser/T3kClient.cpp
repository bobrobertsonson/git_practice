#include "T3kClient.h"

#include <algorithm>

namespace sawblade::plugin {
namespace {

constexpr int kLoginDefaultMs = 15 * 60 * 1000;

// The last non-empty lines of the merged output, bounded: shown when a failure has no error object.
std::string tail(const std::string& out) {
  std::string t = out;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  constexpr std::size_t kMax = 300;
  if (t.size() > kMax) t = t.substr(t.size() - kMax);
  if (const auto nl = t.find('\n'); nl != std::string::npos && t.size() > 120) t = t.substr(nl + 1);
  return t;
}

// Failure of a run: launch / timeout / the CLI's error object / a generic one with the output tail.
t3k::ErrorInfo failure(const RunResult& r) {
  if (!r.launched) return {r.launchError.empty() ? "could not run sawblade-t3k" : r.launchError, "launch"};
  if (r.timedOut) return {"sawblade-t3k timed out", "timeout"};
  if (r.cancelled) return {"cancelled", "cancelled"};
  if (auto e = t3k::parseErrorObject(r.output)) return *e;
  const std::string t = tail(r.output);
  return {"sawblade-t3k exited with status " + std::to_string(r.exitCode) + (t.empty() ? "" : ": " + t), "exit"};
}

}  // namespace

std::string describe(const t3k::ErrorInfo& e) { return e.message + " (" + e.code + ")"; }

template <class T>
void T3kClient::run(T3kRunner::Job job, std::function<std::optional<T>(const std::string&, std::string&)> parse, std::function<void(Reply<T>)> cb) {
  job.onDone = [parse = std::move(parse), cb = std::move(cb)](RunResult r) {
    Reply<T> out;
    if (r.launched && !r.timedOut && !r.cancelled && r.exitCode == 0) {
      std::string err;
      if (auto v = parse(r.output, err)) {
        out.ok = true;
        out.value = std::move(*v);
      } else {
        out.error = {"unreadable answer from sawblade-t3k: " + err, "parse"};
      }
    } else {
      out.error = failure(r);
    }
    cb(std::move(out));
  };
  runner_.submit(std::move(job));
}

void T3kClient::whoami(std::function<void(Reply<t3k::WhoAmI>)> cb) {
  T3kRunner::Job j;
  j.args = {"whoami", "--json"};
  j.timeoutMs = queryTimeoutMs;
  j.supersedeKey = "whoami";
  run<t3k::WhoAmI>(std::move(j), [](const std::string& s, std::string& e) { return t3k::parseWhoAmI(s, e); }, std::move(cb));
}

std::vector<std::string> T3kClient::listArgs(Source source, const std::string& query, const std::string& gear, int limit) {
  std::vector<std::string> a;
  if (source == Source::Search) a = {"search", "--json"};
  else a = {"list", "--source", source == Source::Pool ? "pool" : "favorites", "--json"};
  if (source != Source::Search && !query.empty()) a.push_back("--query=" + query);  // '=' keeps a leading '-' intact
  a.push_back("--limit");
  a.push_back(std::to_string(limit));
  if (!gear.empty()) {
    a.push_back("--gear");
    a.push_back(gear);
  }
  if (source == Source::Search) {  // the query verbatim, after "--" (it may start with '-')
    a.push_back("--");
    a.push_back(query);
  }
  return a;
}

void T3kClient::list(Source source, const std::string& query, const std::string& gear, int limit, std::function<void(Reply<Records>)> cb) {
  T3kRunner::Job j;
  j.args = listArgs(source, query, gear, limit);
  j.timeoutMs = queryTimeoutMs;
  j.supersedeKey = "list";
  run<Records>(std::move(j), [](const std::string& s, std::string& e) { return t3k::parseRecords(s, e); }, std::move(cb));
}

void T3kClient::models(std::int64_t toneId, std::function<void(Reply<t3k::ModelsResult>)> cb) {
  T3kRunner::Job j;
  j.args = {"models", std::to_string(toneId), "--json"};
  j.timeoutMs = queryTimeoutMs;
  j.supersedeKey = "models";
  run<t3k::ModelsResult>(std::move(j), [](const std::string& s, std::string& e) { return t3k::parseModels(s, e); }, std::move(cb));
}

void T3kClient::fetch(std::int64_t toneId, std::int64_t modelId, std::function<void(Reply<t3k::FetchResult>)> cb) {
  T3kRunner::Job j;
  j.args = {"fetch", std::to_string(toneId), "--json"};
  if (modelId > 0) {
    j.args.push_back("--model");
    j.args.push_back(std::to_string(modelId));
  }
  j.timeoutMs = fetchTimeoutMs;
  j.supersedeKey = "fetch";
  run<t3k::FetchResult>(std::move(j), [](const std::string& s, std::string& e) { return t3k::parseFetch(s, e); }, std::move(cb));
}

void T3kClient::login(std::function<void(const t3k::LoginEvent&)> onEvent, std::function<void(Reply<bool>)> done) {
  struct State {  // message thread
    bool loggedIn = false;
  };
  auto st = std::make_shared<State>();
  T3kRunner::Job j;
  j.args = {"login", "--json-events"};
  j.timeoutMs = kLoginDefaultMs;
  j.keepOutput = false;  // may hold a token: never kept, never shown
  j.onWorkerLine = [](const std::string& line) {
    const auto ev = t3k::parseLoginLine(line);
    return ev && ev->kind == t3k::LoginEvent::Kind::DeviceCode && ev->expiresIn > 0 ? (ev->expiresIn + 10) * 1000 : 0;
  };
  j.onLine = [st, onEvent = std::move(onEvent)](const std::string& line) {
    if (auto ev = t3k::parseLoginLine(line)) {
      if (ev->kind == t3k::LoginEvent::Kind::LoggedIn) st->loggedIn = true;
      onEvent(*ev);
    }  // anything else is discarded
  };
  j.onDone = [st, done = std::move(done)](RunResult r) {
    Reply<bool> out;
    if (st->loggedIn && !r.timedOut) {
      out.ok = true;
      out.value = true;
    } else if (r.timedOut) {
      out.error = {"the login code expired", "timeout"};
    } else if (!r.launched) {
      out.error = {r.launchError, "launch"};
    } else if (r.cancelled) {
      out.error = {"cancelled", "cancelled"};
    } else {
      out.error = {"login did not complete (exit status " + std::to_string(r.exitCode) + ")", "exit"};
    }
    done(std::move(out));
  };
  runner_.submit(std::move(j));
}

}  // namespace sawblade::plugin
