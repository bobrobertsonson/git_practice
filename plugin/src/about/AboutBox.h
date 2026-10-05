#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "settings/Settings.h"

namespace sawblade::plugin::about {

// The About overlay (docs/specs/phase11_settings.md section 5): icon, version line, licence note, the
// captures of the current preset with their creator / licence / link (the CLAUDE.md attribution rule made
// visible) and docs/THIRD_PARTY.md. An overlay over the whole editor, not a window, so it works in every
// host and in the editor tests.
class AboutBox : public juce::Component {
 public:
  AboutBox(SawbladeProcessor& p, settings::Settings& s);
  ~AboutBox() override;

  // Creates the overlay as a child of `parent` (filling it), owned by `slot`. CLOSE hides it at once and
  // destroys it on the next message-loop turn.
  static void show(juce::Component& parent, std::unique_ptr<AboutBox>& slot, SawbladeProcessor& p, settings::Settings& s);

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;
  void close();

  std::function<void()> onClose;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutBox)
};

}  // namespace sawblade::plugin::about
