#include "PresetInfoPanel.h"

#include "../SawbladeLookAndFeel.h"

namespace sawblade::plugin {
namespace {
using L = SawbladeLookAndFeel;
juce::String u(const std::string& s) { return juce::String(juce::CharPointer_UTF8(s.c_str())); }
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
}  // namespace

PresetInfoPanel::PresetInfoPanel() {
  setTitle("Preset info");
  setInterceptsMouseClicks(false, false);
}

void PresetInfoPanel::setEntry(const PresetEntry* e) {
  rows_.clear();
  if (e != nullptr) {
    using K = Row::Kind;
    rows_.push_back({K::Name, u(e->name)});
    rows_.push_back({K::Meta, u(e->displayCategory()).toUpperCase() + kDot + u(subBankName(e->bank)).toUpperCase()});
    rows_.push_back({K::File, u(e->file.string())});
    if (!e->loadable()) rows_.push_back({K::Error, u(e->error)});
    if (!e->notes.empty()) rows_.push_back({K::Notes, u(e->notes)});
    if (e->loadable()) {
      rows_.push_back({K::Heading, "CAPTURES"});
      for (const auto& c : e->captures) {
        rows_.push_back({K::Where, u(c.where)});
        if (!c.hasSource) {
          rows_.push_back({K::Title, u(c.file)});
          rows_.push_back({K::Missing, "local file: no attribution recorded"});
          continue;
        }
        rows_.push_back({K::Title, u(c.title.empty() ? c.file : c.title)});
        juce::StringArray attr;
        if (!c.creator.empty()) attr.add("@" + u(c.creator));
        if (!c.license.empty()) attr.add(u(c.license));
        rows_.push_back({K::Attribution, attr.isEmpty() ? juce::String("TONE3000 capture (no creator or licence recorded)") : attr.joinIntoString(kDot)});
        if (c.nonCommercial) rows_.push_back({K::Tag, "NON-COMMERCIAL"});
        if (!c.url.empty()) rows_.push_back({K::Url, u(c.url)});
      }
    }
  }
  repaint();
}

juce::StringArray PresetInfoPanel::rows() const {
  juce::StringArray r;
  for (const auto& row : rows_) r.add(row.text);
  return r;
}

int PresetInfoPanel::nonCommercialTags() const {
  int n = 0;
  for (const auto& r : rows_) n += r.kind == Row::Kind::Tag ? 1 : 0;
  return n;
}

void PresetInfoPanel::paint(juce::Graphics& g) {
  g.setColour(L::panelDeep());
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
  using K = Row::Kind;
  float y = 14.0f;
  const float x = 16.0f, w = static_cast<float>(getWidth()) - 32.0f;
  auto text = [&](const juce::String& s, const juce::Font& f, juce::Colour c, float gapAfter) {
    juce::AttributedString a;
    a.append(s, f, c);
    juce::TextLayout tl;
    tl.createLayout(a, w);
    tl.draw(g, {x, y, w, tl.getHeight() + 2.0f});
    y += tl.getHeight() + gapAfter;
  };
  for (const auto& r : rows_) {
    if (y > static_cast<float>(getHeight())) break;
    switch (r.kind) {
      case K::Name: text(r.text, L::titleFont(20.0f), L::text(), 4.0f); break;
      case K::Meta: text(r.text, L::labelFont(11.0f), L::dimText(), 4.0f); break;
      case K::File: text(r.text, L::monoFont(10.0f), L::placeholderText(), 10.0f); break;
      case K::Error: text(r.text, L::bodyFont(12.0f), L::error(), 10.0f); break;
      case K::Notes: text(r.text, L::bodyFont(13.0f), L::text().withAlpha(0.85f), 14.0f); break;
      case K::Heading:
        g.setColour(L::rule());
        g.fillRect(x, y, w, 1.0f);
        y += 8.0f;
        text(r.text, L::labelFont(11.0f), L::dimText(), 6.0f);
        break;
      case K::Where: y += 4.0f; text(r.text, L::labelFont(10.0f), L::sawText(), 1.0f); break;
      case K::Title: text(r.text, L::bodyFont(14.0f).boldened(), L::text(), 1.0f); break;
      case K::Attribution: text(r.text, L::monoFont(12.0f), L::bodyText(), 2.0f); break;
      case K::Url: text(r.text, L::monoFont(10.0f), L::placeholderText(), 2.0f); break;
      case K::Missing: text(r.text, L::bodyFont(12.0f), L::dimText(), 2.0f); break;
      case K::Tag: {
        const juce::Rectangle<float> b(x, y, 120.0f, 16.0f);
        g.setColour(L::studio().withAlpha(0.2f));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(L::studio());
        g.drawRoundedRectangle(b, 3.0f, 1.0f);
        g.setFont(L::labelFont(10.0f));
        g.drawText(r.text, b, juce::Justification::centred);
        y += 20.0f;
        break;
      }
    }
  }
}

}  // namespace sawblade::plugin
