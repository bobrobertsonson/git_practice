#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PlayAlong.h"

// What the PLAY ALONG panel and the MATCH screen share for getting a song in (v0.2.1 Task D): the two explicit
// pickers (asynchronous native choosers) and what is done with their result, drops, and the load status line (separation
// progress, refusal notice, model-missing install command). Both views call this, so a pick or a drop behaves the same
// whichever view it comes from; each view only owns its own labels and its own short-lived "rejected pick" notice.
namespace sawblade::plugin::song_input {

// A one-purpose native chooser: a song file (files only, audio filter) or a stems folder (directories only, no filter).
// A combined files+directories chooser with a type filter is what the macOS panel greyed the .wav out of; see docs/PLUGIN.md.
enum class Action { SongFile, StemsFolder };
struct ChooserSpec {
  juce::String title, filter;  // filter: ';'-separated "*.ext" wildcards, empty = none
  int flags;                   // juce::FileBrowserComponent flags
};
#if JUCE_MAC
constexpr bool kIsMac = true;
#else
constexpr bool kIsMac = false;
#endif
// Pure: testable without a native dialog. mac: the song chooser uses the filter "*" (no allowed-types list, the panel
// delegate accepts everything) and handlePicked validates the pick instead.
ChooserSpec chooserSpec(Action a, bool mac = kIsMac);

// A rejected pick's message, shown in a view's status line for a few seconds.
struct PickNotice {
  juce::String text;
  std::uint32_t until = 0;
  void set(const juce::String& t) {
    text = t;
    until = juce::Time::getMillisecondCounter() + 8000;
  }
  void clear() { text.clear(); }
  juce::String active() const { return text.isNotEmpty() && juce::Time::getMillisecondCounter() < until ? text : juce::String(); }
};

// A chosen file / folder: validated (a non-song file or a non-folder is refused with a message in `notice` and the current
// song is kept, never passed to loadSong), then loaded as a user-initiated load. Returns whether it was loaded. An empty
// file = cancelled (false, notice untouched).
bool handlePicked(PlayAlong& pa, Action a, const juce::File& f, PickNotice& notice);

// Opens the native chooser for `a`; `holder` keeps it alive while it is open; `onPicked` gets the result (message thread).
void launchChooser(std::unique_ptr<juce::FileChooser>& holder, Action a, std::function<void(const juce::File&)> onPicked);

// Drops: a song file or a folder (of stems) is loaded as a user-initiated load. loadDroppedFiles returns whether something
// was handed to loadSong (a refused folder still counts: the refusal shows in the status line).
bool isLoadableDrop(const juce::StringArray& files);
bool loadDroppedFiles(PlayAlong& pa, const juce::StringArray& files);

// The load status as one status line. `pickNotice` is the view's active rejected-pick message ("" = none). `noneText` is
// what the view says when nothing was ever loaded.
struct StatusLine {
  juce::String text;          // the (short) line; for a missing model, the heading of the command field
  juce::String tooltip;       // the full line / message
  juce::Colour colour;
  bool separating = false;
  bool showFetch = false;     // model missing: show the install command field
  juce::String fetchCommand;
  juce::String fetchTooltip;  // for the COPY button
};
StatusLine statusLine(const PlayAlong::LoadStatus& st, bool standalone, bool hostSync, const juce::String& pickNotice, const juce::String& noneText);

// The wrapped, selectable, read-only field that shows the install command, and the longest font (9 pt down to 7 pt) at
// which the whole wrapped command fits it with no scrolling.
void styleFetchField(juce::TextEditor& field);
void fitFetchFont(juce::TextEditor& field);

}  // namespace sawblade::plugin::song_input
