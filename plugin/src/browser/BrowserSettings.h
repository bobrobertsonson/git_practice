#pragma once

#include <memory>
#include <string>

#include <juce_data_structures/juce_data_structures.h>

namespace sawblade::plugin {

// Where `sawblade-t3k` lives. Stored in the user app-data folder (Sawblade/browser.settings), never in the
// plugin state. Default: <repo>/match/.venv/bin/sawblade-t3k with <repo> = SAWBLADE_REPO_DIR.
class BrowserSettings {
 public:
  BrowserSettings();                              // the real file
  explicit BrowserSettings(const juce::File& f);  // tests
  ~BrowserSettings();

  std::string executable() const;
  void setExecutable(const std::string& path);  // "" resets
  void reset() { setExecutable({}); }
  static std::string defaultExecutable();

 private:
  std::unique_ptr<juce::PropertiesFile> file_;
};

}  // namespace sawblade::plugin
