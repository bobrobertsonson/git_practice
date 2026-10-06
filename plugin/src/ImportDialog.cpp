#include "ImportDialog.h"

#include "MatchGlue.h"
#include "SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

constexpr int kBoxW = 580;
}  // namespace

struct ImportDialog::Impl {
  Impl(ImportDialog& o, SawbladeProcessor& p, const juce::File& f, const di_import::Probe& pr) : owner(o), proc(p), file(f), probe(pr) {}

  ImportDialog& owner;
  SawbladeProcessor& proc;
  juce::File file;
  di_import::Probe probe;
  std::unique_ptr<di_import::ImportJob> job;
  juce::Rectangle<int> box;

  juce::Label title, fileName, info, capChannel, capStart, hint, note, error;
  juce::ToggleButton left, right, sum, dontKnow, same;
  juce::TextEditor timeField;
  juce::TextButton importBtn, cancelBtn;

  bool stereo() const { return probe.channels == 2; }

  void label(juce::Label& l, float size, juce::Colour c, bool bold = false, bool caption = false) {
    l.setFont(caption ? L::labelFont(size) : bold ? L::titleFont(size) : L::bodyFont(size));
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(juce::Justification::topLeft);
    l.setMinimumHorizontalScale(1.0f);
    l.setInterceptsMouseClicks(false, false);
    owner.addAndMakeVisible(l);
  }
  void toggle(juce::ToggleButton& b, const juce::String& text, const juce::String& title, const juce::String& tip) {
    b.setButtonText(text);
    b.setTitle(title);
    b.setTooltip(tip);
    b.setColour(juce::ToggleButton::textColourId, L::text());
    owner.addAndMakeVisible(b);
  }

  void build() {
    label(title, 20.0f, L::saw(), true);
    title.setText("IMPORT DI", juce::dontSendNotification);
    label(fileName, 14.0f, L::text(), true);
    fileName.setText(file.getFileName(), juce::dontSendNotification);
    label(info, 12.0f, L::dimText());
    info.setText(juce::String(stereo() ? "stereo" : "mono") + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(probe.sampleRate / 1000.0, 1) + " kHz" +
                     juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(probe.seconds(), 1) + " s. The file is copied into your takes; the original is not touched.",
                 juce::dontSendNotification);

    label(capChannel, 11.0f, L::dimText(), false, true);
    capChannel.setText("STEREO FILE: THE GUITAR IS THE", juce::dontSendNotification);
    toggle(left, "LEFT", "Left channel", "Use the left channel (the default: a DI is usually the left or the only channel).");
    toggle(right, "RIGHT", "Right channel", "Use the right channel.");
    toggle(sum, "SUM", "Sum of both channels", "Use the sum of both channels (left + right, halved).");
    for (auto* b : {&left, &right, &sum}) b->setRadioGroupId(4711);
    left.setToggleState(true, juce::dontSendNotification);
    capChannel.setVisible(stereo());
    for (auto* b : {&left, &right, &sum}) b->setVisible(stereo());

    toggle(same, "same performance as the song (my own recording)", "Same performance as the song",
           "Tick this only if this DI is the guitar the song was made from (a bounce of the recording). Then the matcher lines the two up in time. "
           "Leave it off for a DI you played along to the song.");
    same.setToggleState(false, juce::dontSendNotification);
    label(note, 11.0f, L::dimText());
    note.setText("Off: the take is matched on tone only, its timing against the song is not used.", juce::dontSendNotification);

    label(capStart, 11.0f, L::dimText(), false, true);
    capStart.setText("DI STARTS AT", juce::dontSendNotification);
    timeField.setText(di_import::formatSongTime(0.0), juce::dontSendNotification);
    timeField.setTitle("DI start time");
    timeField.setTooltip("Where this DI starts in the song, as m:ss.mmm. A DI bounced from the start of the song: leave 0:00.000.");
    timeField.setFont(L::monoFont(14.0f));
    timeField.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141210));
    timeField.setColour(juce::TextEditor::textColourId, L::text());
    timeField.setColour(juce::TextEditor::outlineColourId, L::chipBorder());
    timeField.setInputRestrictions(12, "0123456789:.");
    owner.addAndMakeVisible(timeField);
    label(hint, 11.5f, L::dimText());
    hint.setText("in the song (m:ss.mmm): a DI bounced from the start of the song: leave 0:00", juce::dontSendNotification);
    toggle(dontKnow, "don't know", "Don't know where it starts", "Pass no start time: the matcher searches the whole song for where this DI starts.");
    dontKnow.setToggleState(false, juce::dontSendNotification);

    label(error, 12.0f, L::error());

    importBtn.setButtonText("IMPORT");
    importBtn.setTitle("IMPORT");
    importBtn.setTooltip("Copy this file into your takes and use it for MATCH");
    importBtn.setColour(juce::TextButton::buttonColourId, L::saw());
    importBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff140a04));
    cancelBtn.setButtonText("CANCEL");
    cancelBtn.setTitle("CANCEL IMPORT");
    cancelBtn.setTooltip("Close without importing");
    owner.addAndMakeVisible(importBtn);
    owner.addAndMakeVisible(cancelBtn);

    same.onClick = [this] { updateEnabled(); };
    dontKnow.onClick = [this] { updateEnabled(); };
    importBtn.onClick = [this] { startImport(); };
    cancelBtn.onClick = [this] { owner.close(); };
    updateEnabled();
  }

  void updateEnabled() {
    const bool busy = job != nullptr;
    const bool pos = same.getToggleState() && !busy;
    timeField.setEnabled(pos && !dontKnow.getToggleState());
    timeField.setAlpha(timeField.isEnabled() ? 1.0f : 0.45f);
    dontKnow.setEnabled(pos);
    capStart.setAlpha(pos ? 1.0f : 0.45f);
    hint.setAlpha(pos ? 1.0f : 0.45f);
    for (auto* b : {&left, &right, &sum}) b->setEnabled(!busy);
    same.setEnabled(!busy);
    importBtn.setEnabled(!busy);
    cancelBtn.setEnabled(!busy);
    note.setText(same.getToggleState() ? "On: the take is lined up with the song in time. Give where it starts, or tick don't know."
                                        : "Off: the take is matched on tone only, its timing against the song is not used.",
                 juce::dontSendNotification);
  }

  void startImport() {
    if (job) return;
    di_import::Options opt;
    opt.channel = right.getToggleState() ? di_import::Channel::Right : sum.getToggleState() ? di_import::Channel::Sum : di_import::Channel::Left;
    opt.samePerformance = same.getToggleState();
    if (opt.samePerformance && !dontKnow.getToggleState()) {
      const auto ms = di_import::parseSongTime(timeField.getText().trim().toStdString());
      if (!ms) {
        error.setText("Enter the start time as m:ss.mmm, for example 1:23.500 (or tick don't know).", juce::dontSendNotification);
        return;
      }
      opt.offsetMs = *ms;
    }
    error.setText({}, juce::dontSendNotification);
    job = std::make_unique<di_import::ImportJob>(proc.recorder(), fs::path(file.getFullPathName().toStdString()), opt);
    updateEnabled();
    note.setText("Importing...", juce::dontSendNotification);
    owner.startTimerHz(30);
  }

  void layout() {
    const auto b = owner.getLocalBounds();
    const int boxH = stereo() ? 430 : 372;
    box = juce::Rectangle<int>(kBoxW, boxH).withCentre(b.getCentre());
    const int x = box.getX() + 24, w = box.getWidth() - 48;
    int y = box.getY() + 18;
    title.setBounds(x, y, w, 26);
    y += 30;
    fileName.setBounds(x, y, w, 20);
    y += 22;
    info.setBounds(x, y, w, 34);
    y += 44;
    if (stereo()) {
      capChannel.setBounds(x, y, w, 14);
      y += 18;
      left.setBounds(x, y, 90, 24);
      right.setBounds(x + 100, y, 100, 24);
      sum.setBounds(x + 210, y, 90, 24);
      y += 38;
    }
    same.setBounds(x, y, w, 24);
    y += 26;
    note.setBounds(x + 4, y, w - 4, 16);
    y += 28;
    capStart.setBounds(x, y, w, 14);
    y += 18;
    timeField.setBounds(x, y, 130, 28);
    dontKnow.setBounds(x + 146, y + 2, 130, 24);
    y += 34;
    hint.setBounds(x, y, w, 16);
    y += 26;
    error.setBounds(x, y, w, 34);
    importBtn.setBounds(box.getRight() - 24 - 120 - 12 - 120, box.getBottom() - 24 - 34, 120, 34);
    cancelBtn.setBounds(box.getRight() - 24 - 120, box.getBottom() - 24 - 34, 120, 34);
  }
};

ImportDialog::ImportDialog(SawbladeProcessor& p, const juce::File& file, const di_import::Probe& probe) : impl_(std::make_unique<Impl>(*this, p, file, probe)) {
  setTitle("Import DI");
  setWantsKeyboardFocus(true);
  impl_->build();
}

ImportDialog::~ImportDialog() { stopTimer(); }

void ImportDialog::show(juce::Component& parent, std::unique_ptr<ImportDialog>& slot, SawbladeProcessor& p, const juce::File& file, const di_import::Probe& probe,
                        std::function<void(const std::string&)> onImportedCb) {
  if (slot) {  // one at a time: the old one goes (it is not importing: its controls were modal)
    slot->setVisible(false);
    slot.reset();
  }
  slot = std::make_unique<ImportDialog>(p, file, probe);
  ImportDialog* raw = slot.get();
  raw->onImported = std::move(onImportedCb);
  parent.addAndMakeVisible(*raw);
  raw->setBounds(parent.getLocalBounds());
  raw->toFront(true);
  juce::Component::SafePointer<juce::Component> sp(&parent);
  std::unique_ptr<ImportDialog>* slotPtr = &slot;
  raw->onClose = [sp, slotPtr, raw] {
    raw->setVisible(false);
    juce::MessageManager::callAsync([sp, slotPtr, raw] {
      if (sp != nullptr && slotPtr->get() == raw) slotPtr->reset();
    });
  };
  if (raw->isShowing()) raw->grabKeyboardFocus();
}

void ImportDialog::close() {
  stopTimer();
  if (onClose) onClose();
  else setVisible(false);
}

bool ImportDialog::importing() const { return impl_->job != nullptr; }

bool ImportDialog::keyPressed(const juce::KeyPress& k) {
  if (k == juce::KeyPress::escapeKey && !importing()) {
    close();
    return true;
  }
  return true;  // modal: nothing falls through to the editor
}

void ImportDialog::timerCallback() {
  if (!impl_->job || !impl_->job->done()) return;
  const di_import::Outcome out = impl_->job->outcome();
  impl_->job.reset();  // joins (the worker has finished)
  stopTimer();
  if (out.ok) {
    if (onImported) onImported(out.takeName);
    close();
    return;
  }
  impl_->error.setText(juce::String(out.error), juce::dontSendNotification);
  impl_->updateEnabled();
}

void ImportDialog::parentSizeChanged() {
  if (auto* p = getParentComponent()) setBounds(p->getLocalBounds());
}

void ImportDialog::resized() { impl_->layout(); }

void ImportDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colours::black.withAlpha(0.62f));
  const auto r = impl_->box.toFloat();
  g.setColour(L::panel());
  g.fillRoundedRectangle(r, 8.0f);
  g.setColour(L::saw().withAlpha(0.8f));
  g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.5f);
}

// ---- DiImporter ----------------------------------------------------------------------------------------------------------
DiImporter::DiImporter(SawbladeProcessor& p, juce::Component& view) : proc_(p), view_(view) {}
DiImporter::~DiImporter() = default;

void DiImporter::choose() {
  song_input::launchChooserSpec(chooser_, chooserSpec(), [this](const juce::File& f) { handlePicked(f); });
}

bool DiImporter::handlePicked(const juce::File& f) {
  if (f == juce::File()) return false;  // cancelled
  const di_import::Probe pr = di_import::probe(fs::path(f.getFullPathName().toStdString()));
  if (!pr.ok()) {
    if (onRejected) onRejected(juce::String(pr.error));
    return false;
  }
  juce::Component& parent = view_.getParentComponent() != nullptr ? *view_.getParentComponent() : view_;
  SawbladeProcessor& proc = proc_;
  auto cb = onImported;
  ImportDialog::show(parent, dialog_, proc_, f, pr, [&proc, cb](const std::string& name) {
    chooseTakeForMatch(proc, name);  // the new take is the DI for MATCH
    if (cb) cb(name);
  });
  return true;
}

bool DiImporter::isImportableDrop(const juce::StringArray& files) {
  for (const auto& f : files)
    if (!juce::File(f).isDirectory() && di_import::isImportableName(f.toStdString())) return true;
  return false;
}

bool DiImporter::handleDrop(const juce::StringArray& files) {
  for (const auto& f : files)
    if (!juce::File(f).isDirectory() && di_import::isImportableName(f.toStdString())) return handlePicked(juce::File(f));
  return false;
}

}  // namespace sawblade::plugin
