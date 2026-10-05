#include "ui/screens/DictionaryScreens.h"

#include <stdio.h>
#include <string.h>

#include "app/App.h"
#include "core/lang/Charset.h"
#include "core/search/Search.h"
#include "core/text/Typesetter.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/views/CardText.h"
#include "ui/views/Chrome.h"

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
using RowKind = RowSpec::Kind;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

enum : app::ActionId { kActionRow = app::kFirstViewAction, kActionLetter };

// The lemma the entry and verb table show: the list's choice.
uint16_t gLemma = 0;
// The list runs over English keys (EKEY) instead of headwords (LKEY).
bool gEnglish = false;

// Row `index` of the list in the current mode: its lemma and, in English,
// the English key it is listed under.
bool listRow(const pk::Pack& pack, uint32_t index, uint16_t& lemma, const char** english) {
  if (gEnglish) {
    pk::EnglishKey key;
    if (!pack.englishAt(index, key)) return false;
    lemma = key.lemma;
    if (english) *english = pack.str(key.key);
    return true;
  }
  pk::LemmaKey key;
  if (!pack.lemmaKeyAt(index, key)) return false;
  lemma = key.lemma;
  if (english) *english = nullptr;
  return true;
}

pk::KeyRange prefixRange(const pk::Pack& pack, const char* prefix) {
  return gEnglish ? pack.findEnglishPrefix(prefix) : pack.findLemmaPrefix(prefix);
}

// PLAN.md 8.5: a vulgar entry is always listed and opens, but with "show
// vulgar words" off it shows the headword, the "vulgar" label and the note,
// and leaves out the gloss (and the examples, which translate it).
bool glossHidden(App& app, const pk::Lemma& lemma) {
  return lemma.reg == pk::Register::Vulgar && !app.profile().showVulgar;
}
bool gHideGloss = false;
// Whether the entry's word is in the deck or learnt: a line under its head.
App::DeckState gDeck = App::DeckState::None;

bool starrable(App::DeckState state) { return state == App::DeckState::Open || state == App::DeckState::Starred; }

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

Column columnIn(const Rect area) {
  Column col;
  col.x = area.x;
  col.y = area.y;
  col.width = area.width;
  col.bottom = area.bottom();
  return col;
}

// "2/3" in the bottom right corner of the body.
void drawPageNumber(app::UiScreen& screen, const Theme& theme, uint8_t page, uint8_t count) {
  if (count <= 1) return;
  char text[16];
  snprintf(text, sizeof text, tr(Str::PageFmt), page + 1, count);
  freeink::ui::DrawTarget& t = screen.target();
  const Rect r = screen.takeBottom(t.lineHeight(kSlotSmall), 2);
  t.text(inset(r, theme.margin), text, styled(kSlotSmall, Color::Black, TextAlign::Right));
}

// A sheet's rows go below `top` and above `bottom` (screen coordinates).
void constrainTo(app::UiScreen& screen, int16_t top, int16_t bottom) {
  const Rect safe = screen.frame().safeRect();
  screen.setContentMargin(freeink::ui::Insets{i16(top > safe.y ? top - safe.y : 0), 0,
                                              i16(safe.bottom() > bottom ? safe.bottom() - bottom : 0), 0});
}

// Touch footers: page back and forward on the outer cells.
void pagingBar(ChoiceBar& bar, bool back, bool forward) {
  bar.cells[0].icon = &icons::kChevronLeft24;
  bar.cells[0].enabled = back;
  bar.cells[3].icon = &icons::kChevronRight24;
  bar.cells[3].enabled = forward;
}

// ── Entry blocks ─────────────────────────────────────────────────────────────
// The entry is a column of blocks, paginated whole: 0 the head (label,
// headword, respelling), 1 the glosses, 2 the forms, 3 the synonyms used
// elsewhere, 4 the usage note, then one block per example. An absent block
// takes no room.

constexpr uint8_t kFixedBlocks = 5;
constexpr uint8_t kMaxExamples = 8;

uint8_t blockCount(const pk::Lemma& lemma) {
  if (gHideGloss) return kFixedBlocks;
  return static_cast<uint8_t>(kFixedBlocks + (lemma.exampleCount < kMaxExamples ? lemma.exampleCount : kMaxExamples));
}

bool drawBlockParts(freeink::ui::DisplayTarget& t, Column& col, const pk::Pack& pack, const pk::Lemma& lemma,
                    uint16_t id, uint8_t block, TextSize size) {
  constexpr int16_t kGap = 10;
  switch (block) {
    case 0: {
      char text[96];
      formatLemmaLabel(lemma, text, sizeof text);
      if (text[0] && !drawText(t, col, text, FontRole::Label, size, 1, 6)) return false;
      formatHeadword(pack, lemma, true, text, sizeof text);
      if (!drawHeadword(t, col, text, 0)) return false;
      if (pack.str(lemma.pron)[0] && !drawText(t, col, pack.str(lemma.pron), FontRole::Respelling, size, 1, kGap))
        return false;
      if (gDeck == App::DeckState::Starred && !drawTextIn(t, col, tr(Str::DeckIn), font(FontRole::ChromeSmall), 2, 6))
        return false;
      if (gDeck == App::DeckState::Learnt &&
          !drawTextIn(t, col, tr(Str::DeckLearnt), font(FontRole::ChromeSmall), 1, 6))
        return false;
      drawRule(t, col, kGap);
      return true;
    }
    case 1:
      if (gHideGloss) return true;
      return drawText(t, col, pack.str(lemma.en), FontRole::EnglishGloss, size, 0, kGap);
    case 2: {
      // Feminine and plural, for words that have them.
      char forms[96] = {};
      const char* feminine = pack.str(lemma.feminine);
      const char* plural = pack.str(lemma.plural);
      if (feminine[0] && plural[0]) {
        snprintf(forms, sizeof forms, "%s · %s", feminine, plural);
      } else {
        snprintf(forms, sizeof forms, "%s", feminine[0] ? feminine : plural);
      }
      if (!forms[0]) return true;
      return drawText(t, col, forms, FontRole::SpanishAside, size, 2, kGap);
    }
    case 3: {
      const char* alt = pack.str(lemma.alt);
      if (!alt[0]) return true;
      return drawText(t, col, alt, FontRole::SpanishAside, size, 2, kGap);
    }
    case 4: {
      const char* note = pack.str(lemma.note);
      if (!note[0]) return true;
      return drawText(t, col, note, FontRole::EnglishTranslation, size, 0, kGap);
    }
    default: {
      pk::Sentence sentence;
      if (!pack.sentence(pack.lemmaExample(lemma, static_cast<uint16_t>(block - kFixedBlocks)), sentence)) return true;
      if (!drawSentence(t, col, pack, sentence, id, -1, false, size, 2)) return false;
      return drawText(t, col, pack.str(sentence.en), FontRole::EnglishTranslation, size, 0, kGap);
    }
  }
}

// A whole block or nothing; with `force`, as much of it as fits.
bool drawBlock(freeink::ui::DisplayTarget& t, Column& col, const pk::Pack& pack, const pk::Lemma& lemma, uint16_t id,
               uint8_t block, TextSize size, bool force) {
  Column probe = col;
  probe.dryRun = true;
  probe.whole = true;
  if (drawBlockParts(t, probe, pack, lemma, id, block, size)) {
    if (col.dryRun) {
      col = probe;
      col.dryRun = true;
    } else {
      drawBlockParts(t, col, pack, lemma, id, block, size);
    }
    return true;
  }
  if (!force) return false;
  col.whole = false;
  drawBlockParts(t, col, pack, lemma, id, block, size);
  col.whole = true;
  return true;
}

}  // namespace

void showEntry(App& app, const uint16_t lemma, const core::usage::Source via) {
  pk::Lemma record;
  app.usage().entry(lemma, via, app.pack().lemma(lemma, record) ? app.pack().str(record.es) : "");
  gLemma = lemma;
  app.clearTapFlash();
  app.push(ScreenId::DictEntry);
}

// ── List ─────────────────────────────────────────────────────────────────────

const char* DictionaryScreen::title() const { return tr(gEnglish ? Str::DictionaryEnglish : Str::Dictionary); }

uint32_t DictionaryScreen::total() const { return app_.pack().count(gEnglish ? pk::Section::Ekey : pk::Section::Lkey); }

void DictionaryScreen::enter(const bool returning) {
  traceNext_ = true;
  if (!returning) {
    top_ = 0;
    cursor_ = 0;
  }
  bar_ = ChoiceBar{};
  bar_.cells[0].label = tr(Str::Letters);
  // The X4 Pro searches with its keyboard instead of jumping letter by letter.
  bar_.cells[1].label = tr(Str::Search);
  bar_.cells[2].icon = &icons::kChevronUp24;
  bar_.cells[3].icon = &icons::kChevronDown24;
  logFocus();
}

void DictionaryScreen::setEnglish(const bool english) {
  if (gEnglish == english) return;
  gEnglish = english;
  top_ = 0;
  cursor_ = 0;
  traceNext_ = true;
  platform::log("dict mode %s", english ? "english" : "spanish");
}

void DictionaryScreen::jumpTo(const uint32_t index) {
  top_ = index < total() ? index : 0;
  cursor_ = top_;
  traceNext_ = true;
  logFocus();
}

void DictionaryScreen::logFocus() const {
  uint16_t id = 0;
  const char* english = nullptr;
  pk::Lemma lemma;
  if (!listRow(app_.pack(), cursor_, id, &english) || !app_.pack().lemma(id, lemma)) return;
  platform::log("dict top %lu cursor %lu %s", static_cast<unsigned long>(top_), static_cast<unsigned long>(cursor_),
                english ? english : app_.pack().str(lemma.es));
}

const ChoiceBar* DictionaryScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar.cells[2].enabled = top_ > 0;
  bar.cells[3].enabled = top_ + rows_ < total();
  return &bar_;
}

bool DictionaryScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell.label = tr(Str::Open);
        break;
      case Key::Left:
        cell = CellSpec{};
        cell.label = tr(Str::Letters);
        break;
      case Key::Right:
        cell = CellSpec{};
        cell.icon = &icons::kChevronDown24;
        break;
      default:
        break;
    }
  }
  return true;
}

void DictionaryScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const bool touch = !app_.keyDevice();
  const int16_t rowH = theme.rowHeight;
  const int16_t available = screen.body().height;
  rows_ = static_cast<uint8_t>(available / rowH > 0 ? (available / rowH < 30 ? available / rowH : 30) : 1);
  const uint32_t count = total();
  if (cursor_ < top_ || cursor_ >= top_ + rows_) cursor_ = top_;

  const BitmapFont& headFont = font(FontRole::SpanishText);
  for (uint8_t i = 0; i < rows_ && top_ + i < count; ++i) {
    const uint32_t index = top_ + i;
    uint16_t id = 0;
    const char* english = nullptr;
    pk::Lemma lemma;
    if (!listRow(pack, index, id, &english) || !pack.lemma(id, lemma)) break;
    const Rect row = screen.takeTop(rowH);
    const bool focused = !touch && index == cursor_;
    const Color ink = focused ? Color::White : Color::Black;
    if (focused) t.fill(row, Paint::solid(Color::Black));

    // Spanish: the headword and its gloss. English: the English key and the
    // headword it leads to.
    const Rect inner = inset(row, theme.margin);
    const char* word = english ? english : pack.str(lemma.es);
    t.setFont(kSlotScratch, english ? font(FontRole::EnglishGloss) : headFont);
    TextStyle head = styled(kSlotScratch, ink);
    int16_t headW = i16(t.measureText(kSlotScratch, word, head).width + 2);
    if (headW > inner.width * 3 / 5) headW = i16(inner.width * 3 / 5);
    t.text(Rect{inner.x, inner.y, headW, inner.height}, word, head);
    const int16_t glossX = i16(inner.x + headW + 2 * theme.gap);
    if (glossX < inner.right()) {
      if (english) {
        t.setFont(kSlotScratch, headFont);
        t.text(Rect{glossX, inner.y, i16(inner.right() - glossX), inner.height}, pack.str(lemma.es),
               styled(kSlotScratch, ink));
      } else {
        // A vulgar word with the setting off: its register in place of the gloss.
        t.text(Rect{glossX, inner.y, i16(inner.right() - glossX), inner.height},
               glossHidden(app_, lemma) ? "vulgar" : pack.str(lemma.en), styled(kSlotSmall, ink));
      }
    }
    if (!focused) {
      t.fill(Rect{inner.x, i16(row.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
    }
    if (platform::kSimulator && traceNext_) platform::log("dict row %u %s", i, word);
    if (touch) {
      screen.frame().hit(row, kActionRow, i, freeink::ui::InputTouch);
      if (platform::kSimulator && traceNext_) {
        platform::log("target dictionary/row%u %d %d", i, row.x + row.width / 2, row.y + row.height / 2);
      }
    }
  }
  traceNext_ = false;
}

void DictionaryScreen::moveCursor(const int32_t delta) {
  const uint32_t count = total();
  if (count == 0) return;
  int64_t next = static_cast<int64_t>(cursor_) + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  cursor_ = static_cast<uint32_t>(next);
  // Off the page: the page turns, the cursor at its edge.
  if (cursor_ < top_) top_ = cursor_ + 1 >= rows_ ? cursor_ + 1 - rows_ : 0;
  if (cursor_ >= top_ + rows_) top_ = cursor_;
  logFocus();
  app_.invalidate();
}

void DictionaryScreen::turnPage(const int32_t pages) {
  const uint32_t count = total();
  int64_t next = static_cast<int64_t>(top_) + static_cast<int64_t>(pages) * rows_;
  if (next < 0) next = 0;
  if (next >= count) return;
  if (static_cast<uint32_t>(next) == top_) return;
  top_ = static_cast<uint32_t>(next);
  cursor_ = top_;
  traceNext_ = true;
  logFocus();
  app_.invalidateCard();
}

void DictionaryScreen::openEntry(const uint32_t index) {
  uint16_t lemma = 0;
  if (!listRow(app_.pack(), index, lemma, nullptr)) return;
  showEntry(app_, lemma, core::usage::Source::Dictionary);
}

void DictionaryScreen::onAction(const app::ActionEvent& event) {
  if (event.action == kActionRow && event.value >= 0) {
    openEntry(top_ + static_cast<uint32_t>(event.value));
    return;
  }
  if (event.action != app::kActionChoice) return;
  switch (event.value) {
    case 0:
      app_.clearTapFlash();
      app_.push(ScreenId::DictLetters);
      break;
    case 1:
      app_.clearTapFlash();
      app_.push(ScreenId::Search);
      break;
    case 2:
      turnPage(-1);
      break;
    case 3:
      turnPage(1);
      break;
    default:
      break;
  }
}

bool DictionaryScreen::onInput(const InputEvent& event) {
  if (swipe(event, freeink::ui::SwipeDir::Up)) {
    turnPage(1);
    return true;
  }
  if (swipe(event, freeink::ui::SwipeDir::Down)) {
    turnPage(-1);
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Up:
      moveCursor(-1);
      return true;
    case Key::Down:
      moveCursor(1);
      return true;
    case Key::Right:
      turnPage(1);
      return true;
    case Key::Left:
      app_.push(ScreenId::DictLetters);
      return true;
    case Key::Confirm:
      openEntry(cursor_);
      return true;
    default:
      return false;
  }
}

// ── Letters ──────────────────────────────────────────────────────────────────
// Key devices build a prefix a letter at a time: each letter jumps the list
// to the first word that starts so, and the sheet stays up with the letters
// that can follow (the others grey and skipped by the cursor, which goes
// back to the first after each pick), until Back. Three more cells: a space
// (multi-word headwords), Delete (the last letter) and the English <->
// Spanish switch. With the sheet closed, the list's Down and Right finish
// the job. Worst case over the whole course, 16 rows to a page: 25 presses
// to any of the 5,329 headwords (mean 14), 28 to any of the 10,450 English
// keys (mean 14; several headwords can share one key).
//
// The X4 Pro has search; there a letter jumps and closes the sheet.

void LetterSheet::enter(bool) {
  traceNext_ = true;
  prefix_[0] = '\0';
  length_ = 0;
  // Start on the letter the list is at.
  focus_ = 0;
  const pk::Pack& pack = app_.pack();
  const uint32_t top = static_cast<DictionaryScreen*>(app_.view(ScreenId::Dictionary))->top();
  for (uint8_t i = 0; i < kLetters; ++i) {
    const char query[2] = {static_cast<char>('a' + i), '\0'};
    const pk::KeyRange r = prefixRange(pack, query);
    if (r.count > 0 && r.first <= top) focus_ = i;
  }
  refresh();
  if (!cellEnabled(focus_)) step(1);
}

void LetterSheet::refresh() {
  next_ = core::search::nextLetters(app_.pack(), prefix_, gEnglish);
  platform::log("dict prefix '%s' %s", prefix_, gEnglish ? "english" : "spanish");
}

void LetterSheet::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const bool touch = !app_.keyDevice();
  const Rect body = screen.body();
  const Rect full = screen.frame().screen();
  const uint8_t cells = touch ? kLetters : kCells;
  const uint8_t rowCount = static_cast<uint8_t>((cells + kColumns - 1) / kColumns);
  const int16_t cellH = theme.rowHeight;

  freeink::ui::SheetProps props;
  props.anchor = freeink::ui::SheetEdge::Bottom;
  props.dismissAction = app::kActionBack;
  props.radius = theme.tokens.sheetRadius;
  const int16_t band = i16(props.grabberMargin + props.grabberHeight + props.grabberInset);
  const int16_t titleH = i16(t.lineHeight(kSlotBodyBold) + 2 * theme.gap);
  const int16_t height = i16(band + titleH + rowCount * cellH + 2 * theme.gap);
  const Rect sheet{full.x, i16(body.bottom() - height), full.width, height};
  freeink::ui::sheet(screen.frame(), sheet, props);
  const Rect content = freeink::ui::sheetContentRect(sheet, props);

  const Rect title{i16(content.x + theme.margin), content.y, i16(content.width - 2 * theme.margin), titleH};
  char heading[48];
  if (length_ > 0) {
    snprintf(heading, sizeof heading, "%s %s-", tr(Str::JumpToLetter), prefix_);
  } else {
    snprintf(heading, sizeof heading, "%s", tr(gEnglish ? Str::JumpToLetterEnglish : Str::JumpToLetter));
  }
  t.text(title, heading, styled(kSlotBodyBold));
  const int16_t gridX = i16(content.x + theme.margin);
  const int16_t cellW = i16((content.width - 2 * theme.margin) / kColumns);
  for (uint8_t i = 0; i < cells; ++i) {
    const Rect cell{i16(gridX + (i % kColumns) * cellW), i16(title.bottom() + (i / kColumns) * cellH), cellW, cellH};
    const bool focused = !touch && i == focus_;
    const bool enabled = cellEnabled(i);
    if (focused) t.fill(cell, Paint::solid(Color::Black));
    const Color ink = focused ? Color::White : (enabled ? Color::Black : Color::LightGray);
    if (i < kLetters) {
      const char letter[2] = {static_cast<char>('A' + i), '\0'};
      t.text(cell, letter, styled(kSlotTitle, ink, TextAlign::Center));
    } else {
      const char* label = i == kSpaceCell ? "_" : i == kDeleteCell ? tr(Str::DeleteShort) : (gEnglish ? "ES" : "EN");
      t.text(cell, label, styled(kSlotBodyBold, ink, TextAlign::Center));
    }
    if (touch && enabled) {
      screen.frame().hit(cell, kActionLetter, i, freeink::ui::InputTouch);
      if (platform::kSimulator && traceNext_) {
        platform::log("target dict-letters/%c %d %d", 'a' + i, cell.x + cell.width / 2, cell.y + cell.height / 2);
      }
    }
  }
  if (platform::kSimulator && !touch) platform::log("dict letters focus %u", focus_);
  traceNext_ = false;
}

bool LetterSheet::cellEnabled(const uint8_t cell) const {
  if (cell < kLetters) return (next_ >> cell) & 1u;
  if (cell == kSpaceCell) return (next_ >> core::search::kSpaceBit) & 1u;
  if (cell == kDeleteCell) return length_ > 0;
  return cell == kModeCell;
}

void LetterSheet::step(const int8_t by) {
  uint8_t cells[kCells];
  uint8_t n = 0;
  int8_t at = -1;
  for (uint8_t i = 0; i < kCells; ++i) {
    if (!cellEnabled(i)) continue;
    if (i == focus_) at = static_cast<int8_t>(n);
    cells[n++] = i;
  }
  if (n == 0) return;
  // From a cell that cannot be picked (the letters changed under it): the
  // first that can, or the last going back.
  if (at < 0) {
    focus_ = cells[by > 0 ? 0 : n - 1];
    return;
  }
  const int next = ((at + by) % n + n) % n;
  focus_ = cells[next];
}

void LetterSheet::pick(const uint8_t cell) {
  if (!cellEnabled(cell)) return;
  DictionaryScreen& list = *static_cast<DictionaryScreen*>(app_.view(ScreenId::Dictionary));
  const pk::Pack& pack = app_.pack();
  if (cell == kModeCell) {
    list.setEnglish(!gEnglish);
    length_ = 0;
    prefix_[0] = '\0';
  } else if (cell == kDeleteCell) {
    prefix_[--length_] = '\0';
  } else if (length_ + 1 < sizeof prefix_) {
    prefix_[length_++] = cell == kSpaceCell ? ' ' : static_cast<char>('a' + cell);
    prefix_[length_] = '\0';
  }
  const pk::KeyRange r = prefixRange(pack, prefix_);
  list.jumpTo(length_ > 0 ? r.first : 0);
  platform::log("dict letter %c", cell < kLetters ? 'a' + cell : cell == kDeleteCell ? '<' : '=');
  if (length_ > 0) {
    app_.usage().search(gEnglish ? core::usage::SearchMode::English : core::usage::SearchMode::Letter, prefix_,
                        static_cast<uint16_t>(r.count > 0xFFFF ? 0xFFFF : r.count));
  }
  app_.clearTapFlash();
  if (!app_.keyDevice()) {
    app_.pop();
    return;
  }
  refresh();
  // The cursor back on the first letter that can follow.
  focus_ = 0;
  if (!cellEnabled(focus_)) step(1);
  // The list behind changed too: a new screen's refresh.
  app_.invalidateCard();
}

void LetterSheet::onAction(const app::ActionEvent& event) {
  if (event.action == kActionLetter && event.value >= 0) pick(static_cast<uint8_t>(event.value));
  if (event.action == app::kActionBack) {
    app_.clearTapFlash();
    app_.pop();
  }
}

bool LetterSheet::onInput(const InputEvent& event) {
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Left:
      step(-1);
      break;
    case Key::Right:
      step(1);
      break;
    case Key::Up:
      step(-kColumns);
      break;
    case Key::Down:
      step(kColumns);
      break;
    case Key::Confirm:
      pick(focus_);
      return true;
    default:
      return false;
  }
  app_.invalidate();
  return true;
}

bool LetterSheet::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    switch (app_.keys().footerCell(i).key) {
      case Key::Left:
        out.cells[i].icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        out.cells[i].icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

// ── Entry ────────────────────────────────────────────────────────────────────

const char* EntryScreen::title() const { return tr(Str::Dictionary); }

void EntryScreen::enter(const bool returning) {
  traceNext_ = true;
  if (returning) return;
  page_ = 0;
  pageCount_ = 1;
  laidOutHeight_ = -1;
  pk::Lemma lemma;
  app_.pack().lemma(gLemma, lemma);
  // Now, not at the first build: the footer (the X4 Pro's star) is drawn
  // before the body.
  gHideGloss = glossHidden(app_, lemma);
  gDeck = gHideGloss ? App::DeckState::None : app_.deckState(gLemma);
  static const char* const kDeck[] = {"", " learnt", " starred", " starrable"};
  platform::log("dict entry %s%s%s%s", app_.pack().str(lemma.es), isVerb() ? " verb" : "",
                glossHidden(app_, lemma) ? " gloss hidden" : "",
                glossHidden(app_, lemma) ? "" : kDeck[static_cast<uint8_t>(app_.deckState(gLemma))]);
  // Browsing must not cost heap: the entry is read from the pack in place.
  if (!platform::kSimulator) {
    platform::log("heap free %lu, lowest %lu", static_cast<unsigned long>(app_.board().freeHeap()),
                  static_cast<unsigned long>(app_.board().minFreeHeap()));
  }
}

bool EntryScreen::isVerb() const {
  pk::Lemma lemma;
  return app_.pack().lemma(gLemma, lemma) && lemma.verbTable != pk::kNone16;
}

const ChoiceBar* EntryScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  pagingBar(bar, page_ > 0, page_ + 1 < pageCount_);
  if (isVerb()) bar.cells[1].label = tr(Str::Conjugation);
  if (starrable(gDeck))
    bar.cells[2].label = tr(gDeck == App::DeckState::Starred ? Str::DeckShortRemove : Str::DeckShortAdd);
  return &bar_;
}

// Key devices: Confirm does the one thing there is, or opens the choice of
// both (a verb that can be starred).
void EntryScreen::confirm() {
  const bool verb = isVerb();
  const bool star = starrable(gDeck);
  if (verb && star) {
    app_.clearTapFlash();
    app_.push(ScreenId::EntryActions);
  } else if (verb) {
    openTable();
  } else if (star) {
    toggleStar();
  }
}

void EntryScreen::toggleStar() {
  // The head gains or loses a line: a new screen's refresh.
  if (app_.toggleStar(gLemma)) app_.invalidateCard();
}

bool EntryScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        if (isVerb() && starrable(gDeck)) {
          cell.label = tr(Str::Options);
        } else if (isVerb()) {
          cell.label = tr(Str::Conjugation);
        } else if (starrable(gDeck)) {
          cell.label = tr(gDeck == App::DeckState::Starred ? Str::DeckShortRemove : Str::DeckShortAdd);
        }
        break;
      case Key::Left:
        cell = CellSpec{};
        if (page_ > 0) cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (page_ + 1 < pageCount_) cell.icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void EntryScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lemma lemma;
  if (!pack.lemma(gLemma, lemma)) return;
  // The setting can change (Settings over this screen) between builds, and
  // the word can join the deck.
  const App::DeckState deck = glossHidden(app_, lemma) ? App::DeckState::None : app_.deckState(gLemma);
  if (gHideGloss != glossHidden(app_, lemma) || deck != gDeck) laidOutHeight_ = -1;
  gHideGloss = glossHidden(app_, lemma);
  gDeck = deck;
  const uint8_t blocks = blockCount(lemma);

  // Reserve the page number's line, then paginate once for this height.
  const int16_t numberH = i16(t.lineHeight(kSlotSmall) + 2);
  Rect area = inset(screen.body(), theme.margin);
  area.height = i16(area.height - numberH);
  if (laidOutHeight_ != area.height) {
    laidOutHeight_ = area.height;
    pageCount_ = 0;
    uint8_t b = 0;
    while (b < blocks && pageCount_ < kMaxPages) {
      pageStart_[pageCount_++] = b;
      Column col = columnIn(area);
      col.dryRun = true;
      col.whole = true;
      const uint8_t first = b;
      while (b < blocks && drawBlock(t, col, pack, lemma, gLemma, b, size, b == first)) ++b;
    }
    pageStart_[pageCount_] = blocks;
    if (pageCount_ == 0) pageCount_ = 1;
    if (page_ >= pageCount_) page_ = static_cast<uint8_t>(pageCount_ - 1);
    platform::log("dict entry pages %u", pageCount_);
  }

  Column col = columnIn(area);
  col.whole = true;
  const uint8_t first = pageStart_[page_];
  for (uint8_t b = first; b < pageStart_[page_ + 1]; ++b) drawBlock(t, col, pack, lemma, gLemma, b, size, b == first);
  drawPageNumber(screen, theme, page_, pageCount_);
  traceNext_ = false;
}

void EntryScreen::turn(const int8_t dir) {
  const int next = page_ + dir;
  if (next < 0 || next >= pageCount_) return;
  page_ = static_cast<uint8_t>(next);
  platform::log("dict entry page %u", page_);
  app_.invalidateCard();
}

void EntryScreen::openTable() {
  if (!isVerb()) return;
  app_.clearTapFlash();
  app_.push(ScreenId::VerbTable);
}

void EntryScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == 1) openTable();
  if (event.value == 2 && starrable(gDeck)) toggleStar();
  if (event.value == 3) turn(1);
}

bool EntryScreen::onInput(const InputEvent& event) {
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
      confirm();
      return true;
    default:
      return false;
  }
}

// ── Entry actions ────────────────────────────────────────────────────────────

uint8_t EntryActionsSheet::rowCount() const { return 2; }

void EntryActionsSheet::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  out.kind = RowKind::Action;
  if (index == 0) {
    out.label = tr(Str::Conjugation);
    out.slug = "conjugation";
  } else {
    out.label = tr(gDeck == App::DeckState::Starred ? Str::DeckRemove : Str::DeckAdd);
    out.slug = "deck";
  }
}

void EntryActionsSheet::activate(const uint8_t index) {
  app_.clearTapFlash();
  app_.pop();
  if (index == 0) {
    app_.push(ScreenId::VerbTable);
  } else if (app_.toggleStar(gLemma)) {
    app_.invalidateCard();
  }
}

void EntryActionsSheet::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const Rect body = screen.body();
  const Rect full = screen.frame().screen();
  freeink::ui::SheetProps props;
  props.anchor = freeink::ui::SheetEdge::Bottom;
  props.dismissAction = app::kActionBack;
  props.radius = theme.tokens.sheetRadius;
  const int16_t band = i16(props.grabberMargin + props.grabberHeight + props.grabberInset);
  const int16_t height =
      i16(band + t.lineHeight(kSlotSmall) + 2 * (theme.rowHeight + (theme.touch ? 4 : 0)) + theme.gap);
  const Rect sheet{full.x, i16(body.bottom() - height), full.width, height};
  freeink::ui::sheet(screen.frame(), sheet, props);
  const Rect content = freeink::ui::sheetContentRect(sheet, props);
  constrainTo(screen, content.y, content.bottom());
  buildRows(screen);
}

void EntryActionsSheet::onAction(const app::ActionEvent& event) {
  if (event.action == app::kActionBack) {
    app_.clearTapFlash();
    app_.pop();
    return;
  }
  FormView::onAction(event);
}

// ── Verb table ───────────────────────────────────────────────────────────────

const char* VerbTableScreen::title() const { return tr(Str::Conjugation); }

void VerbTableScreen::enter(const bool returning) {
  if (!returning) tense_ = 0;
  platform::log("verb tense %u %s", tense_, tenseNameSpanish(static_cast<pk::Tense>(tense_)));
  app_.usage().verbTable(gLemma, tense_);
}

const ChoiceBar* VerbTableScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  pagingBar(bar, tense_ > 0, tense_ + 1 < pk::kTenseCount);
  return &bar_;
}

bool VerbTableScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        break;
      case Key::Left:
        cell = CellSpec{};
        if (tense_ > 0) cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (tense_ + 1 < pk::kTenseCount) cell.icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void VerbTableScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lemma lemma;
  pk::VerbInfo info;
  if (!pack.lemma(gLemma, lemma) || !pack.verbInfo(lemma.verbTable, info)) return;
  const pk::Tense tense = static_cast<pk::Tense>(tense_);

  Column col = columnIn(inset(screen.body(), theme.margin));
  drawHeadword(t, col, pack.str(info.infinitive), 0);
  if (!glossHidden(app_, lemma)) drawText(t, col, pack.str(lemma.en), FontRole::EnglishGloss, size, 2, 8);
  drawRule(t, col, 8);

  // The tense, in Spanish, with the chrome language's name beside it.
  const BitmapFont& tenseFont = font(FontRole::SpanishEmphasis, size);
  char page[16];
  snprintf(page, sizeof page, tr(Str::PageFmt), tense_ + 1, pk::kTenseCount);
  const Rect line{col.x, col.y, col.width, tenseFont.yAdvance};
  t.setFont(kSlotScratch, tenseFont);
  t.text(line, tenseNameSpanish(tense), styled(kSlotScratch));
  t.text(line, page, styled(kSlotSmall, Color::Black, TextAlign::Right));
  col.skip(i16(tenseFont.yAdvance + 2));
  if (language() != core::UiLanguage::Spanish) drawText(t, col, tenseName(tense), FontRole::Label, size, 1, 8);
  col.skip(4);

  // Five rows: the subject in italics, the form beside it.
  const BitmapFont& personFont = font(FontRole::SpanishAside, size);
  const BitmapFont& formFont = font(FontRole::SpanishText, size);
  const int16_t rowH = i16(formFont.yAdvance + 12);
  // The forms start a little past the widest subject at this text size.
  int32_t widest = 0;
  for (uint8_t p = 0; p < pk::kPersonCount; ++p) {
    const char* subject = personLabel(static_cast<pk::Person>(p));
    const int32_t w = core::text::Typesetter::measure(personFont, subject, strlen(subject));
    if (w > widest) widest = w;
  }
  int16_t split = i16(widest + 3 * theme.gap);
  if (split > col.width * 3 / 5) split = i16(col.width * 3 / 5);
  for (uint8_t p = 0; p < pk::kPersonCount && col.room() >= rowH; ++p) {
    const pk::Person person = static_cast<pk::Person>(p);
    const char* form = pack.verbForm(lemma.verbTable, tense, person);
    char shown[64];
    if (!form[0]) {
      snprintf(shown, sizeof shown, "%s", TINTA_EM_DASH);
    } else {
      snprintf(shown, sizeof shown, "%s%s", tense == pk::Tense::NegativeImperative ? "no " : "", form);
    }
    const Rect row{col.x, col.y, col.width, rowH};
    t.setFont(kSlotScratch, personFont);
    t.text(Rect{row.x, row.y, split, row.height}, personLabel(person), styled(kSlotScratch));
    t.setFont(kSlotScratch, formFont);
    t.text(Rect{i16(row.x + split), row.y, i16(row.width - split), row.height}, shown, styled(kSlotScratch));
    t.fill(Rect{row.x, i16(row.bottom() - 1), row.width, 1}, Paint::dither(Color::LightGray));
    col.skip(rowH);
  }
  col.skip(10);
  drawLabelled(t, col, tr(Str::Gerund), pack.str(info.gerund), size, 2);
  drawLabelled(t, col, tr(Str::Participle), pack.str(info.participle), size, 0);
}

void VerbTableScreen::turn(const int8_t dir) {
  const int next = tense_ + dir;
  if (next < 0 || next >= pk::kTenseCount) return;
  tense_ = static_cast<uint8_t>(next);
  platform::log("verb tense %u %s", tense_, tenseNameSpanish(static_cast<pk::Tense>(tense_)));
  app_.usage().verbTable(gLemma, tense_);
  app_.invalidateCard();
}

void VerbTableScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == 3) turn(1);
}

bool VerbTableScreen::onInput(const InputEvent& event) {
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
    default:
      return false;
  }
}

}  // namespace tinta::ui
