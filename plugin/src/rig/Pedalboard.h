#pragma once

// The pedalboards of the main page (v0.4 Task D; Task C makes them draggable). One component spans the rig area (940 x 742 design px,
// rig coordinates) and owns both boards, SAW (path A, left column) and BODY (path B, right column), so that a drag between the paths
// can be handled by one component. The editor lays it over the RigView (the room, the amp heads, the cables, the cab chip).
//
// A path's board shows its blocks BEFORE its amp (all blocks when the path has no amp), in path order, as tiles; blocks after the amp
// are reachable from the rig editor only and are counted in a "+n AFTER AMP (rig editor)" note. Path B off: the board is empty, dimmed
// and says "BODY PATH OFF - turn up BLEND"; SAW's geometry never depends on B. A tile is a pedal render with a name chip over its baked
// caption, a footswitch + LED (bypass: one RigController::edit = one undo step) and a selection ring. The live PedalFace is laid over the
// tile of the first circuit block by the editor (tileForBlock()); the board has no knobs of its own.
//
// Refresh: the board rebuilds its tiles only when the shown block list changed (ids, types, names, path B on/off), never otherwise, so a
// refresh cannot destroy a widget under the user's hand.

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
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

  // Shows the bypass state (no callback).
  void showBypass(bool bypass);
  void setSelected(bool s);

  std::function<void(BoardTile&)> onSelect, onDoubleClick;
  std::function<void(BoardTile&, bool bypass)> onBypass;  // the footswitch was clicked

  void paint(juce::Graphics&) override;
  void paintOverChildren(juce::Graphics&) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent&) override;
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
  static constexpr int kMinTileW = 40, kMaxTileW = 180, kTileGap = 8, kBoardPad = 12;

  explicit Pedalboard(RigController& c);
  ~Pedalboard() override;

  // The board of `path` (0 = SAW, 1 = BODY) in this component's (= the rig's) coordinates.
  static juce::Rectangle<int> boardBounds(int path) { return skin::RigLayout::board(path); }
  // Where tile `index` of `path` is (or would be, past the last tile and for an empty board).
  juce::Rectangle<int> slotBounds(int path, int index) const;

  // Shows `shown` (the rig as edited: SawbladeProcessor::editBasePreset()). Rebuilds the tiles only when the block list changed.
  void refresh(const Preset& shown);
  std::function<void()> onTilesChanged;  // after a rebuild: the editor re-places the pedal face

  int tileCount(int path) const noexcept { return static_cast<int>(paths_[static_cast<std::size_t>(path)].tiles.size()); }
  BoardTile* tile(int path, int index);                  // index among the shown tiles
  BoardTile* tileForBlock(int path, int blockIndex);     // nullptr: that block is not a tile
  bool pathOff(int path) const noexcept { return paths_[static_cast<std::size_t>(path)].off; }
  juce::String captionText(int path) const;              // "SAW · 2 PEDALS" / "BODY · OFF"
  juce::String offText(int path) const;                  // "BODY PATH OFF — turn up BLEND" while path `path` is off, else ""
  juce::String afterAmpText(int path) const;             // "+2 AFTER AMP (rig editor)" or ""
  juce::Button& addButton(int path);                     // the "+ PEDAL" slot (disabled until Task C)

  // The pedal tile the inspector shows: `path` -1 = none (an amp head is selected); `blockId` "" = the first tile of the path.
  void setSelected(int path, const std::string& blockId);
  BoardTile* selectedTile();
  std::function<void(BoardTile&)> onSelect;         // a tile was clicked
  std::function<void(BoardTile&)> onTileDoubleClick;

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  struct Key {
    std::string id, type;
    juce::String name;
    bool operator==(const Key& o) const { return id == o.id && type == o.type && name == o.name; }
  };
  struct PathView {
    bool off = false;
    int afterAmp = 0;
    std::vector<Key> keys;
    std::vector<std::unique_ptr<BoardTile>> tiles;
    std::unique_ptr<juce::TextButton> add;
  };

  static std::vector<juce::Rectangle<int>> slotRects(int path, int tiles, bool withAdd);
  void rebuild(int path, const PathPreset& pp, std::vector<Key> keys, bool off);
  void layoutPath(int path);
  void applySelection();
  void editBypass(int path, const std::string& id, bool bypass);

  RigController& controller_;
  std::array<PathView, 2> paths_;
  int selPath_ = -1;
  std::string selId_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Pedalboard)
};

}  // namespace sawblade::plugin::rig
