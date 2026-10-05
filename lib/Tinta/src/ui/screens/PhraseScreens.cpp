#include "ui/screens/PhraseScreens.h"

#include <stdio.h>

#include "app/App.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/views/CardText.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
using app::App;
using app::ScreenId;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

constexpr int16_t kGap = 10;
constexpr uint8_t kPractiseCell = 1;

// The category PhrasesScreen shows: the list's choice.
uint16_t gCategory = 0;

TextStyle styled(freeink::ui::FontId font, Color color = Color::Black, TextAlign align = TextAlign::Left) {
  TextStyle s;
  s.font = font;
  s.color = color;
  s.align = align;
  return s;
}

bool swipe(const InputEvent& e, freeink::ui::SwipeDir dir) {
  return e.kind == InputEvent::Kind::Swipe && !e.fromTopEdge && e.swipe == dir;
}

Rect inset(const Rect r, const int16_t margin) {
  return Rect{i16(r.x + margin), r.y, i16(r.width - 2 * margin), r.height};
}

const char* registerName(const pk::Register reg) {
  switch (reg) {
    case pk::Register::Formal:
      return "formal";
    case pk::Register::Informal:
      return "informal";
    case pk::Register::Vulgar:
      return "vulgar";
    default:
      return "";
  }
}

}  // namespace

// ── Categories ───────────────────────────────────────────────────────────────

const char* PhrasebookScreen::title() const { return tr(Str::Phrases); }

uint16_t PhrasebookScreen::rowCount() const { return static_cast<uint16_t>(app_.pack().count(pk::Section::Phrs)); }

int16_t PhrasebookScreen::rowHeight() const {
  return i16(font(FontRole::SpanishText).yAdvance + font(FontRole::ChromeSmall).yAdvance + 12);
}

void PhrasebookScreen::drawRow(app::UiScreen& screen, const Rect row, const uint16_t index, const bool focused) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  pk::PhraseCategory category;
  if (!pack.phraseCategory(index, category)) return;
  const Color ink = focused ? Color::White : Color::Black;
  if (focused) t.fill(row, Paint::solid(Color::Black));
  const Rect inner = inset(row, theme.margin);
  const int16_t countW = 100;
  const BitmapFont& titleFont = font(FontRole::SpanishText);
  t.setFont(kSlotScratch, titleFont);
  TextStyle title = styled(kSlotScratch, ink);
  title.maxLines = 1;
  t.text(Rect{inner.x, i16(inner.y + 4), i16(inner.width - countW), titleFont.yAdvance}, pack.str(category.title),
         title);
  TextStyle en = styled(kSlotSmall, ink);
  en.maxLines = 1;
  t.text(Rect{inner.x, i16(inner.y + 4 + titleFont.yAdvance), i16(inner.width - countW),
              font(FontRole::ChromeSmall).yAdvance},
         pack.str(category.titleEn), en);
  char count[24];
  snprintf(count, sizeof count, tr(Str::PhrasesCountFmt), category.entryCount);
  t.text(Rect{i16(inner.right() - countW), inner.y, countW, inner.height}, count,
         styled(kSlotSmall, ink, TextAlign::Right));
  if (!focused) t.fill(Rect{inner.x, i16(row.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
  (void)screen;
}

void PhrasebookScreen::activate(const uint16_t index) {
  gCategory = index;
  app_.clearTapFlash();
  app_.push(ScreenId::Phrases);
}

// ── One category ─────────────────────────────────────────────────────────────

const char* PhrasesScreen::title() const {
  pk::PhraseCategory category;
  app_.pack().phraseCategory(category_, category);
  snprintf(title_, sizeof title_, "%s", app_.pack().str(category.title));
  return title_;
}

void PhrasesScreen::enter(const bool returning) {
  if (returning && category_ == gCategory) {
    logPage();
    return;
  }
  category_ = gCategory;
  page_ = 0;
  pageCount_ = 1;
  laidOutHeight_ = -1;
  entryCount_ = 0;
  const pk::Pack& pack = app_.pack();
  pk::PhraseCategory category;
  if (!pack.phraseCategory(category_, category)) return;
  for (uint16_t i = 0; i < category.entryCount && entryCount_ < kMaxEntries; ++i) {
    pk::PhraseEntry entry;
    pk::Sentence sentence;
    if (!pack.phraseEntry(category, i, entry) || !pack.sentence(entry.sentence, sentence)) continue;
    if (sentence.reg == pk::Register::Vulgar && !app_.profile().showVulgar) continue;
    entries_[entryCount_++] = static_cast<uint16_t>(category.firstEntry + i);
  }
  platform::log("phrases %u %s, %u phrases", category_, pack.str(category.title), entryCount_);
  app_.usage().phrasebook(category_, core::usage::PhraseAction::Opened);
}

// One phrase, whole or not at all on a page.
bool PhrasesScreen::drawCard(freeink::ui::DisplayTarget& t, Column& col, const uint8_t index) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::PhraseEntry entry;
  pk::Sentence sentence;
  if (!pack.phraseEntry(entries_[index], entry) || !pack.sentence(entry.sentence, sentence)) return true;
  const auto parts = [&](Column& c) {
    if (!drawText(t, c, pack.str(sentence.es), FontRole::SpanishEmphasis, size, 3, 2)) return false;
    if (pack.str(entry.pron)[0] && !drawText(t, c, pack.str(entry.pron), FontRole::Respelling, size, 2, 4))
      return false;
    if (!drawText(t, c, pack.str(sentence.en), FontRole::EnglishGloss, size, 3, 4)) return false;
    char label[96];
    const char* reg = registerName(sentence.reg);
    const char* note = pack.str(sentence.note);
    if (reg[0] && note[0]) {
      snprintf(label, sizeof label, "%s · %s", reg, note);
    } else {
      snprintf(label, sizeof label, "%s", reg[0] ? reg : note);
    }
    if (label[0] && !drawText(t, c, label, FontRole::EnglishTranslation, size, 3, 4)) return false;
    drawRule(t, c, kGap);
    return true;
  };
  Column probe = col;
  probe.dryRun = true;
  probe.whole = true;
  if (!parts(probe)) return false;
  if (col.dryRun) {
    col.y = probe.y;
  } else {
    parts(col);
  }
  return true;
}

void PhrasesScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DisplayTarget& t = app_.target();
  const int16_t numberH = i16(t.lineHeight(kSlotSmall) + 2);
  Rect area = inset(screen.body(), theme.margin);
  area.height = i16(area.height - numberH);
  const auto columnFor = [&]() {
    Column col;
    col.x = area.x;
    col.y = area.y;
    col.width = area.width;
    col.bottom = area.bottom();
    col.whole = true;
    return col;
  };
  if (laidOutHeight_ != area.height) {
    laidOutHeight_ = area.height;
    pageCount_ = 0;
    uint8_t e = 0;
    while (e < entryCount_ && pageCount_ < kMaxPages) {
      pageStart_[pageCount_++] = e;
      Column col = columnFor();
      col.dryRun = true;
      const uint8_t first = e;
      while (e < entryCount_ && drawCard(t, col, e)) ++e;
      // A card taller than a page still gets one.
      if (e == first) ++e;
    }
    pageStart_[pageCount_] = e;
    if (pageCount_ == 0) pageCount_ = 1;
    if (page_ >= pageCount_) page_ = static_cast<uint8_t>(pageCount_ - 1);
    logPage();
  }
  Column col = columnFor();
  for (uint8_t e = pageStart_[page_]; e < pageStart_[page_ + 1]; ++e) {
    if (!drawCard(t, col, e)) {
      col.whole = false;
      drawCard(t, col, e);
      break;
    }
  }
  if (pageCount_ > 1) {
    char text[16];
    snprintf(text, sizeof text, tr(Str::PageFmt), page_ + 1, pageCount_);
    const Rect r = screen.takeBottom(t.lineHeight(kSlotSmall), 2);
    t.text(inset(r, theme.margin), text, styled(kSlotSmall, Color::Black, TextAlign::Right));
  }
}

void PhrasesScreen::logPage() const { platform::log("phrases %u page %u/%u", category_, page_ + 1, pageCount_); }

const ChoiceBar* PhrasesScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  bar.cells[0].icon = &icons::kChevronLeft24;
  bar.cells[0].enabled = page_ > 0;
  bar.cells[kPractiseCell].label = tr(Str::Practise);
  bar.cells[kPractiseCell].enabled = entryCount_ > 0;
  bar.cells[3].icon = &icons::kChevronRight24;
  // The footer is drawn before the first build has paginated: until then,
  // assume more pages (a turn past the last one does nothing).
  bar.cells[3].enabled = page_ + 1 < pageCount_ || laidOutHeight_ < 0;
  return &bar_;
}

bool PhrasesScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        if (entryCount_ > 0) cell.label = tr(Str::Practise);
        break;
      case Key::Left:
        cell = CellSpec{};
        if (page_ > 0) cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (page_ + 1 < pageCount_ || laidOutHeight_ < 0) cell.icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void PhrasesScreen::turn(const int8_t dir) {
  const int next = page_ + dir;
  if (next < 0 || next >= pageCount_) return;
  page_ = static_cast<uint8_t>(next);
  logPage();
  app_.clearTapFlash();
  app_.invalidateCard();
}

void PhrasesScreen::practise() {
  if (entryCount_ == 0) return;
  app::SessionController& s = app_.session();
  app_.clearTapFlash();
  if (!(s.active() && s.sessionCategory() == static_cast<int32_t>(category_))) {
    // Another session in progress ends here; its grades are kept.
    if (s.active()) s.end(core::usage::SessionEndHow::Left);
    if (!s.startCategory(category_)) return;
    app_.usage().phrasebook(category_, core::usage::PhraseAction::Practised);
  }
  const ScreenId ids[] = {app_.rootId(), ScreenId::Session};
  app_.resetTo(ids, 2);
}

void PhrasesScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == kPractiseCell) practise();
  if (event.value == 3) turn(1);
}

bool PhrasesScreen::onInput(const InputEvent& event) {
  if (swipe(event, freeink::ui::SwipeDir::Left)) {
    turn(1);
    return true;
  }
  if (swipe(event, freeink::ui::SwipeDir::Right)) {
    turn(-1);
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Left:
    case Key::Up:
      turn(-1);
      return true;
    case Key::Right:
    case Key::Down:
      turn(1);
      return true;
    case Key::Confirm:
      practise();
      return true;
    default:
      return false;
  }
}

}  // namespace tinta::ui
