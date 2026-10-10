#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "ToolRunner.h"

namespace sawblade::plugin::settings {

// Pure state machine behind the Settings panel's "Log in to TONE3000" box. Fed the lines of
// `sawblade-t3k login --json` (docs/specs/phase11_settings.md section 8):
//   {"event":"device_code","user_code":..,"verification_uri":..,"verification_uri_complete":..|null,"expires_in":N}
//   {"event":"logged_in","username":..,"display_name":..,"id":..,"token_file":..}
//   {"error":"<msg>","code":"auth|network|..."}      (the CLI's error shape; {"event":"error","message":..} is accepted too)
// `--json` is an alias of `--json-events`; username / display_name / id / token_file on logged_in are optional.
// Lines that are not JSON objects with an "event" or "error" key are ignored (log noise). Exit code 4 means "not logged in".
struct LoginFlow {
  enum class State { Idle, Starting, WaitingForApproval, LoggedIn, Failed };
  State state = State::Idle;
  std::string code, url, openUrl;  // user code, verification uri, what OPEN launches
  int expiresIn = 0;               // seconds, as announced
  std::string loggedInAs, displayName;
  std::string message;  // error text when Failed

  void begin() { *this = LoginFlow{}; state = State::Starting; }

  // Returns true if the state or any field changed.
  bool feedLine(const std::string& line) {
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto str = [&](const char* k) {
      auto it = j.find(k);
      return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    if (j.contains("error") && j["error"].is_string()) {  // upstream's error line
      message = str("error");
      if (message.empty()) message = "login failed";
      state = State::Failed;
      return true;
    }
    if (!j.contains("event") || !j["event"].is_string()) return false;
    const std::string ev = j["event"].get<std::string>();
    if (ev == "device_code") {
      code = str("user_code");
      url = str("verification_uri");
      const std::string full = str("verification_uri_complete");
      openUrl = full.empty() ? url : full;
      expiresIn = (j.contains("expires_in") && j["expires_in"].is_number()) ? j["expires_in"].get<int>() : 0;
      state = State::WaitingForApproval;
      return true;
    }
    if (ev == "logged_in") {
      loggedInAs = str("username");
      displayName = str("display_name");
      state = State::LoggedIn;
      return true;
    }
    if (ev == "error") {
      message = str("message");
      if (message.empty()) message = "login failed";
      state = State::Failed;
      return true;
    }
    return false;
  }

  // The process ended. A clean exit without a logged_in event, or a failure without an error event,
  // is a failure too.
  void finish(const ToolResult& r) {
    if (state == State::LoggedIn || state == State::Failed) return;
    if (r.outcome == ToolResult::Outcome::Cancelled) {
      state = State::Idle;
      return;
    }
    state = State::Failed;
    if (r.exitCode == 4) {
      message = "not logged in";
      return;
    }
    message = !r.error.empty() ? r.error : (r.lines.empty() ? "login ended without a result" : r.lines.back());
  }

  std::string loggedInText() const {
    if (loggedInAs.empty()) return displayName.empty() ? "Logged in" : "Logged in as " + displayName;
    std::string s = "Logged in as @" + loggedInAs;
    if (!displayName.empty()) s += " (" + displayName + ")";
    return s;
  }
};

// `sawblade-t3k whoami --json`: {"username":..,"display_name":..,"id":..,"token_file":..} or {"error":..}.
struct WhoamiResult {
  bool ok = false;
  std::string text;  // "Logged in as @user (Name)" or the error text
};
inline WhoamiResult parseWhoami(const ToolResult& r) {
  WhoamiResult w;
  if (r.json && r.json->is_object()) {
    const auto& j = *r.json;
    if (j.contains("error") && j["error"].is_string()) {
      w.text = j["error"].get<std::string>();
      return w;
    }
    if (j.contains("username") && j["username"].is_string()) {
      w.ok = true;
      w.text = "Logged in as @" + j["username"].get<std::string>();
      if (j.contains("display_name") && j["display_name"].is_string() && !j["display_name"].get<std::string>().empty())
        w.text += " (" + j["display_name"].get<std::string>() + ")";
      return w;
    }
  }
  if (r.exitCode == 4) w.text = "not logged in";
  else if (!r.error.empty()) w.text = r.error;
  else if (!r.lines.empty()) w.text = r.lines.back();
  else w.text = "whoami failed (exit " + std::to_string(r.exitCode) + ")";
  return w;
}

}  // namespace sawblade::plugin::settings
