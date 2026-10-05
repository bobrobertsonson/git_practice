#include "PresetBrowser.h"

#include <algorithm>
#include <atomic>
#include <thread>

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

juce::String u(const std::string& s) { return juce::String(juce::CharPointer_UTF8(s.c_str())); }
std::string s8(const juce::String& s) { return s.toStdString(); }
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

const char* const kRecommended[] = {"Death metal", "Swedish death (HM-2)", "Black metal", "Thrash", "Doom / Sludge / Fuzz", "Hardcore / Crust",
                                    "Grind", "Metalcore / Djent", "Nu-metal", "Prog", "Other"};

// A simple scrolling single-selection list.
class RowList : public juce::Component {
 public:
  struct Row {
    juce::String text, sub;
    bool indent = false, dim = false;
  };
  explicit RowList(int rowH) : rowH_(rowH) { setWantsKeyboardFocus(false); }
  void setRows(std::vector<Row> r) {
    rows_ = std::move(r);
    scroll_ = std::clamp(scroll_, 0, maxScroll());
    repaint();
  }
  int numRows() const { return static_cast<int>(rows_.size()); }
  void setSelected(int i) {
    selected_ = i;
    repaint();
  }
  int selected() const { return selected_; }
  std::function<void(int)> onSelect, onActivate;

  void paint(juce::Graphics& g) override {
    g.setColour(juce::Colour(0xff0e0d0c));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.saveState();
    g.reduceClipRegion(getLocalBounds().reduced(1));
    for (int i = 0; i < numRows(); ++i) {
      const int y = i * rowH_ - scroll_;
      if (y + rowH_ < 0 || y > getHeight()) continue;
      const auto b = juce::Rectangle<int>(0, y, getWidth(), rowH_);
      if (i == selected_) {
        g.setColour(juce::Colour(0xff2a1a0e));
        g.fillRect(b);
        g.setColour(L::saw());
        g.fillRect(b.withWidth(3));
      }
      const auto& r = rows_[static_cast<std::size_t>(i)];
      const int x = r.indent ? 24 : 12;
      const auto text = r.dim ? L::dimText().withAlpha(0.6f) : L::text();
      if (r.sub.isEmpty()) {
        g.setColour(text);
        g.setFont(L::labelFont(13.0f));
        g.drawText(r.text, x, y, getWidth() - x - 8, rowH_, juce::Justification::centredLeft, true);
      } else {
        g.setColour(text);
        g.setFont(L::titleFont(14.0f));
        g.drawText(r.text, x, y + 3, getWidth() - x - 8, rowH_ / 2, juce::Justification::centredLeft, true);
        g.setColour(r.dim ? L::error().withAlpha(0.8f) : L::dimText());
        g.setFont(L::labelFont(10.0f));
        g.drawText(r.sub, x, y + rowH_ / 2, getWidth() - x - 8, rowH_ / 2 - 3, juce::Justification::centredLeft, true);
      }
    }
    g.restoreState();
  }
  void mouseDown(const juce::MouseEvent& e) override {
    const int i = (e.y + scroll_) / rowH_;
    if (i >= 0 && i < numRows()) {
      selected_ = i;
      repaint();
      if (onSelect) onSelect(i);
    }
  }
  void mouseDoubleClick(const juce::MouseEvent& e) override {
    const int i = (e.y + scroll_) / rowH_;
    if (i >= 0 && i < numRows() && onActivate) onActivate(i);
  }
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override {
    scroll_ = std::clamp(scroll_ - juce::roundToInt(w.deltaY * 120.0f), 0, maxScroll());
    repaint();
  }
  juce::Rectangle<int> rowBounds(int i) const { return {0, i * rowH_ - scroll_, getWidth(), rowH_}; }

 private:
  int maxScroll() const { return std::max(0, numRows() * rowH_ - getHeight()); }
  int rowH_;
  int scroll_ = 0, selected_ = -1;
  std::vector<Row> rows_;
};
}  // namespace

struct PresetBrowser::Impl {
  Impl(PresetBrowser& b, SawbladeProcessor& p, LibraryConfig cfg)
      : o(b), proc(p), lib(std::move(cfg)), banks(30), cats(28), list(46),
        flow(p, {[this](int d, int t, const std::string& title) { onProgress(d, t, title); }, [this](const PresetLoadFlow::Outcome& out) { onFinished(out); }}) {}

  PresetBrowser& o;
  SawbladeProcessor& proc;
  PresetLibrary lib;
  RowList banks, cats, list;
  PresetInfoPanel info;
  juce::TextEditor search;
  juce::TextButton back, loadFile, save, saveAsBtn, rename, del, cancel, locate;
  juce::Label title, status;
  juce::Label bankHeader, catHeader;
  double progress = -1.0;
  std::unique_ptr<juce::ProgressBar> bar;
  PresetLoadFlow flow;
  PresetLibrary::TrashFn trash;

  int bankRow = 0;
  juce::String category;  // "" = all
  std::vector<int> visible;
  std::vector<juce::String> categoryNames;
  int selectedIdx = -1;
  fs::path currentFile;
  SubBank currentBank = SubBank::Classic;
  bool needLocate = false;
  juce::String progressText;
  std::atomic<bool> scanning{false};
  std::thread scanThread;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  std::unique_ptr<juce::AlertWindow> dialog;

  void init() {
    auto style = [](juce::TextButton& b, const juce::String& t, const juce::String& tip) {
      b.setButtonText(t);
      b.setTitle(t);
      b.setTooltip(tip);
    };
    style(back, juce::String::fromUTF8("\xe2\x80\xb9 BACK"), "Close the preset browser");
    style(loadFile, juce::String::fromUTF8("LOAD FILE\xe2\x80\xa6"), "Load a preset file from disk");
    style(save, "SAVE", "Save over the current user preset (a factory preset: Save As)");
    style(saveAsBtn, "SAVE AS", "Save the current sound as a new user preset");
    style(rename, "RENAME", "Rename the selected user preset");
    style(del, "DELETE", "Move the selected user preset to the trash");
    style(cancel, "CANCEL", "Stop resolving the TONE3000 captures");
    style(locate, juce::String::fromUTF8("LOCATE\xe2\x80\xa6"), "Find the sawblade-t3k executable and remember it");
    back.onClick = [this] {
      if (o.onClose) o.onClose();
    };
    loadFile.onClick = [this] {
      if (o.onLoadFile) o.onLoadFile();
    };
    save.onClick = [this] { o.saveCurrent(); };
    saveAsBtn.onClick = [this] { askSaveAs(); };
    rename.onClick = [this] { askRename(); };
    del.onClick = [this] { askDelete(); };
    cancel.onClick = [this] { flow.cancel(); };
    locate.onClick = [this] { chooseExecutable(); };
    title.setText("PRESETS", juce::dontSendNotification);
    title.setFont(L::wordmarkFont().withHeight(22.0f));
    title.setColour(juce::Label::textColourId, L::saw());
    status.setFont(L::bodyFont(12.0f));
    bankHeader.setText("BANKS", juce::dontSendNotification);
    catHeader.setText("CATEGORIES", juce::dontSendNotification);
    for (auto* l : {&bankHeader, &catHeader}) {
      l->setFont(L::labelFont(11.0f));
      l->setColour(juce::Label::textColourId, L::dimText());
    }
    for (juce::Label* l : {&title, &status, &bankHeader, &catHeader}) {
      l->setInterceptsMouseClicks(false, false);
      o.addAndMakeVisible(*l);
    }
    search.setTextToShowWhenEmpty("Search name, category, notes, creator...", L::dimText());
    search.setTitle("Search presets");
    search.setTooltip("Search presets: every word must match the name, category, notes or a capture's title / creator");
    search.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0e0d0c));
    search.setColour(juce::TextEditor::textColourId, L::text());
    search.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
    search.onTextChange = [this] {
      refilter();
    };
    bar = std::make_unique<juce::ProgressBar>(progress);
    bar->setPercentageDisplay(false);
    bar->setColour(juce::ProgressBar::foregroundColourId, L::saw());
    bar->setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff0e0d0c));
    for (juce::Component* c : std::initializer_list<juce::Component*>{&back, &loadFile, &save, &saveAsBtn, &rename, &del, &search, &banks, &cats, &list, &info})
      o.addAndMakeVisible(*c);
    o.addChildComponent(*bar);
    o.addChildComponent(cancel);
    o.addChildComponent(locate);
    banks.onSelect = [this](int r) {
      bankRow = r;
      category = {};
      refilter();
    };
    cats.onSelect = [this](int r) {
      category = r == 0 ? juce::String() : categoryNames[static_cast<std::size_t>(r - 1)];
      refilter(/*keepCategory=*/true);
    };
    list.onSelect = [this](int r) { select(r < static_cast<int>(visible.size()) ? visible[static_cast<std::size_t>(r)] : -1); };
    list.onActivate = [this](int r) {
      if (r < static_cast<int>(visible.size())) o.loadEntry(visible[static_cast<std::size_t>(r)]);
    };
    refilter();
  }

  ~Impl() {
    *alive = false;
    if (scanThread.joinable()) scanThread.join();
  }

  LibraryFilter filter() const {
    LibraryFilter f;
    switch (bankRow) {
      case 0: break;
      case 1: f.anyBank = false; f.factoryOnly = true; break;
      case 2: f.anyBank = false; f.bank = SubBank::Classic; break;
      case 3: f.anyBank = false; f.bank = SubBank::Styles; break;
      case 4: f.anyBank = false; f.bank = SubBank::Matched; break;
      default: f.anyBank = false; f.bank = SubBank::User; break;
    }
    f.category = s8(category);
    f.search = s8(search.getText());
    return f;
  }

  void refilter(bool keepCategory = false) {
    LibraryFilter f = filter();
    // the bank counts ignore the bank filter, so they stay stable while the user clicks around
    auto count = [&](bool any, bool factory, SubBank b) {
      LibraryFilter g = f;
      g.category.clear();
      g.anyBank = any;
      g.factoryOnly = factory;
      g.bank = b;
      return lib.count(g);
    };
    std::vector<RowList::Row> br = {{"All", juce::String(count(true, false, SubBank::User)), false, false},
                                    {"Factory", juce::String(count(false, true, SubBank::User)), false, false},
                                    {"Classic", juce::String(count(false, false, SubBank::Classic)), true, false},
                                    {"Styles", juce::String(count(false, false, SubBank::Styles)), true, false},
                                    {"Matched", juce::String(count(false, false, SubBank::Matched)), true, false},
                                    {"User", juce::String(count(false, false, SubBank::User)), false, false}};
    for (auto& r : br) {
      r.text += "  (" + r.sub + ")";
      r.sub = {};
    }
    banks.setRows(std::move(br));
    banks.setSelected(bankRow);

    const auto cs = lib.categories(f);
    categoryNames.clear();
    std::vector<RowList::Row> cr;
    int total = 0;
    for (const auto& [name, n] : cs) {
      total += n;
      categoryNames.push_back(u(name));
      cr.push_back({u(name) + "  (" + juce::String(n) + ")", {}, false, false});
    }
    cr.insert(cr.begin(), {"All categories  (" + juce::String(total) + ")", {}, false, false});
    cats.setRows(std::move(cr));
    int sel = 0;
    if (!category.isEmpty()) {
      const auto it = std::find(categoryNames.begin(), categoryNames.end(), category);
      if (it != categoryNames.end()) sel = static_cast<int>(it - categoryNames.begin()) + 1;
      else if (!keepCategory) category = {};
      if (it == categoryNames.end()) category = {};
    }
    cats.setSelected(sel);

    f = filter();
    visible = lib.filtered(f);
    std::vector<RowList::Row> lr;
    int selRow = -1;
    for (int k = 0; k < static_cast<int>(visible.size()); ++k) {
      const auto& e = lib.entries()[static_cast<std::size_t>(visible[static_cast<std::size_t>(k)])];
      RowList::Row r;
      r.text = u(e.name);
      r.sub = e.loadable() ? u(e.displayCategory()) + kDot + u(subBankName(e.bank)) : "cannot be loaded: " + u(e.error).upToFirstOccurrenceOf("\n", false, false);
      r.dim = !e.loadable();
      lr.push_back(r);
      if (visible[static_cast<std::size_t>(k)] == selectedIdx) selRow = k;
    }
    list.setRows(std::move(lr));
    list.setSelected(selRow);
    if (selRow < 0) select(-1);
    updateButtons();
  }

  void select(int libIdx) {
    selectedIdx = libIdx;
    info.setEntry(libIdx >= 0 ? &lib.entries()[static_cast<std::size_t>(libIdx)] : nullptr);
    updateButtons();
  }

  bool selectedIsUser() const {
    return selectedIdx >= 0 && selectedIdx < static_cast<int>(lib.entries().size()) && lib.entries()[static_cast<std::size_t>(selectedIdx)].bank == SubBank::User;
  }
  void updateButtons() {
    const bool user = selectedIsUser();
    rename.setEnabled(user);
    del.setEnabled(user);
    const bool busy = flow.resolving();
    cancel.setVisible(busy);
    bar->setVisible(busy);
    locate.setVisible(needLocate && !busy);
  }

  void setMessage(const juce::String& m, bool error) {
    status.setColour(juce::Label::textColourId, error ? L::error() : L::warning());
    status.setText(m, juce::dontSendNotification);
  }

  void onProgress(int done, int total, const std::string& titleText) {
    progress = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
    setMessage("Resolving " + juce::String(done) + "/" + juce::String(total) + ": " + u(titleText), false);
  }

  void onFinished(const PresetLoadFlow::Outcome& out) {
    using S = PresetLoadFlow::Outcome::Status;
    needLocate = out.status == S::MissingExecutable;
    switch (out.status) {
      case S::Loaded:
        currentFile = out.file;
        {
          const int i = lib.indexOfFile(out.file);
          currentBank = i >= 0 ? lib.entries()[static_cast<std::size_t>(i)].bank : SubBank::User;
        }
        setMessage({}, false);
        break;
      case S::Cancelled: setMessage("Resolving cancelled; the sound is unchanged.", false); break;
      default: setMessage(u(out.message), true); break;
    }
    updateButtons();
    o.repaint();
  }

  void chooseExecutable() {
    dialogChooser = std::make_unique<juce::FileChooser>("Locate the sawblade-t3k executable", juce::File(settings::t3kExecutable().string()), "*");
    dialogChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      std::string err;
      if (!settings::setT3kExecutable(fs::path(f.getFullPathName().toStdString()), &err)) {
        setMessage(u(err), true);
        return;
      }
      needLocate = false;
      setMessage("Executable saved. Load the preset again.", false);
      updateButtons();
    });
  }
  std::unique_ptr<juce::FileChooser> dialogChooser;

  // --- dialogs (the operations themselves are PresetBrowser members, so tests skip the dialogs)
  void askSaveAs() {
    dialog = std::make_unique<juce::AlertWindow>("Save preset as", "Name and category of the new user preset", juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", u(proc.status().presetName), "Name");
    juce::StringArray cs;
    for (const char* c : kRecommended) cs.add(c);
    dialog->addComboBox("category", cs, "Category");
    dialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const auto alive_ = alive;
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive_](int r) {
      if (!*alive_ || r != 1 || !dialog) return;
      const juce::String name = dialog->getTextEditorContents("name").trim();
      const juce::String cat = dialog->getComboBoxComponent("category")->getText();
      dialog.reset();
      if (name.isNotEmpty() && !o.saveAs(name, cat, false) && lib.userPathFor(s8(name)) != fs::path() && fs::exists(lib.userPathFor(s8(name)))) {
        confirm("Overwrite?", "A user preset with this file name exists. Overwrite it?", [this, name, cat] { o.saveAs(name, cat, true); });
      }
    }), false);
  }
  void askRename() {
    if (!selectedIsUser()) return;
    dialog = std::make_unique<juce::AlertWindow>("Rename preset", "New name", juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", u(lib.entries()[static_cast<std::size_t>(selectedIdx)].name), "Name");
    dialog->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const auto alive_ = alive;
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive_](int r) {
      if (!*alive_ || r != 1 || !dialog) return;
      const juce::String name = dialog->getTextEditorContents("name").trim();
      dialog.reset();
      if (name.isNotEmpty()) o.renameSelected(name);
    }), false);
  }
  void askDelete() {
    if (!selectedIsUser()) return;
    confirm("Delete preset", "Move \"" + u(lib.entries()[static_cast<std::size_t>(selectedIdx)].name) + "\" to the trash?", [this] { o.deleteSelected(); });
  }
  void confirm(const juce::String& title_, const juce::String& text, std::function<void()> yes) {
    dialog = std::make_unique<juce::AlertWindow>(title_, text, juce::MessageBoxIconType::QuestionIcon);
    dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const auto alive_ = alive;
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive_, yes](int r) {
      if (!*alive_) return;
      dialog.reset();
      if (r == 1) yes();
    }), false);
  }
};

PresetBrowser::PresetBrowser(SawbladeProcessor& p, LibraryConfig cfg) {
  setTitle("Preset browser");
  impl_ = std::make_unique<Impl>(*this, p, std::move(cfg));
  impl_->init();
  setSize(kWidth, kHeight);
  resized();
}

PresetBrowser::~PresetBrowser() = default;

void PresetBrowser::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, kWidth, 46);
  g.fillRect(0, kHeight - 62, kWidth, 62);
  g.setColour(L::rule());
  g.drawHorizontalLine(45, 0.0f, static_cast<float>(kWidth));
  g.drawHorizontalLine(kHeight - 62, 0.0f, static_cast<float>(kWidth));
}

void PresetBrowser::resized() {
  Impl& d = *impl_;
  d.back.setBounds(18, 7, 90, 32);
  d.title.setBounds(124, 6, 200, 34);
  d.loadFile.setBounds(kWidth - 18 - 130, 7, 130, 32);
  d.search.setBounds(16, 58, 236, 30);
  d.bankHeader.setBounds(16, 98, 200, 14);
  d.banks.setBounds(16, 114, 236, 6 * 30);
  d.catHeader.setBounds(16, 114 + 6 * 30 + 12, 200, 14);
  d.cats.setBounds(16, 114 + 6 * 30 + 28, 236, kHeight - 62 - 12 - (114 + 6 * 30 + 28));
  d.list.setBounds(264, 58, 470, kHeight - 62 - 12 - 58);
  d.info.setBounds(746, 58, 518, kHeight - 62 - 12 - 58);
  d.save.setBounds(16, kHeight - 62 + 15, 80, 32);
  d.saveAsBtn.setBounds(104, kHeight - 62 + 15, 96, 32);
  d.rename.setBounds(208, kHeight - 62 + 15, 96, 32);
  d.del.setBounds(312, kHeight - 62 + 15, 90, 32);
  d.status.setBounds(420, kHeight - 62 + 8, 700, 22);
  d.bar->setBounds(420, kHeight - 62 + 36, 520, 14);
  d.cancel.setBounds(952, kHeight - 62 + 28, 90, 26);
  d.locate.setBounds(1052, kHeight - 62 + 28, 110, 26);
}

void PresetBrowser::scanBlocking() {
  impl_->lib.rescan();
  impl_->refilter(true);
}

bool PresetBrowser::scanning() const { return impl_->scanning.load(); }

void PresetBrowser::open() {
  Impl& d = *impl_;
  if (d.scanning.exchange(true)) return;
  if (d.scanThread.joinable()) d.scanThread.join();
  const LibraryConfig cfg = d.lib.config();
  const auto alive = d.alive;
  juce::Component::SafePointer<PresetBrowser> self(this);
  d.scanThread = std::thread([&d, cfg, alive, self] {
    auto entries = std::make_shared<std::vector<PresetEntry>>(scanLibrary(cfg));
    juce::MessageManager::callAsync([&d, alive, self, entries] {
      if (!*alive || self == nullptr) return;
      d.lib.setEntries(std::move(*entries));
      d.scanning = false;
      d.refilter(true);
    });
  });
}

PresetLibrary& PresetBrowser::library() { return impl_->lib; }
PresetInfoPanel& PresetBrowser::infoPanel() { return impl_->info; }

void PresetBrowser::setSearch(const juce::String& s) {
  impl_->search.setText(s, true);
  impl_->refilter(true);  // TextEditor reports the change asynchronously
}
void PresetBrowser::selectBankRow(int row) {
  impl_->bankRow = std::clamp(row, 0, 5);
  impl_->category = {};
  impl_->refilter();
}
void PresetBrowser::selectCategory(const juce::String& n) {
  impl_->category = n;
  impl_->refilter(true);
}
const std::vector<int>& PresetBrowser::visible() const { return impl_->visible; }
void PresetBrowser::selectEntry(int i) {
  impl_->select(i);
  const auto& v = impl_->visible;
  const auto it = std::find(v.begin(), v.end(), i);
  impl_->list.setSelected(it == v.end() ? -1 : static_cast<int>(it - v.begin()));
}
int PresetBrowser::selectedEntry() const { return impl_->selectedIdx; }

juce::StringArray PresetBrowser::categoryRows() const {
  juce::StringArray r;
  // rebuilt from the library with the same rule as the list
  const auto cs = impl_->lib.categories(impl_->filter());
  int total = 0;
  for (const auto& c : cs) total += c.second;
  r.add("All categories  (" + juce::String(total) + ")");
  for (const auto& [name, n] : cs) r.add(u(name) + "  (" + juce::String(n) + ")");
  return r;
}

bool PresetBrowser::loadEntry(int i) {
  Impl& d = *impl_;
  if (i < 0 || i >= static_cast<int>(d.lib.entries().size())) return false;
  const PresetEntry& e = d.lib.entries()[static_cast<std::size_t>(i)];
  if (!e.loadable()) {
    d.setMessage(u(e.error), true);
    return false;
  }
  d.setMessage({}, false);
  d.progress = -1.0;
  d.needLocate = false;
  const bool ok = d.flow.load(e.file, e.bank);
  d.updateButtons();
  return ok;
}

bool PresetBrowser::step(int dir) {
  Impl& d = *impl_;
  const auto& v = d.visible;
  if (v.empty() || d.flow.resolving()) return false;
  const int n = static_cast<int>(v.size());
  int pos = -1;
  const int cur = d.lib.indexOfFile(d.currentFile);
  for (int k = 0; k < n; ++k)
    if (v[static_cast<std::size_t>(k)] == cur) pos = k;
  for (int tries = 0; tries < n; ++tries) {
    pos = pos < 0 ? (dir > 0 ? 0 : n - 1) : ((pos + dir) % n + n) % n;
    const int idx = v[static_cast<std::size_t>(pos)];
    if (d.lib.entries()[static_cast<std::size_t>(idx)].loadable()) {
      selectEntry(idx);
      return loadEntry(idx);
    }
  }
  return false;
}

bool PresetBrowser::resolving() const { return impl_->flow.resolving(); }
juce::String PresetBrowser::message() const { return impl_->status.getText(); }
juce::String PresetBrowser::currentFileName() const { return u(impl_->currentFile.filename().string()); }

bool PresetBrowser::saveAs(const juce::String& name, const juce::String& category, bool overwrite) {
  Impl& d = *impl_;
  std::string err;
  fs::path written;
  if (!d.lib.saveAs(d.proc.currentPreset(), s8(name), s8(category), overwrite, &written, &err)) {
    d.setMessage(u(err), true);
    return false;
  }
  d.currentFile = written;
  d.currentBank = SubBank::User;
  d.refilter(true);
  d.setMessage("Saved " + u(written.filename().string()), false);
  const int i = d.lib.indexOfFile(written);
  if (i >= 0) selectEntry(i);
  return true;
}

bool PresetBrowser::saveCurrent() {
  Impl& d = *impl_;
  if (d.currentBank == SubBank::User && !d.currentFile.empty() && d.lib.indexOfFile(d.currentFile) >= 0) {
    std::string err;
    if (!d.lib.save(d.proc.currentPreset(), d.currentFile, &err)) {
      d.setMessage(u(err), true);
      return false;
    }
    d.refilter(true);
    d.setMessage("Saved " + u(d.currentFile.filename().string()), false);
    return true;
  }
  // not a user preset (factory, or a file loaded from elsewhere): Save As with the preset's own name and category
  const Preset p = d.proc.currentPreset();
  const juce::String name = u(p.name.empty() ? "Untitled" : p.name), cat = u(p.category);
  if (saveAs(name, cat, /*overwrite=*/false)) return true;
  std::error_code ec;
  if (fs::exists(d.lib.userPathFor(s8(name)), ec))
    d.confirm("Overwrite?", "A user preset with this file name exists. Overwrite it?", [this, name, cat] { saveAs(name, cat, true); });
  return false;
}

bool PresetBrowser::renameSelected(const juce::String& newName) {
  Impl& d = *impl_;
  const int i = d.selectedIdx;
  const fs::path oldFile = i >= 0 ? d.lib.entries()[static_cast<std::size_t>(i)].file : fs::path();
  std::string err;
  if (!d.lib.rename(i, s8(newName), &err)) {
    d.setMessage(u(err), true);
    return false;
  }
  if (d.currentFile == oldFile) d.currentFile = d.lib.userPathFor(s8(newName));
  d.refilter(true);
  const int j = d.lib.indexOfFile(d.lib.userPathFor(s8(newName)));
  if (j >= 0) selectEntry(j);
  d.setMessage("Renamed to " + newName, false);
  return true;
}

bool PresetBrowser::deleteSelected() {
  Impl& d = *impl_;
  const int i = d.selectedIdx;
  const fs::path f = i >= 0 ? d.lib.entries()[static_cast<std::size_t>(i)].file : fs::path();
  std::string err;
  if (!d.lib.remove(i, d.trash, &err)) {
    d.setMessage(u(err), true);
    return false;
  }
  if (d.currentFile == f) d.currentFile.clear();
  d.select(-1);
  d.refilter(true);
  d.setMessage("Moved to the trash: " + u(f.filename().string()), false);
  return true;
}

void PresetBrowser::setTrashFunction(PresetLibrary::TrashFn fn) { impl_->trash = std::move(fn); }

juce::Button* PresetBrowser::buttonTitled(const juce::String& t) {
  for (auto* c : getChildren())
    if (auto* b = dynamic_cast<juce::Button*>(c))
      if (b->getTitle() == t) return b;
  return nullptr;
}

}  // namespace sawblade::plugin
