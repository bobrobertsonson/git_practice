#include "MicPage.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "../SawbladeLookAndFeel.h"
#include "../presets/T3kTool.h"
#include "BinaryData.h"
#include "IrResponse.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
namespace fs = std::filesystem;

const juce::Colour kGreen(0xff7fd13b);
const juce::Colour kIrText(0xffb8f08a);
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

// Layout in page coordinates (1280 x 742).
constexpr int kHeaderH = 46;
const juce::Rectangle<int> kStage{16, 62, 664, 664};
constexpr int kRightX = 698, kRightW = 566;
constexpr int kFieldW = 279, kFieldH = 56;
constexpr float kDotRadius = 10.0f, kMicHit = 24.0f;

juce::Rectangle<int> fieldCell(int i) { return {kRightX + (i % 2) * (kFieldW + 8), 160 + (i / 2) * (kFieldH + 8), kFieldW, kFieldH}; }

juce::String distanceText(double d) {
  if (std::isnan(d)) return juce::String::fromUTF8("\xe2\x80\x94");
  juce::String s(d, 2);
  while (s.endsWith("0") && !s.endsWith(".0")) s = s.dropLastCharacters(1);
  return s + " in";
}

juce::String slotName(int slot, int nDrivers) {
  if (slot <= 0) return "SLOT UNKNOWN";
  const int i = (slot - 1) % std::max(nDrivers, 1);
  if (nDrivers == 4) {
    static const char* n[] = {"UPPER-LEFT", "UPPER-RIGHT", "LOWER-LEFT", "LOWER-RIGHT"};
    return n[i];
  }
  if (nDrivers == 2) return i == 0 ? "LEFT" : "RIGHT";
  return "SPEAKER " + juce::String(i + 1);
}

juce::String upper(const std::string& s) { return juce::String(juce::CharPointer_UTF8(s.c_str())).toUpperCase(); }

struct Embedded {
  juce::Image image;
  mic::CabLayout layout;
};

Embedded loadEmbedded(const char* png, int pngSize, const char* json, int jsonSize) {
  Embedded e;
  e.image = juce::ImageFileFormat::loadFrom(png, static_cast<size_t>(pngSize));
  const bool ok = mic::parseCabLayout(std::string_view(json, static_cast<size_t>(jsonSize)), e.layout);
  jassert(e.image.isValid() && ok);
  if (!ok) e.layout = {};
  return e;
}

void styleButton(juce::TextButton& b, const juce::String& text, const juce::String& tip) {
  b.setButtonText(text);
  b.setTitle(text);
  b.setTooltip(tip);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct MicPage::Impl {
  // The cab image with its dots and mics.
  class Stage : public juce::Component {
   public:
    explicit Stage(Impl& i) : impl(i) { setTitle("Cab mic placement"); }

    juce::Rectangle<float> imageRect() const {
      const auto& lay = impl.layout();
      const float w = static_cast<float>(getWidth());
      if (!lay.valid()) return {0, 0, w, w};
      const float h = w * static_cast<float>(lay.height / lay.width);
      return {0.0f, (static_cast<float>(getHeight()) - h) * 0.5f, w, h};
    }
    juce::Point<float> toScreen(mic::Point p) const {
      const auto r = imageRect();
      return {r.getX() + static_cast<float>(p.x) * r.getWidth(), r.getY() + static_cast<float>(p.y) * r.getWidth()};
    }
    mic::Point toNorm(juce::Point<float> s) const {
      const auto r = imageRect();
      return {static_cast<double>((s.x - r.getX()) / r.getWidth()), static_cast<double>((s.y - r.getY()) / r.getWidth())};
    }
    bool micPlaced(int m) const { return impl.o.session_.model(m) >= 0 && (m == 0 || impl.o.session_.blend()); }
    // Where mic m is drawn (stage coordinates): the snapped dot, or the cursor while it is dragged. When both mics sit on
    // the same dot they are drawn side by side so each one can be grabbed.
    juce::Point<float> micPoint(int m) const {
      if (dragMic == m) return dragPos;
      if (!micPlaced(m)) return {};
      const auto& session = impl.o.session_;
      juce::Point<float> p = toScreen(session.pack().modelPoint(session.model(m)));
      if (session.blend() && session.model(0) >= 0 && session.model(1) >= 0 &&
          session.pack().dotOfModel(session.model(0)) == session.pack().dotOfModel(session.model(1)))
        p.x += m == 0 ? -15.0f : 15.0f;
      return p;
    }

    void paint(juce::Graphics& g) override {
      juce::Path clip;
      clip.addRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
      g.setColour(juce::Colour(0xff0e0d0c));
      g.fillPath(clip);
      g.saveState();
      g.reduceClipRegion(clip);
      const juce::Image& img = impl.image();
      if (img.isValid()) {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(img, imageRect());
      }
      g.restoreState();

      const auto& session = impl.o.session_;
      const auto& pack = session.pack();
      const int sel[2] = {session.model(0), session.blend() ? session.model(1) : -1};
      const int selDot[2] = {pack.dotOfModel(sel[0]), pack.dotOfModel(sel[1])};
      auto drawDot = [&](int d) {
        const auto& dot = pack.dots()[static_cast<std::size_t>(d)];
        const auto c = toScreen(dot.pos);
        const bool s0 = d == selDot[0], s1 = d == selDot[1];
        const juce::Rectangle<float> b(c.x - kDotRadius, c.y - kDotRadius, 2 * kDotRadius, 2 * kDotRadius);
        g.setColour(s0 ? kGreen : s1 ? L::text() : juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(b);
        g.setColour(s0 ? kGreen : L::text().withAlpha(0.75f));
        g.drawEllipse(b, 2.0f);
        g.setColour(s0 || s1 ? juce::Colour(0xff10140a) : L::text());
        g.setFont(L::labelFont(11.0f));
        g.drawText(juce::String(d + 1), b.toNearestInt(), juce::Justification::centred);
        if (dot.offAxis) {  // angle mark
          g.setColour(L::text().withAlpha(0.9f));
          juce::Path a;
          a.startNewSubPath(c.x + kDotRadius + 2, c.y - kDotRadius - 1);
          a.lineTo(c.x + kDotRadius + 9, c.y - kDotRadius - 9);
          a.startNewSubPath(c.x + kDotRadius + 2, c.y - kDotRadius - 1);
          a.lineTo(c.x + kDotRadius + 12, c.y - kDotRadius - 1);
          g.strokePath(a, juce::PathStrokeType(1.6f));
        }
        if (d == hoverDot) {
          g.setColour(juce::Colours::white.withAlpha(0.9f));
          g.drawEllipse(c.x - kDotRadius - 5, c.y - kDotRadius - 5, 2 * (kDotRadius + 5), 2 * (kDotRadius + 5), 2.0f);
        }
      };
      const int nDots = static_cast<int>(pack.dots().size());
      for (int d = 0; d < nDots; ++d) {
        if (d == selDot[0] || d == selDot[1]) {  // glow under the mic
          const auto c = toScreen(pack.dots()[static_cast<std::size_t>(d)].pos);
          g.setColour((d == selDot[0] ? kGreen : L::text()).withAlpha(0.35f));
          g.fillEllipse(c.x - kDotRadius - 6, c.y - kDotRadius - 6, 2 * (kDotRadius + 6), 2 * (kDotRadius + 6));
        } else {
          drawDot(d);
        }
      }
      for (int m = 1; m >= 0; --m)  // mic 1 on top
        if (micPlaced(m) || dragMic == m) drawMic(g, micPoint(m), m);
      for (int d = 0; d < nDots; ++d)  // the selected dots stay visible over the mic heads
        if (d == selDot[0] || d == selDot[1]) drawDot(d);
    }

    void drawMic(juce::Graphics& g, juce::Point<float> c, int m) const {
      const auto& session = impl.o.session_;
      g.setColour(juce::Colours::black.withAlpha(0.45f));
      g.fillEllipse(c.x - 14, c.y - 10, 34, 34);
      g.setColour(juce::Colour(0xff9a9a9a));
      g.fillEllipse(c.x - 17, c.y - 17, 34, 34);
      g.setColour(juce::Colour(0xff2c2c2c));
      g.fillEllipse(c.x - 14, c.y - 14, 28, 28);
      g.setGradientFill(juce::ColourGradient(juce::Colour(0xff7a7a7a), c.x - 8, c.y - 8, juce::Colour(0xff363636), c.x + 10, c.y + 10, true));
      g.fillEllipse(c.x - 11, c.y - 11, 22, 22);
      g.setColour(juce::Colours::black.withAlpha(0.35f));
      for (int k = -8; k <= 8; k += 4) {
        g.drawLine(c.x - 10, c.y + static_cast<float>(k), c.x + 10, c.y + static_cast<float>(k), 0.8f);
        g.drawLine(c.x + static_cast<float>(k), c.y - 10, c.x + static_cast<float>(k), c.y + 10, 0.8f);
      }
      if (session.blend()) {
        const bool active = session.active() == m;
        if (active) {
          g.setColour(kGreen);
          g.drawEllipse(c.x - 20, c.y - 20, 40, 40, 2.0f);
        }
        const juce::Rectangle<float> badge(c.x + 9, c.y + 9, 16, 16);
        g.setColour(m == 0 ? kGreen : L::text());
        g.fillEllipse(badge);
        g.setColour(juce::Colour(0xff10140a));
        g.setFont(L::labelFont(12.0f));
        g.drawText(juce::String(m + 1), badge.toNearestInt(), juce::Justification::centred);
      }
    }

    int hitMic(juce::Point<float> p) const {
      int best = -1;
      float bestD = kMicHit;
      for (int m = 0; m < (impl.o.session_.blend() ? 2 : 1); ++m) {
        if (!micPlaced(m)) continue;
        const float d = micPoint(m).getDistanceFrom(p);
        if (d < bestD) {
          bestD = d;
          best = m;
        }
      }
      return best;
    }

    void mouseDown(const juce::MouseEvent& e) override {
      if (impl.o.session_.readOnly() || impl.o.session_.pack().dots().empty()) return;
      const int hit = hitMic(e.position);
      dragMic = hit >= 0 ? hit : impl.o.session_.active();
      impl.o.session_.setActive(dragMic);
      dragPos = clampToImage(e.position);
      updateHover();
      impl.updateAll();
      repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override {
      if (dragMic < 0) return;
      dragPos = clampToImage(e.position);
      updateHover();
      repaint();
    }
    void mouseUp(const juce::MouseEvent& e) override {
      if (dragMic < 0) return;
      const int m = dragMic;
      dragMic = -1;
      hoverDot = -1;
      const auto& session = impl.o.session_;
      const mic::Snap s = session.pack().snapToNearest(toNorm(clampToImage(e.position)), session.model(m));
      repaint();
      if (s.model >= 0) impl.o.submit(impl.o.session_.choose(impl.o.processor_.currentPreset(), m, s.model));
    }
    // The drag is cancelled without loading anything (e.g. the page closes).
    void cancelDrag() {
      dragMic = -1;
      hoverDot = -1;
    }

    juce::Point<float> clampToImage(juce::Point<float> p) const {
      const auto r = imageRect();
      return {std::clamp(p.x, r.getX(), r.getRight()), std::clamp(p.y, r.getY(), r.getBottom())};
    }
    void updateHover() {
      const auto& session = impl.o.session_;
      hoverDot = session.pack().snapToNearest(toNorm(dragPos), session.model(dragMic)).dot;
    }

    Impl& impl;
    int dragMic = -1, hoverDot = -1;
    juce::Point<float> dragPos;
  };

  // Magnitude response of the selected (or mixed) IR, 20 Hz - 20 kHz, log frequency.
  class Plot : public juce::Component {
   public:
    Plot() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override {
      const auto b = getLocalBounds().toFloat();
      g.setColour(juce::Colour(0xff0e0d0c));
      g.fillRoundedRectangle(b, 6.0f);
      g.setColour(L::placeholderText());
      g.setFont(L::monoFont(10.0f));
      g.drawText("IR frequency response", 8, 4, 220, 14, juce::Justification::centredLeft);
      const int n = static_cast<int>(response.db.size());
      const auto area = b.reduced(10.0f, 22.0f);
      auto xOf = [&](double hz) {
        return area.getX() + static_cast<float>(std::log(hz / mic::kPlotLowHz) / std::log(mic::kPlotHighHz / mic::kPlotLowHz)) * area.getWidth();
      };
      g.setColour(L::rule());
      for (double hz : {100.0, 1000.0, 10000.0}) {
        g.drawVerticalLine(juce::roundToInt(xOf(hz)), area.getY(), area.getBottom());
        g.setColour(L::placeholderText());
        g.setFont(L::monoFont(9.0f));
        g.drawText(hz >= 1000 ? juce::String(juce::roundToInt(hz / 1000)) + "k" : juce::String(juce::roundToInt(hz)), juce::roundToInt(xOf(hz)) + 2,
                   juce::roundToInt(area.getBottom()) + 2, 30, 12, juce::Justification::centredLeft);
        g.setColour(L::rule());
      }
      if (n < 2) return;
      // The mean level of the guitar band.
      double mid = 0.0;
      int cnt = 0;
      for (int i = 0; i < n; ++i) {
        const double f = mic::plotFrequency(i, n);
        if (f >= 200.0 && f <= 4000.0) {
          mid += static_cast<double>(response.db[static_cast<std::size_t>(i)]);
          ++cnt;
        }
      }
      mid = cnt > 0 ? mid / cnt : 0.0;
      const double top = mid + 12.0, bottom = mid - 36.0;  // a 48 dB window around the guitar band's level
      for (double d = std::ceil(bottom / 10.0) * 10.0; d < top; d += 10.0) {
        const float y = area.getBottom() - static_cast<float>((d - bottom) / (top - bottom)) * area.getHeight();
        g.setColour(L::rule().withAlpha(0.5f));
        g.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
      }
      juce::Path p;
      for (int i = 0; i < n; ++i) {
        const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(n - 1);
        const double v = std::clamp(static_cast<double>(response.db[static_cast<std::size_t>(i)]), bottom, top);
        const float y = area.getBottom() - static_cast<float>((v - bottom) / (top - bottom)) * area.getHeight();
        if (i == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
      }
      g.setColour(kGreen);
      g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
    }
    mic::Response response;
  };

  explicit Impl(MicPage& page) : o(page), stage(*this) {
    // --- assets
    const Embedded e4 = loadEmbedded(BinaryData::cab_4x12_open_png, BinaryData::cab_4x12_open_pngSize, BinaryData::cab_4x12_open_json,
                                     BinaryData::cab_4x12_open_jsonSize);
    const Embedded e2 = loadEmbedded(BinaryData::cab_2x12_open_png, BinaryData::cab_2x12_open_pngSize, BinaryData::cab_2x12_open_json,
                                     BinaryData::cab_2x12_open_jsonSize);
    img4 = e4.image;
    lay4 = e4.layout;
    img2 = e2.image;
    lay2 = e2.layout;
    o.session_.setLayout(lay4);

    // --- widgets
    styleButton(back, juce::String::fromUTF8("\xe2\x80\xb9 RIG"), "Back to the rig");
    back.onClick = [this] {
      stage.cancelDrag();
      if (o.onClose) o.onClose();
    };
    title.setText(juce::String::fromUTF8("CAB \xc2\xb7 MIC"), juce::dontSendNotification);
    title.setFont(L::wordmarkFont().withHeight(22.0f));
    title.setColour(juce::Label::textColourId, kGreen);
    hint.setText(juce::String::fromUTF8("DRAG THE MIC \xc2\xb7 EACH POSITION IS A REAL IR FROM THE CAPTURE PACK"), juce::dontSendNotification);
    hint.setFont(L::labelFont(11.0f));
    hint.setColour(juce::Label::textColourId, L::dimText());
    packLabel.setText("IR PACK", juce::dontSendNotification);
    selectedLabel.setText("SELECTED IR", juce::dontSendNotification);
    for (juce::Label* l : {&packLabel, &selectedLabel}) {
      l->setFont(L::labelFont(11.0f));
      l->setColour(juce::Label::textColourId, L::dimText());
    }
    packTitle.setFont(L::titleFont(20.0f));
    packSub.setFont(L::labelFont(11.0f));
    packSub.setColour(juce::Label::textColourId, L::dimText());
    status.setFont(L::bodyFont(12.0f));
    for (juce::Label* l : {&irName[0], &irName[1]}) l->setFont(L::monoFont(13.0f));
    static const char* fieldNames[4] = {"SPEAKER", "MIC", "DISTANCE", "POSITION"};
    for (int i = 0; i < 4; ++i) {
      fieldLabel[i].setText(fieldNames[i], juce::dontSendNotification);
      fieldLabel[i].setFont(L::labelFont(11.0f));
      fieldLabel[i].setColour(juce::Label::textColourId, L::dimText());
      fieldValue[i].setFont(L::monoFont(15.0f));
    }
    mixLabel.setText("MIC MIX", juce::dontSendNotification);
    mixLabel.setFont(L::labelFont(11.0f));
    mixLabel.setColour(juce::Label::textColourId, L::dimText());
    mixRead.setFont(L::monoFont(13.0f));
    mixRead.setJustificationType(juce::Justification::centredRight);
    note.setFont(L::bodyFont(13.0f));
    note.setColour(juce::Label::textColourId, L::dimText());
    note.setJustificationType(juce::Justification::topLeft);
    note.setMinimumHorizontalScale(1.0f);
    note.setText("Positions come from the IR pack's own mic shots; dots show where the pack has an IR. Mic distance and angle snap to the nearest real capture.",
                 juce::dontSendNotification);
    studioNote.setFont(L::bodyFont(13.0f));
    studioNote.setColour(juce::Label::textColourId, L::studio());
    studioNote.setText("studio blend: per-path cabs; mic placement edits the shared cab only", juce::dontSendNotification);

    for (juce::Label* l : {&title, &hint, &packLabel, &selectedLabel, &packTitle, &packSub, &status, &irName[0], &irName[1], &mixLabel, &mixRead,
                           &note, &studioNote, &fieldLabel[0], &fieldLabel[1], &fieldLabel[2], &fieldLabel[3], &fieldValue[0], &fieldValue[1],
                           &fieldValue[2], &fieldValue[3]}) {
      l->setInterceptsMouseClicks(false, false);
      o.addAndMakeVisible(*l);
    }

    styleButton(loadPackBtn, "LOAD PACK", "Fetch every IR of this TONE3000 IR pack (runs sawblade-t3k pack) so the page can show all its mic positions");
    loadPackBtn.onClick = [this] { o.loadPack(); };
    styleButton(folderBtn, juce::String::fromUTF8("LOAD IR FOLDER\xe2\x80\xa6"), "Use a local folder of IR .wav files as the pack");
    folderBtn.onClick = [this] { chooseFolder(); };
    styleButton(cancelBtn, "CANCEL", "Stop downloading the pack");
    cancelBtn.onClick = [this] { tool.cancel(); };
    styleButton(locateBtn, juce::String::fromUTF8("LOCATE\xe2\x80\xa6"), "Find the sawblade-t3k executable and remember it");
    locateBtn.onClick = [this] { chooseExecutable(); };
    styleButton(abBtn, "A / B", "Toggle between the last two IRs you chose for this mic");
    abBtn.onClick = [this] { o.submit(o.session_.toggleAB(o.processor_.currentPreset())); };
    styleButton(nextBtn, "NEXT POSITION", "Move this mic to the next position of the pack");
    nextBtn.onClick = [this] { o.submit(o.session_.nextPosition(o.processor_.currentPreset())); };
    styleButton(blendBtn, "BLEND 2 MICS", "Blend two mic positions into one cab IR (the no-cab NAM export stays exact)");
    blendBtn.setClickingTogglesState(false);
    blendBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2f4a1a));
    blendBtn.onClick = [this] {
      stage.cancelDrag();
      o.submit(o.session_.setBlend(o.processor_.currentPreset(), !o.session_.blend()));
    };
    for (juce::Button* b : std::initializer_list<juce::Button*>{&back, &loadPackBtn, &folderBtn, &cancelBtn, &locateBtn, &abBtn, &nextBtn, &blendBtn})
      o.addAndMakeVisible(*b);

    mixSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    mixSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    mixSlider.setRange(0.0, 100.0, 1.0);
    mixSlider.setDoubleClickReturnValue(true, 50.0);
    mixSlider.setTitle("Mic mix");
    mixSlider.setTooltip("Mix between mic 1 (A) and mic 2 (B), 0 to 100 %");
    mixSlider.setColour(juce::Slider::trackColourId, kGreen);
    mixSlider.setColour(juce::Slider::thumbColourId, L::text());
    mixSlider.onValueChange = [this] { mixMoved(); };
    mixSlider.onDragEnd = [this] {
      if (mixDirty) submitMix();
    };
    o.addAndMakeVisible(mixSlider);

    for (int i = 1; i <= 2; ++i) {
      juce::ComboBox& c = i == 1 ? micCombo : distCombo;
      c.setTitle(i == 1 ? "Mic model" : "Mic distance");
      c.setTooltip(i == 1 ? "Mics this position has an IR for" : "Distances this position has an IR for");
      c.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff0e0d0c));
      c.setColour(juce::ComboBox::textColourId, L::text());
      c.setColour(juce::ComboBox::outlineColourId, L::chipBorder());
      c.setColour(juce::ComboBox::arrowColourId, L::dimText());
      o.addChildComponent(c);
    }
    micCombo.onChange = [this] { comboChanged(true); };
    distCombo.onChange = [this] { comboChanged(false); };

    progressBar = std::make_unique<juce::ProgressBar>(progressValue);
    progressBar->setPercentageDisplay(false);
    progressBar->setTextToDisplay("");
    progressBar->setColour(juce::ProgressBar::foregroundColourId, kGreen);
    progressBar->setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff0e0d0c));
    o.addChildComponent(*progressBar);
    progressLabel.setFont(L::monoFont(11.0f));
    progressLabel.setColour(juce::Label::textColourId, L::dimText());
    progressLabel.setInterceptsMouseClicks(false, false);
    o.addChildComponent(progressLabel);

    o.addAndMakeVisible(stage);
    o.addAndMakeVisible(plot);
  }

  // --- the cab image in use
  bool use2x12() const { return o.session_.pack().preferredCab() == "2x12" && lay2.valid() && img2.isValid(); }
  const juce::Image& image() const { return use2x12() ? img2 : img4; }
  const mic::CabLayout& layout() const { return use2x12() ? lay2 : lay4; }
  void applyLayout() {
    const mic::CabLayout& l = layout();
    if (l.valid()) o.session_.setLayout(l);
    stage.repaint();
  }

  // --- tool
  void chooseFolder() {
    chooser = std::make_unique<juce::FileChooser>("Choose a folder of IR .wav files", juce::File(), "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f != juce::File()) o.loadFolder(fs::path(f.getFullPathName().toStdString()));
    });
  }
  void chooseExecutable() {
    chooser = std::make_unique<juce::FileChooser>("Locate the sawblade-t3k executable", juce::File(settings::t3kExecutable().string()), "*");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      std::string err;
      if (!settings::setT3kExecutable(fs::path(f.getFullPathName().toStdString()), &err)) {
        setMessage(juce::String(err), true);
        return;
      }
      needLocate = false;
      setMessage({}, false);
      o.loadPack();
    });
  }

  void onToolProgress(const T3kTool::Progress& p) {
    progressValue = p.total > 0 ? static_cast<double>(p.done) / static_cast<double>(p.total) : -1.0;
    progressLabel.setText(juce::String(p.done) + " / " + juce::String(p.total) + (p.name.empty() ? "" : "  " + juce::String(juce::CharPointer_UTF8(p.name.c_str()))),
                          juce::dontSendNotification);
  }
  void onToolDone(const T3kTool::Result& r) {
    toolRunning = false;
    needLocate = r.status == T3kTool::Status::MissingExecutable;
    if (r.status == T3kTool::Status::Ok) {
      std::string err;
      mic::IrPack p = mic::IrPack::fromManifestFile(packManifestPath(o.session_.toneId()), &err);
      if (p.empty()) {
        setMessage(juce::String(err.empty() ? "the pack is empty" : err), true);
      } else {
        o.session_.setPack(std::move(p));
        setMessage({}, false);
        packChanged();
      }
    } else if (r.status == T3kTool::Status::Cancelled) {
      setMessage("Pack download cancelled.", false);
    } else {
      setMessage(juce::String(juce::CharPointer_UTF8(r.message.c_str())), true);
    }
    updateAll();
  }
  void packChanged() {
    applyLayout();
    responseKey.clear();
    updateAll();
  }

  void setMessage(const juce::String& m, bool error) {
    localMessage = m;
    localIsError = error;
    updateStatus();
  }
  void updateStatus() {
    if (localMessage.isNotEmpty()) {
      status.setColour(juce::Label::textColourId, localIsError ? L::error() : L::warning());
      status.setText(localMessage, juce::dontSendNotification);
      return;
    }
    const auto st = o.processor_.status();
    if (!st.error.empty() && !st.loading) {
      status.setColour(juce::Label::textColourId, L::error());
      status.setText(juce::String(juce::CharPointer_UTF8(st.error.c_str())), juce::dontSendNotification);
    } else if (!o.session_.cab().enabled) {
      status.setColour(juce::Label::textColourId, L::warning());
      status.setText("The cab is bypassed in this preset: mic changes are kept but silent.", juce::dontSendNotification);
    } else {
      status.setText({}, juce::dontSendNotification);
    }
  }

  // --- mix
  void mixMoved() {
    if (updating || !o.session_.blend()) return;
    o.session_.setMixValue(mixSlider.getValue() / 100.0);
    mixDirty = true;
    updateMixText();
    refreshResponse();
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastMixSubmit >= static_cast<juce::uint32>(kMixIntervalMs)) submitMix();
    else if (!o.isTimerRunning()) o.startTimer(kMixIntervalMs);
  }
  void submitMix() {
    mixDirty = false;
    lastMixSubmit = juce::Time::getMillisecondCounter();
    o.submit(o.session_.setMix(o.processor_.currentPreset(), mixSlider.getValue() / 100.0));
  }
  void updateMixText() {
    const int b = juce::roundToInt(mixSlider.getValue());
    mixRead.setText("A " + juce::String(100 - b) + " % / B " + juce::String(b) + " %", juce::dontSendNotification);
  }

  // --- combos
  void comboChanged(bool isMic) {
    if (updating) return;
    const auto& choices = isMic ? micChoice : distChoice;
    const int id = (isMic ? micCombo : distCombo).getSelectedId();
    if (id < 1 || id > static_cast<int>(choices.size())) return;
    o.submit(o.session_.choose(o.processor_.currentPreset(), o.session_.active(), choices[static_cast<std::size_t>(id - 1)]));
  }

  // --- the response plot
  const mic::Spectrum& spectrumFor(const Capture& c, bool normalize) {
    const std::string key = c.resolvedPath.string() + (normalize ? "|n" : "|r");
    auto it = spectra.find(key);
    if (it == spectra.end()) {
      std::string err;
      mic::Spectrum s = mic::spectrumOfCapture(c, normalize, &err);
      if (s.empty()) setMessage(juce::String("Cannot read the IR: ") + juce::String(juce::CharPointer_UTF8(err.c_str())), true);
      if (spectra.size() > 64) spectra.clear();
      it = spectra.emplace(key, std::move(s)).first;
    }
    return it->second;
  }
  void refreshResponse() {
    const auto& s = o.session_;
    const bool blend = s.blend();
    const std::string key = s.capture(0).resolvedPath.string() + "|" + (blend ? s.capture(1).resolvedPath.string() + "|" + std::to_string(s.mix()) : "") +
                            (s.cab().normalize ? "|n" : "|r");
    if (key == responseKey) return;
    responseKey = key;
    const bool norm = s.cab().normalize;
    const mic::Spectrum a = spectrumFor(s.capture(0), norm);  // copy: the next call may rehash
    if (blend) {
      const mic::Spectrum b = spectrumFor(s.capture(1), norm);
      plot.response = mic::responseFromSpectrum(a, &b, s.mix());
    } else {
      plot.response = mic::responseFromSpectrum(a);
    }
    plot.repaint();
  }

  // --- the whole page from the session
  void updateAll() {
    const auto& s = o.session_;
    const auto& pack = s.pack();
    const bool ro = s.readOnly();

    // IR pack card
    juce::String ttl, sub;
    const auto& info = pack.info();
    const std::string capTitle = s.capture(0).source ? s.capture(0).source->title : std::string();
    if (info.kind == mic::PackInfo::Kind::Folder) {
      ttl = juce::String(juce::CharPointer_UTF8(info.title.c_str()));
      sub = "LOCAL FOLDER" + kDot + juce::String(pack.size()) + " IRs";
    } else {
      ttl = juce::String(juce::CharPointer_UTF8((info.title.empty() ? s.capture(0).file : info.title).c_str()));
      juce::StringArray parts;
      if (!info.creator.empty()) parts.add("@" + juce::String(juce::CharPointer_UTF8(info.creator.c_str())));
      if (!info.license.empty()) parts.add(juce::String(juce::CharPointer_UTF8(info.license.c_str())));
      parts.add(juce::String(pack.size()) + (pack.size() == 1 ? " IR" : " IRs"));
      if (info.kind == mic::PackInfo::Kind::Manifest || !info.toneId.empty()) parts.add("VIA TONE3000");
      sub = parts.joinIntoString(kDot).toUpperCase();
    }
    if (info.kind == mic::PackInfo::Kind::Single && ttl.isEmpty()) ttl = juce::String(capTitle);
    packTitle.setText(ttl, juce::dontSendNotification);
    packSub.setText(sub, juce::dontSendNotification);

    // buttons
    const bool canLoad = !toolRunning && !s.toneId().empty() &&
                         !(info.kind == mic::PackInfo::Kind::Manifest && info.toneId == s.toneId());
    loadPackBtn.setVisible(canLoad);
    cancelBtn.setVisible(toolRunning);
    progressBar->setVisible(toolRunning);
    progressLabel.setVisible(toolRunning);
    locateBtn.setVisible(needLocate && !toolRunning);
    abBtn.setEnabled(s.canToggleAB());
    nextBtn.setEnabled(!ro && !pack.dots().empty() && pack.dots().size() > 1);
    blendBtn.setEnabled(!ro);
    blendBtn.setToggleState(s.blend(), juce::dontSendNotification);
    studioNote.setVisible(ro);
    note.setVisible(!ro);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&mixLabel, &mixRead, &mixSlider}) c->setVisible(s.blend() && !ro);
    stage.setMouseCursor(ro ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);

    // mix slider follows the session unless it is being dragged
    if (s.blend() && !mixSlider.isMouseButtonDown()) {
      updating = true;
      mixSlider.setValue(s.mix() * 100.0, juce::dontSendNotification);
      updating = false;
    }
    updateMixText();

    // fields for the active mic
    const int sel = s.model(s.active());
    const mic::MicShot shot = sel >= 0 ? pack.models()[static_cast<std::size_t>(sel)].shot : mic::MicShot{};
    const juce::String dash = juce::String::fromUTF8("\xe2\x80\x94");
    const int nDrivers = static_cast<int>(layout().drivers.size());
    fieldValue[0].setText(sel < 0 ? dash : slotName(shot.speakerSlot, nDrivers) + kDot + (shot.speaker == "unknown" ? juce::String("UNKNOWN") : upper(shot.speaker)),
                          juce::dontSendNotification);
    fieldValue[1].setText(sel < 0 || shot.mic == "unknown" ? dash : upper(shot.mic) + kDot + upper(mic::micTypeName(shot.micType)), juce::dontSendNotification);
    fieldValue[2].setText(sel < 0 ? dash : distanceText(shot.distanceIn), juce::dontSendNotification);
    fieldValue[3].setText(sel < 0 ? dash
                          : shot.position != mic::MicPosition::Unknown ? juce::String(mic::positionName(shot.position))
                          : shot.positionIndex > 0                      ? "POSITION " + juce::String(shot.positionIndex)
                                                                        : dash,
                          juce::dontSendNotification);

    // MIC / DISTANCE become combo boxes when several models share the selected dot
    micChoice.clear();
    distChoice.clear();
    updating = true;
    micCombo.clear(juce::dontSendNotification);
    distCombo.clear(juce::dontSendNotification);
    const int dot = pack.dotOfModel(sel);
    if (dot >= 0 && !ro) {
      const auto& models = pack.dots()[static_cast<std::size_t>(dot)].models;
      std::vector<std::string> mics;
      for (int m : models) {
        const auto& ms = pack.models()[static_cast<std::size_t>(m)].shot;
        if (std::find(mics.begin(), mics.end(), ms.mic) == mics.end()) mics.push_back(ms.mic);
      }
      // one entry per mic: the model of this dot with that mic nearest to the current distance
      for (std::size_t i = 0; i < mics.size(); ++i) {
        int best = -1;
        double bestD = 1e300;
        for (int m : models) {
          const auto& ms = pack.models()[static_cast<std::size_t>(m)].shot;
          if (ms.mic != mics[i]) continue;
          const double dd = std::isnan(ms.distanceIn) || std::isnan(shot.distanceIn) ? 1e299 : std::fabs(ms.distanceIn - shot.distanceIn);
          if (best < 0 || dd < bestD) {
            best = m;
            bestD = dd;
          }
        }
        micChoice.push_back(best);
        micCombo.addItem(upper(mics[i]) + (mics[i] == "unknown" ? juce::String() : kDot + upper(mic::micTypeName(pack.models()[static_cast<std::size_t>(best)].shot.micType))),
                         static_cast<int>(i) + 1);
        if (mics[i] == shot.mic) micCombo.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
      }
      std::vector<std::pair<double, int>> dists;  // distance, model (this dot, the current mic)
      for (int m : models) {
        const auto& ms = pack.models()[static_cast<std::size_t>(m)].shot;
        if (ms.mic != shot.mic) continue;
        bool dup = false;
        for (const auto& d : dists)
          if ((std::isnan(d.first) && std::isnan(ms.distanceIn)) || d.first == ms.distanceIn) dup = true;
        if (!dup) dists.emplace_back(ms.distanceIn, m);
      }
      std::stable_sort(dists.begin(), dists.end(), [](const auto& a, const auto& b) { return (std::isnan(b.first) ? !std::isnan(a.first) : a.first < b.first); });
      for (std::size_t i = 0; i < dists.size(); ++i) {
        distChoice.push_back(dists[i].second);
        distCombo.addItem(distanceText(dists[i].first), static_cast<int>(i) + 1);
        if ((std::isnan(dists[i].first) && std::isnan(shot.distanceIn)) || dists[i].first == shot.distanceIn)
          distCombo.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
      }
    }
    updating = false;
    const bool micMany = micChoice.size() > 1, distMany = distChoice.size() > 1;
    micCombo.setVisible(micMany);
    distCombo.setVisible(distMany);
    fieldValue[1].setVisible(!micMany);
    fieldValue[2].setVisible(!distMany);

    // selected IR names
    auto nameOf = [&](int mic) -> juce::String {
      const int m = s.model(mic);
      const std::string n = m >= 0 ? pack.models()[static_cast<std::size_t>(m)].name : s.capture(mic).resolvedPath.stem().string();
      return juce::String(juce::CharPointer_UTF8(n.c_str()));
    };
    if (s.blend()) {
      irName[0].setText(juce::String(s.active() == 0 ? juce::String::fromUTF8("\xe2\x96\xb8 ") : "  ") + "1  " + nameOf(0), juce::dontSendNotification);
      irName[1].setText(juce::String(s.active() == 1 ? juce::String::fromUTF8("\xe2\x96\xb8 ") : "  ") + "2  " + nameOf(1), juce::dontSendNotification);
      irName[s.active()].setColour(juce::Label::textColourId, kIrText);  // the highlighted row is the active mic's
      irName[1 - s.active()].setColour(juce::Label::textColourId, L::dimText());
      irName[1].setVisible(true);
    } else {
      irName[0].setText(nameOf(0), juce::dontSendNotification);
      irName[0].setColour(juce::Label::textColourId, kIrText);
      irName[1].setVisible(false);
    }
    refreshResponse();
    updateStatus();
    stage.repaint();
    o.repaint();
  }

  void resized() {
    back.setBounds(18, 7, 84, 32);
    title.setBounds(118, 6, 160, 34);
    hint.setBounds(292, 14, 900, 18);
    stage.setBounds(kStage);
    packLabel.setBounds(kRightX, 62, 100, 14);
    folderBtn.setBounds(kRightX + kRightW - 164, 56, 164, 26);
    loadPackBtn.setBounds(kRightX + kRightW - 164 - 8 - 110, 56, 110, 26);
    packTitle.setBounds(kRightX, 84, kRightW, 26);
    packSub.setBounds(kRightX, 112, kRightW, 18);
    status.setBounds(kRightX, 134, kRightW - 100, 20);
    progressBar->setBounds(kRightX, 136, kRightW - 100, 16);
    progressLabel.setBounds(kRightX + 8, 136, kRightW - 116, 16);
    cancelBtn.setBounds(kRightX + kRightW - 90, 132, 90, 24);
    locateBtn.setBounds(kRightX + kRightW - 100, 132, 100, 24);
    for (int i = 0; i < 4; ++i) {
      const auto c = fieldCell(i);
      fieldLabel[i].setBounds(c.getX() + 10, c.getY() + 7, c.getWidth() - 20, 14);
      fieldValue[i].setBounds(c.getX() + 10, c.getY() + 24, c.getWidth() - 20, 24);
    }
    micCombo.setBounds(fieldCell(1).getX() + 8, fieldCell(1).getY() + 22, kFieldW - 16, 28);
    distCombo.setBounds(fieldCell(2).getX() + 8, fieldCell(2).getY() + 22, kFieldW - 16, 28);
    selectedLabel.setBounds(kRightX, 294, 200, 14);
    irName[0].setBounds(kRightX, 310, kRightW, 20);
    irName[1].setBounds(kRightX, 330, kRightW, 20);
    plot.setBounds(kRightX, 358, kRightW, 150);
    mixLabel.setBounds(kRightX, 520, 70, 24);
    mixSlider.setBounds(kRightX + 72, 520, 330, 24);
    mixRead.setBounds(kRightX + 410, 520, kRightW - 410, 24);
    abBtn.setBounds(kRightX, 556, 90, 32);
    nextBtn.setBounds(kRightX + 98, 556, 160, 32);
    blendBtn.setBounds(kRightX + 266, 556, 160, 32);
    studioNote.setBounds(kRightX, 600, kRightW, 40);
    note.setBounds(kRightX + 10, kStage.getBottom() - 70, kRightW - 20, 60);
  }

  MicPage& o;
  Stage stage;
  Plot plot;
  juce::Image img4, img2;
  mic::CabLayout lay4, lay2;

  juce::TextButton back, loadPackBtn, folderBtn, cancelBtn, locateBtn, abBtn, nextBtn, blendBtn;
  juce::Label title, hint, packLabel, selectedLabel, packTitle, packSub, status, mixLabel, mixRead, note, studioNote;
  juce::Label irName[2], fieldLabel[4], fieldValue[4];
  juce::Slider mixSlider;
  juce::ComboBox micCombo, distCombo;
  std::vector<int> micChoice, distChoice;
  std::unique_ptr<juce::ProgressBar> progressBar;
  juce::Label progressLabel;
  double progressValue = -1.0;
  std::unique_ptr<juce::FileChooser> chooser;

  T3kTool tool;
  bool toolRunning = false, needLocate = false, updating = false, mixDirty = false;
  juce::uint32 lastMixSubmit = 0;
  juce::String localMessage;
  bool localIsError = false;
  std::map<std::string, mic::Spectrum> spectra;
  std::string responseKey;

  // A submitted preset the processor has not committed yet.
  bool pending = false;
  CabPreset pendingCab;
  juce::uint32 pendingSince = 0;
};

// ---------------------------------------------------------------------------------------------
MicPage::MicPage(SawbladeProcessor& p) : processor_(p) {
  impl_ = std::make_unique<Impl>(*this);
  setTitle("Cab mic page");
  setSize(kWidth, kHeight);
  resized();
}

MicPage::~MicPage() { stopTimer(); }

void MicPage::paint(juce::Graphics& g) {
  g.fillAll(L::background());
  g.setColour(L::panel());
  g.fillRect(0, 0, kWidth, kHeaderH);
  g.setColour(L::rule());
  g.drawHorizontalLine(kHeaderH - 1, 0.0f, static_cast<float>(kWidth));
  for (int i = 0; i < 4; ++i) {
    const auto c = fieldCell(i).toFloat();
    g.setColour(juce::Colour(0xff151311));
    g.fillRoundedRectangle(c, 6.0f);
    g.setColour(L::rule());
    g.drawRoundedRectangle(c.reduced(0.5f), 6.0f, 1.0f);
  }
  const auto nb = juce::Rectangle<float>(static_cast<float>(kRightX), static_cast<float>(kStage.getBottom() - 78), static_cast<float>(kRightW), 78.0f);
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(nb, 6.0f);
}

void MicPage::resized() {
  if (impl_) impl_->resized();
}

void MicPage::open() {
  session_.adopt(processor_.currentPreset());
  if (session_.pack().info().kind != mic::PackInfo::Kind::Manifest) session_.loadCachedPack();  // the pack of this cab's tone, if cached
  impl_->pending = false;
  impl_->applyLayout();
  impl_->responseKey.clear();
  impl_->setMessage({}, false);
  impl_->updateAll();
}

void MicPage::refresh() {
  Impl& d = *impl_;
  const Preset cur = processor_.currentPreset();
  const auto st = processor_.status();
  if (d.pending) {
    if (mic::sameCab(cur.cab, d.pendingCab)) d.pending = false;
    else if (!st.loading && juce::Time::getMillisecondCounter() - d.pendingSince > 1500u) d.pending = false;  // the build failed or was superseded
  }
  // (not while the user is mid-edit: a dragged mic, a fader move the throttle has not submitted yet)
  if (!d.pending && d.stage.dragMic < 0 && !d.mixDirty && !mic::sameCab(cur.cab, session_.build(cur).cab)) {
    session_.adopt(cur);
    d.applyLayout();
    d.updateAll();
  } else {
    d.updateStatus();
  }
}

void MicPage::submit(std::optional<Preset> p) {
  if (!p) {
    impl_->updateAll();
    return;
  }
  impl_->pending = true;
  impl_->pendingCab = p->cab;
  impl_->pendingSince = juce::Time::getMillisecondCounter();
  processor_.loadPreset(std::move(*p));
  impl_->updateAll();
}

void MicPage::timerCallback() {
  if (impl_->mixDirty) impl_->submitMix();
  else stopTimer();
}

int MicPage::dotCount() const { return static_cast<int>(session_.pack().dots().size()); }

juce::Point<float> MicPage::dotCentre(int dot) const {
  const auto& dots = session_.pack().dots();
  if (dot < 0 || dot >= static_cast<int>(dots.size())) return {};
  return impl_->stage.toScreen(dots[static_cast<std::size_t>(dot)].pos) + kStage.getTopLeft().toFloat();
}

juce::Point<float> MicPage::micCentre(int m) const {
  if (!impl_->stage.micPlaced(m)) return {};
  return impl_->stage.micPoint(m) + kStage.getTopLeft().toFloat();
}

juce::Component& MicPage::stage() { return impl_->stage; }
juce::Slider& MicPage::mixSlider() { return impl_->mixSlider; }

juce::Button* MicPage::buttonTitled(const juce::String& title) {
  for (auto* c : getChildren())
    if (auto* b = dynamic_cast<juce::Button*>(c))
      if (b->getTitle() == title) return b;
  return nullptr;
}

juce::String MicPage::cabImageName() const { return impl_->use2x12() ? "2x12" : "4x12"; }
juce::String MicPage::statusText() const { return impl_->status.getText(); }
bool MicPage::responseShown() const { return !impl_->plot.response.empty(); }
bool MicPage::packLoading() const { return impl_->toolRunning; }

bool MicPage::loadFolder(const fs::path& dir) {
  std::string err;
  mic::IrPack p = mic::IrPack::fromFolder(dir, &err);
  if (p.empty()) {
    impl_->setMessage(juce::String(juce::CharPointer_UTF8(err.c_str())), true);
    return false;
  }
  session_.setPack(std::move(p));
  impl_->setMessage({}, false);
  impl_->packChanged();
  return true;
}

void MicPage::loadPack() {
  Impl& d = *impl_;
  const std::string id = session_.toneId();
  if (id.empty() || d.toolRunning) return;
  std::string err;
  if (session_.loadCachedPack(&err)) {  // manifests are cached and reused
    d.setMessage({}, false);
    d.packChanged();
    return;
  }
  std::error_code ec;
  fs::create_directories(packCacheDir(), ec);
  d.setMessage({}, false);
  d.needLocate = false;
  d.progressValue = -1.0;
  d.progressLabel.setText({}, juce::dontSendNotification);
  const auto manifest = packManifestPath(id);
  juce::Component::SafePointer<MicPage> self(this);
  d.toolRunning = d.tool.start(
      T3kTool::packArgs(id, manifest),
      [self](const T3kTool::Progress& p) {
        juce::MessageManager::callAsync([self, p] {
          if (self != nullptr) self->impl_->onToolProgress(p);
        });
      },
      [self](const T3kTool::Result& r) {
        juce::MessageManager::callAsync([self, r] {
          if (self != nullptr) self->impl_->onToolDone(r);
        });
      });
  d.updateAll();
}

}  // namespace sawblade::plugin
