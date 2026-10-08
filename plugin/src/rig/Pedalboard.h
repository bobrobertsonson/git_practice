#pragma once

// The pedalboards of the main page (v0.4 Tasks D and C). One component spans the rig area (940 x 742 design px, rig coordinates) and
// owns both boards, SAW (path A, left column) and BODY (path B, right column), so that a drag between the paths is handled in one place.
// The editor lays it over the RigView (the room, the amp heads, the cables, the cab chip).
//
// A path's board shows its blocks BEFORE its amp (all blocks when the path has no amp), in path order, as tiles; blocks after the amp
// are reachable from the rig editor only and are counted in a "+n AFTER AMP (rig editor)" note. Path B off: the board is empty, dimmed,
// says "BODY PATH OFF - turn up BLEND" and is not a drop target; SAW's geometry never depends on B.
//
// A tile is a pedal render with a name chip over its baked caption, a footswitch + LED (bypass) and a selection ring. Tiles are
// min(180, ..) .. 110 px wide; a board with more tiles than fit scrolls horizontally (juce::Viewport, bar at the bottom).
//
// Editing (all by this component's own mouse handling, no juce::DragAndDropContainer):
//   drag      press on a tile's body and move 6 px: a translucent ghost follows the mouse, the target board's outline lights up and an
//             insertion bar shows the index; dropping on a board reorders (same path) or moves the pedal (A <-> B); dropping outside both
//             boards REMOVES it (the ghost says REMOVE). A full target path (8 blocks incl. the amp) refuses a cross-path drop with a
//             status message. The footswitch and the live face's knobs never start a drag.
//   menu      right-click / ctrl-click on a tile: BYPASS / REMOVE.
//   + PEDAL   opens the PedalPicker (MODELED tab; Task B adds CAPTURES); the new pedal goes in before the amp, at the end of the board.
// EVERY edit (move, add, remove, bypass) is exactly one RigController::edit, i.e. one undo step (a move between paths removes and inserts
// in that one edit; the moved block gets a fresh id and keeps everything else). The board re-reads the preset right after its own edits.
//
// Refresh: the board rebuilds its tiles only when the shown block list changed, and never while a tile is pressed or dragged (the refresh
// is skipped until the mouse goes up), so a refresh cannot destroy a widget under the user's hand.

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "browser/T3kClient.h"
#include "rig/CapturePedals.h"
#include "rig/PedalKind.h"
#include "rig/PedalPicker.h"
#include "rig/RigController.h"
#include "rig/RigWidgets.h"
#include "skin/FootswitchButton.h"
#include "skin/LedIndicator.h"
#include "skin/RigView.h"

namespace sawblade::plugin::rig {

// The blocks after the amp (not tiles).
int blocksAfterAmp(const PathPreset& p);
// The name on a tile: CHAINSAW / BIG FUZZ / MODDED SAW / ONE-KNOB SAW (the circuit's generic descriptor), GREEN OVERDRIVE (pedal.ts),
// the EQ's or the capture's title otherwise (upper case).
juce::String pedalName(const Block& b);

// A pedal tile. A modeled pedal shows its render; a CAPTURE (a nam block in a pedal slot, v0.4 Task B) is a flat panel with a 2 px cream
// outline, the CAPTURE badge, the capture's title, creator and licence tag, "CAPTURE · FIXED TONE", a LEVEL knob (the block's output
// level), a setting selector when the tone has several cached models, and the footswitch + LED: no other knob.
class BoardTile : public juce::Component, public juce::SettableTooltipClient, public HasPedalKind {
 public:
  enum class Gesture { Down, Drag, Up };

  // `pp.blocks[blockIndex]` is the tile's block; it is a capture iff isCapturePedal(pp, blockIndex) (the one rule of the rig model).
  BoardTile(int path, int blockIndex, const PathPreset& pp, RigController& controller);
  ~BoardTile() override;

  int path() const noexcept { return path_; }            // 0 = SAW (path A), 1 = BODY (path B)
  int blockIndex() const noexcept { return index_; }     // index in the path's blocks
  const std::string& blockId() const noexcept { return id_; }
  const juce::String& name() const noexcept { return name_; }
  PedalKind pedalKind() const noexcept override { return kind_; }
  bool isCapture() const noexcept { return kind_ == PedalKind::Capture; }
  bool isCircuit() const noexcept { return circuit_; }   // a circuit pedal (pedal.hm / .hmx / .eye / .muff): the live face may lie over it
  bool bypassed() const noexcept { return !fs_.getToggleState(); }
  bool selected() const noexcept { return selected_; }
  skin::FootswitchButton& footswitch() noexcept { return fs_; }
  skin::LedIndicator& led() noexcept { return led_; }

  // Shows the bypass state (no callback; repaints only on a change).
  void showBypass(bool bypass);
  // --- capture tiles ---
  const juce::String& captureName() const noexcept { return title_; }
  const juce::String& captureCreator() const noexcept { return creator_; }  // "@creator" / "LOCAL FILE"
  const juce::String& captureLicence() const noexcept { return licence_; }  // upper case, "" when unknown
  PresetKnob* levelKnob() noexcept { return level_.get(); }
  void showLevel(double db);  // the block's output level, as the preset holds it (a knob being turned ignores it)
  // The setting selector (shown only with >= 2 models): the models of the capture's tone (cached ones, plus the tone's online list when the
  // tool answered), in setting order, and the current one.
  void setSettings(std::vector<CachedModel> models, const std::string& currentModelId);
  // While a model that is not cached yet downloads (the selector's online choice): the selector says so and is disabled.
  void setFetching(bool f);
  bool fetching() const noexcept { return fetching_; }
  const std::string& toneId() const noexcept { return toneId_; }    // the capture's TONE3000 tone ("" for a local file)
  const std::string& modelId() const noexcept { return modelId_; }
  bool hasSelector() const noexcept { return settings_.size() >= 2; }
  juce::TextButton& selectorButton() noexcept { return selector_; }
  const std::vector<CachedModel>& settings() const noexcept { return settings_; }
  juce::PopupMenu settingsMenu() const;          // one item per setting, the current one ticked
  void chooseSetting(const std::string& modelId);  // what an item of the menu does
  std::function<void(BoardTile&, const std::string& modelId)> onSetting;
  void setSelected(bool s);
  // v0.8 I2: the capture's metadata has no input or output level, so the calibrated chain cannot plan it ("UNCAL" badge, capture tiles only).
  void setUncalibrated(bool u);
  bool uncalibrated() const noexcept { return uncalibrated_; }

  std::function<void(BoardTile&)> onSelect, onDoubleClick, onContextMenu;
  std::function<void(BoardTile&, bool bypass)> onBypass;  // the footswitch was clicked
  // The raw mouse on the tile's body (not on the footswitch): the Pedalboard turns it into a drag. The callee may destroy the tile.
  std::function<void(BoardTile&, Gesture, const juce::MouseEvent&)> onGesture;

  void paint(juce::Graphics&) override;
  void paintOverChildren(juce::Graphics&) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent&) override;
  void mouseUp(const juce::MouseEvent&) override;
  void mouseDoubleClick(const juce::MouseEvent&) override;

 private:
  void paintCapture(juce::Graphics& g);
  void layoutCapture();
  void updateSelectorText();
  bool fetching_ = false;
  int path_, index_;
  std::string id_;
  juce::String name_;
  PedalKind kind_;
  bool circuit_;
  skin::Panel panel_;
  float mmHeight_;
  juce::Point<float> switchMm_, ledMm_;
  bool selected_ = false;
  bool uncalibrated_ = false;
  skin::FootswitchButton fs_;
  skin::LedIndicator led_;
  juce::String title_, creator_, licence_;
  std::string toneId_, modelId_;
  std::vector<CachedModel> settings_;
  std::unique_ptr<PresetKnob> level_;
  juce::TextButton selector_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BoardTile)
};

class Pedalboard : public juce::Component {
 public:
  static constexpr int kMinTileW = 110, kMaxTileW = 180, kTileGap = 8, kBoardPad = 12, kScrollBar = 10;
  static constexpr int kDragThreshold = 6;  // px of movement on a tile's body before a drag starts
  static constexpr int kMenuBypass = 1, kMenuRemove = 2;

  explicit Pedalboard(RigController& c);
  ~Pedalboard() override;

  // The board of `path` (0 = SAW, 1 = BODY) in this component's (= the rig's) coordinates.
  static juce::Rectangle<int> boardBounds(int path) { return skin::RigLayout::board(path); }
  // Where tile `index` of `path` is (or would be, past the last tile and for an empty board), unscrolled.
  juce::Rectangle<int> slotBounds(int path, int index) const;
  // A tile's bounds in this component's coordinates (tiles live in a scrolling strip), and whether all of it is in view.
  juce::Rectangle<int> tileBounds(const BoardTile& t) const;
  bool tileFullyVisible(int path, int blockIndex);

  // Shows `shown` (the rig as edited: SawbladeProcessor::editBasePreset()). Rebuilds the tiles only when the block list changed.
  void refresh(const Preset& shown);
  std::function<void()> onTilesChanged;  // after a rebuild: the editor re-places the pedal face
  std::function<void()> onScrolled;      // a board was scrolled
  std::function<void(const juce::String&)> onMessage;  // a status-line message ("path full: 8 blocks")
  // "SEARCH TONE3000...": the editor opens the capture browser in insert mode for path `path` at `index` (the end of its board).
  std::function<void(int path, int index)> onSearchRequest;

  int tileCount(int path) const noexcept { return static_cast<int>(paths_[static_cast<std::size_t>(path)].tiles.size()); }
  BoardTile* tile(int path, int index);                  // index among the shown tiles
  BoardTile* tileForBlock(int path, int blockIndex);     // nullptr: that block is not a tile
  bool pathOff(int path) const noexcept { return paths_[static_cast<std::size_t>(path)].off; }
  bool pathFull(int path) const noexcept { return paths_[static_cast<std::size_t>(path)].blocks >= kMaxBlocksPerPath; }
  juce::String captionText(int path) const;              // "SAW · 2 PEDALS" / "BODY · OFF"
  juce::String offText(int path) const;                  // "BODY PATH OFF — turn up BLEND" while path `path` is off, else ""
  juce::String afterAmpText(int path) const;             // "+2 AFTER AMP (rig editor)" or ""
  juce::Button& addButton(int path);                     // the "+ PEDAL" slot (greyed while the path is full)
  juce::Viewport& viewport(int path);                    // the board's scroller

  // The pedal tile the inspector shows: `path` -1 = none (an amp head is selected); `blockId` "" = the first tile of the path.
  void setSelected(int path, const std::string& blockId);
  BoardTile* selectedTile();
  std::function<void(BoardTile&)> onSelect;         // a tile was clicked
  std::function<void(BoardTile&)> onTileDoubleClick;

  // --- editing: each is exactly one RigController::edit (one undo step); false = nothing was done ---
  // Reorders (same path) or moves (other path) the pedal `id` of `path` to final index `toIndex` among the target's pedals.
  bool movePedal(int path, const std::string& id, int toPath, int toIndex);
  bool removePedal(int path, const std::string& id);
  bool addModeledPedal(int path, const std::string& type);  // before the amp, at the end of the board
  // A capture pedal (a nam block, slot "pedal") before the amp, at the end of the board: one undo step. With LEVEL MATCH on, its make-up
  // (v0.3 slotMakeupDb: the path alone without / with the pedal) is measured in the background and lands when it is done, WITHOUT a step of
  // its own (it is patched into the history, so undo / redo carry it).
  bool addCapturePedal(int path, const Capture& capture);
  // Another model of a capture pedal's tone (the setting selector): the capture is swapped, make-up included, as one undo step.
  bool swapPedalCapture(int path, const std::string& id, const Capture& capture);
  bool setPedalBypass(int path, const std::string& id, bool bypass);
  // The right-click / ctrl-click menu (BYPASS ticked while bypassed, REMOVE) and what its items do.
  juce::PopupMenu menuFor(const BoardTile& t) const;
  void applyMenuChoice(int path, const std::string& id, int choice);

  // --- the picker ---
  void showPicker(int path);
  void closePicker();
  bool pickerOpen() const { return picker_ != nullptr && picker_->isVisible(); }
  PedalPicker* picker() { return picker_.get(); }

  // The `sawblade-t3k` the selector asks for a tone's online models and downloads a chosen one with (default: settings::t3kExecutable()).
  // The listing is skipped when network tools are disabled (SAWBLADE_NO_NETWORK) or the tool is not there: the selector then lists the cache.
  void setModelsExecutable(std::function<std::string()> exe);

  // Whether a mouse button is down (default: the real state). A drag that is still active while this says no lost its mouse-up and is
  // abandoned by the next refresh. Tests replace it (their mouse is synthesized).
  void setMouseDownProbe(std::function<bool()> probe);

  // --- a drag in progress (introspection for tests) ---
  struct DragInfo {
    bool pressed = false;   // the mouse is down on a tile (a drag may or may not have started)
    bool active = false;    // past the threshold: the ghost is out
    bool removing = false;  // over neither board: dropping removes it
    bool refused = false;   // over a full path: dropping is refused
    int targetPath = -1;    // the board that would take the drop, -1 none
    int insertIndex = -1;   // its final index there
    const BoardTile* tile = nullptr;
  };
  DragInfo dragInfo() const;

  void paint(juce::Graphics&) override;
  void paintOverChildren(juce::Graphics&) override;
  void resized() override;

 private:
  class Strip;
  class BoardViewport;
  struct Key {
    std::string id, type;
    juce::String name;
    std::string detail;  // a capture's model (a swap rebuilds the tile)
    bool operator==(const Key& o) const { return id == o.id && type == o.type && name == o.name && detail == o.detail; }
  };
  struct PathView {
    bool off = false;
    int afterAmp = 0;
    int blocks = 0;  // all blocks of the path, the amp included
    std::vector<Key> keys;
    std::unique_ptr<BoardViewport> viewport;
    std::unique_ptr<Strip> strip;
    std::vector<std::unique_ptr<BoardTile>> tiles;
    std::unique_ptr<juce::TextButton> add;
  };
  struct Drop {
    enum class Kind { Cancel, Remove, Insert, Refused } kind = Kind::Cancel;
    int path = -1, index = -1;
    // The target as the user sees it, by block id (the tiles may be stale by the time of the drop): the pedal it goes before / after
    // (among the board's other tiles; "" = none on that side).
    std::string beforeId, afterId;
    juce::Rectangle<float> bar;  // the insertion bar, in this component's coordinates
  };
  struct Drag {
    BoardTile* tile = nullptr;
    juce::Point<float> down, pos, grab;
    bool active = false;
    juce::uint32 downMs = 0;
    juce::Image ghost;
    Drop drop;
  };

  static std::vector<juce::Rectangle<int>> slotRects(int path, int tiles, bool withAdd);
  void rebuild(int path, const PathPreset& pp, std::vector<Key> keys, bool off);
  void layoutPath(int path);
  void applySelection();
  void refreshNow();  // re-reads the processor's edit base (after this component's own edits)
  void gesture(BoardTile& t, BoardTile::Gesture g, const juce::MouseEvent& e);
  void startDrag(BoardTile& t);
  void abortDrag();  // forgets a drag without dropping it (the tile is shown normally again)
  bool hasTile(int path, const std::string& id) const;
  void endDrag(bool drop);
  BoardTile* tileById(int path, const std::string& id);
  void applySettings(BoardTile& t);  // the selector's list: the cache merged with the tone's online models
  void lookupModels(const std::string& toneId);
  void chooseSetting(BoardTile& t, const std::string& modelId);
  // The one implementation of a move. `anchor` (a drop) resolves the target by block id against the CURRENT preset; null = `toIndex`.
  // `cancelled` is set when the target no longer exists (nothing is edited, nothing is recorded).
  bool moveImpl(int path, const std::string& id, int toPath, const Drop* anchor, int toIndex, bool* cancelled);
  Drop computeDrop(juce::Point<float> p) const;
  void showTileMenu(BoardTile& t);
  void say(const juce::String& m);

  RigController& controller_;
  std::array<PathView, 2> paths_;
  int selPath_ = -1;
  std::string selId_;
  Drag drag_;
  bool refreshPending_ = false;  // a refresh arrived while a tile was pressed: the mouse-up re-reads the edit base
  std::function<bool()> mouseDown_ = [] { return juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown(); };
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);  // checked by the level worker callbacks
  std::uint64_t swapSeq_ = 0;  // a newer capture swap supersedes the make-up still being measured for an older one
  // The online models of each capture tone seen this session: asked once per tone (Fetching -> Done / Failed, never retried; Failed = the
  // selector keeps listing the cache only).
  struct ToneModels {
    enum class State { Fetching, Done, Failed } state = State::Fetching;
    std::vector<CachedModel> online;
  };
  std::map<std::string, ToneModels> toneModels_;
  std::set<std::string> fetchingIds_;  // block ids whose chosen setting is downloading
  std::function<std::string()> modelsExe_;
  std::unique_ptr<T3kClient> t3k_;  // after alive_: its callbacks are dropped on destruction
  std::unique_ptr<PedalPicker> picker_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Pedalboard)
};

}  // namespace sawblade::plugin::rig
