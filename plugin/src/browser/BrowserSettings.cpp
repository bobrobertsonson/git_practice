#include "BrowserSettings.h"

#ifndef SAWBLADE_REPO_DIR
#define SAWBLADE_REPO_DIR "."
#endif

namespace sawblade::plugin {
namespace {
constexpr const char* kKey = "t3kExecutable";

juce::PropertiesFile::Options options() {
  juce::PropertiesFile::Options o;
  o.applicationName = "browser";
  o.folderName = "Sawblade";
  o.filenameSuffix = "settings";
  o.osxLibrarySubFolder = "Application Support";
  o.storageFormat = juce::PropertiesFile::storeAsXML;
  return o;
}
}  // namespace

BrowserSettings::BrowserSettings() : file_(std::make_unique<juce::PropertiesFile>(options().getDefaultFile(), options())) {}
BrowserSettings::BrowserSettings(const juce::File& f) : file_(std::make_unique<juce::PropertiesFile>(f, options())) {}
BrowserSettings::~BrowserSettings() = default;

std::string BrowserSettings::defaultExecutable() { return (juce::File(SAWBLADE_REPO_DIR).getChildFile("match/.venv/bin/sawblade-t3k")).getFullPathName().toStdString(); }

std::string BrowserSettings::executable() const {
  const juce::String s = file_->getValue(kKey);
  return s.isNotEmpty() ? s.toStdString() : defaultExecutable();
}

void BrowserSettings::setExecutable(const std::string& path) {
  if (path.empty() || path == defaultExecutable()) file_->removeValue(kKey);
  else file_->setValue(kKey, juce::String(path));
  file_->saveIfNeeded();
}

}  // namespace sawblade::plugin
