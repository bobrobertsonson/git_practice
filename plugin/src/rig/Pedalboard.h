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
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "rig/PedalPicker.h"
#include "rig/RigController.h"
#include "skin/FootswitchButton.h"
#include "skin/LedIndicator.h"
#include "skin/RigView.h"

namespace sawblade::plugin::rig {

// The blocks of `p` that are tiles: those before its amp (all of them when it has none).
int boardBlockCount(const PathPreset& p);
// The blocks after the amp (not tiles).
int blocksAfterAmp(const PathPreset& p);
// The name on a tile: CHAINSAW / BIG FUZZ / MODDED SAW / ONE-KNOB SAW (the circuit's generic descriptor), GREEN OVERDRIVE (pedal.ts),
// the EQ's or the capture's title otherwise (upper case).
juce::String pedalName(const Block& b);

class BoardTile : public juce::Component, public juce::SettableTooltipClient {
 public:
  enum class Gesture { Down, Drag, Up };

  BoardTile(int path, int blockIndex, const Block& b);
  ~BoardTile() override;

  int path() const noexcept { return path_; }            // 0 = SAW (path A), 1 = BODY (path B)
  int blockIndex() const noexcept { return index_; }     // index in the path's blocks
  const std::string& blockId() const noexcept { return id_; }
  const juce::String& name() const noexcept { return name_; }
  bool isCircuit() const noexcept { return circuit_; }   // a circuit pedal (pedal.hm / .hmx / .eye / .muff): the live face may lie over it
  bool bypassed() const noexcept { return !fs_.getToggleState(); }
  bool selected() const noexcept { return selected_; }
  skin::FootswitchButton& footswitch() noexcept { return fs_; }
  skin::LedIndicator& led() noexcept { return led_; }

  // Shows the bypass state (no callback; repaints only on a change).
  void showBypass(bool bypass);
  void setSelected(bool s);

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
  int path_, index_;
  std::string id_;
  juce::String name_;
  bool circuit_;
  skin::Panel panel_;
  float mmHeight_;
  juce::Point<float> switchMm_, ledMm_;
  bool selected_ = false;
  skin::FootswitchButton fs_;
  skin::LedIndicator led_;

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
  bool setPedalBypass(int path, const std::string& id, bool bypass);
  // The right-click / ctrl-click menu (BYPASS ticked while bypassed, REMOVE) and what its items do.
  juce::PopupMenu menuFor(const BoardTile& t) const;
  void applyMenuChoice(int path, const std::string& id, int choice);

  // --- the picker ---
  void showPicker(int path);
  void closePicker();
  bool pickerOpen() const { return picker_ != nullptr && picker_->isVisible(); }
  PedalPicker* picker() { return picker_.get(); }

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
    bool operator==(const Key& o) const { return id == o.id && type == o.type && name == o.name; }
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
  void endDrag(bool drop);
  Drop computeDrop(juce::Point<float> p) const;
  void showTileMenu(BoardTile& t);
  void say(const juce::String& m);

  RigController& controller_;
  std::array<PathView, 2> paths_;
  int selPath_ = -1;
  std::string selId_;
  Drag drag_;
  bool refreshPending_ = false;  // a refresh arrived while a tile was pressed: the mouse-up re-reads the edit base
  std::unique_ptr<PedalPicker> picker_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Pedalboard)
};

}  // namespace sawblade::plugin::rig
