#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"
#include "PresetInfoPanel.h"
#include "PresetLibrary.h"
#include "PresetLoadFlow.h"

namespace sawblade::plugin {

// The preset browser (docs/specs/phase9b_preset_browser.md section 5): an overlay over the rig + inspector (1280 x 742 design px
// below the top bar), opened from the top-bar preset selector. Left: search, banks (Factory: Classic / Styles / Matched; User) and
// the categories present, with counts. Middle: the preset list (double-click loads). Right: PresetInfoPanel. Footer: SAVE, SAVE AS,
// RENAME, DELETE (disabled for factory presets) and the resolve progress / messages. The previous / next buttons of the top bar
// step through the filtered list (step()).
class PresetBrowser : public juce::Component {
 public:
  static constexpr int kWidth = 1280, kHeight = 742;

  explicit PresetBrowser(SawbladeProcessor& p, LibraryConfig cfg = defaultLibraryConfig());
  ~PresetBrowser() override;

  std::function<void()> onClose;     // "< BACK"
  std::function<void()> onLoadFile;  // "LOAD FILE...": the editor's file chooser

  void paint(juce::Graphics&) override;
  void resized() override;

  // Shows the page's data: scans the library on a background thread (the list fills in when it is done).
  void open();
  void scanBlocking();  // tests: scan on this thread
  bool scanning() const;

  PresetLibrary& library();
  PresetInfoPanel& infoPanel();

  // --- filters and selection (the UI calls these too) ---
  void setSearch(const juce::String& s);
  // Rows: 0 All, 1 Factory, 2 Classic, 3 Styles, 4 Matched, 5 User.
  void selectBankRow(int row);
  void selectCategory(const juce::String& displayName);  // "" = all
  const std::vector<int>& visible() const;                // library indices passing the filters
  void selectEntry(int libraryIndex);
  int selectedEntry() const;
  juce::StringArray categoryRows() const;                  // "Name (count)" of the category list

  // --- loading ---
  // Loads through PresetLoadFlow (resolves TONE3000 captures first when needed). False if busy or the entry is not loadable.
  bool loadEntry(int libraryIndex);
  bool step(int direction);  // -1 / +1 through visible(), wrapping; skips unloadable presets
  bool resolving() const;
  juce::String message() const;
  juce::String currentFileName() const;

  // --- user operations (the buttons ask for names / confirmations, then call these) ---
  bool saveCurrent();  // over the current user preset, else Save As with the preset's name
  bool saveAs(const juce::String& name, const juce::String& category, bool overwrite);
  bool renameSelected(const juce::String& newName);
  bool deleteSelected();
  void setTrashFunction(PresetLibrary::TrashFn fn);
  juce::Button* buttonTitled(const juce::String& title);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowser)
};

}  // namespace sawblade::plugin
