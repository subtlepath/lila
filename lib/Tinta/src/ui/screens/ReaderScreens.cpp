#include "ui/screens/ReaderScreens.h"

#include <stdio.h>
#include <string.h>

#include "app/App.h"
#include "core/library/Library.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/TextBuffers.h"
#include "ui/TypesetView.h"
#include "ui/views/CardText.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

namespace pk = core::pack;
namespace tx = core::text;
namespace lib = core::library;
using app::App;
using app::ScreenId;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

enum : app::ActionId { kActionStart = app::kFirstViewAction, kActionOption };

constexpr int16_t kGap = 10;
// Extra pixels around a word that still count as a tap on it.
constexpr int16_t kTapSlop = 6;

// The story ReaderScreen and QuizScreen show: the list's choice.
uint16_t gStory = 0;

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

core::reader::Fonts readerFonts(App& app) {
  const TextSize size = static_cast<TextSize>(app.profile().textSize);
  return core::reader::Fonts{&font(FontRole::SpanishText, size), &font(FontRole::SpanishEmphasis, size)};
}

tx::Frame frameAt(const Rect area, int16_t y) {
  tx::Frame frame;
  frame.x = area.x;
  frame.y = y;
  frame.width = area.width;
  frame.height = i16(area.bottom() - y);
  frame.lineGap = 6;
  return frame;
}

bool storyOpen(App& app, const pk::Story& story) {
  return story.lesson == pk::kNone16 || app.lessonState(story.lesson) != App::LessonState::Locked;
}

// A reading finished: the last page, or the last question (`right` of
// `total` answered right).
void markRead(App& app, const uint16_t id, const uint8_t right, const uint8_t total) {
  pk::Story story;
  if (!app.pack().story(id, story)) return;
  const uint32_t key = lib::storyKey(app.pack(), story);
  app.readLog().add(key);
  app.usage().storyDone(key, right, total);
  platform::log("read %u", id);
}

}  // namespace

void showReading(App& app, const uint16_t story) {
  gStory = story;
  app.clearTapFlash();
  app.push(ScreenId::Reader);
}

// ── Readings ─────────────────────────────────────────────────────────────────

const char* ReadingsScreen::title() const { return tr(Str::Readings); }

void ReadingsScreen::enter(const bool returning) {
  count_ = lib::readings(app_.pack(), stories_, kMax);
  ListView::enter(returning);
}

int16_t ReadingsScreen::rowHeight() const {
  return i16(font(FontRole::SpanishText).yAdvance + font(FontRole::ChromeSmall).yAdvance + 12);
}

bool ReadingsScreen::selectable(const uint16_t index) const {
  pk::Story story;
  return index < count_ && app_.pack().story(stories_[index], story) && storyOpen(app_, story);
}

void ReadingsScreen::drawRow(app::UiScreen& screen, const Rect row, const uint16_t index, const bool focused) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  pk::Story story;
  if (!pack.story(stories_[index], story)) return;
  const bool open = storyOpen(app_, story);
  const bool read = app_.readLog().contains(lib::storyKey(pack, story));
  const Color ink = focused ? Color::White : Color::Black;
  if (focused) t.fill(row, Paint::solid(Color::Black));
  const Rect inner = inset(row, theme.margin);
  const int16_t stateW = 90;
  const BitmapFont& titleFont = font(FontRole::SpanishText);
  t.setFont(kSlotScratch, titleFont);
  TextStyle title = styled(kSlotScratch, ink);
  title.maxLines = 1;
  t.text(Rect{inner.x, i16(inner.y + 4), i16(inner.width - stateW), titleFont.yAdvance}, pack.str(story.title), title);
  TextStyle en = styled(kSlotSmall, ink);
  en.maxLines = 1;
  t.text(Rect{inner.x, i16(inner.y + 4 + titleFont.yAdvance), i16(inner.width - stateW),
              font(FontRole::ChromeSmall).yAdvance},
         pack.str(story.titleEn), en);
  // At the right: a check once read, "locked", or the level.
  const Rect state{i16(inner.right() - stateW), inner.y, stateW, inner.height};
  if (read) {
    drawIcon(t, Rect{i16(state.right() - 24), state.y, 24, state.height}, icons::kCheck24, ink);
  } else if (!open) {
    t.text(state, tr(Str::Locked), styled(kSlotSmall, ink, TextAlign::Right));
  } else {
    static const char* const kLevels[] = {"", "A1", "A2", "B1", "B2"};
    const uint8_t level = static_cast<uint8_t>(story.level);
    t.text(state, level < 5 ? kLevels[level] : "", styled(kSlotSmall, ink, TextAlign::Right));
  }
  if (!focused) t.fill(Rect{inner.x, i16(row.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
  (void)screen;
}

void ReadingsScreen::activate(const uint16_t index) { showReading(app_, stories_[index]); }

// ── Reader ───────────────────────────────────────────────────────────────────

const char* ReaderScreen::title() const {
  snprintf(title_, sizeof title_, "%s", app_.pack().str(record_.title));
  return title_;
}

void ReaderScreen::enter(const bool returning) {
  traceNext_ = true;
  if (returning && story_ == gStory) {
    logPage();
    return;
  }
  story_ = gStory;
  app_.pack().story(story_, record_);
  key_ = lib::storyKey(app_.pack(), record_);
  opened_ = false;
  paragraphCount_ = static_cast<uint8_t>(core::reader::paragraphs(app_.pack(), record_, paragraphs_, kMaxParagraphs));
  page_ = 0;
  pageCount_ = 0;
  laidOutHeight_ = -1;
  cursor_ = -1;
  glossToken_ = -1;
  englishToken_ = -1;
  lastTapped_ = -1;
  cursorToEnd_ = false;
  platform::log("reading %u %s, %u lines, %u questions", story_, app_.pack().str(record_.title), record_.lineCount,
                record_.questionCount);
}

uint16_t ReaderScreen::loadParagraph(const uint8_t paragraph) {
  return core::reader::paragraphSpans(app_.pack(), record_, paragraphs_[paragraph], readerFonts(app_), sharedSpans(),
                                      kSharedSpanCap);
}

// The first page opens with the English title; every page is a run of
// paragraphs, the last one perhaps cut and continued on the next page.
void ReaderScreen::paginate(const Rect area) {
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pageCount_ = 0;
  pages_[pageCount_++] = PageStart{0, 0, 0};
  Column head = columnIn(area);
  head.dryRun = true;
  drawText(t, head, app_.pack().str(record_.titleEn), FontRole::EnglishTranslation, size, 2, kGap);
  int16_t y = head.y;
  const int16_t paragraphGap = i16(font(FontRole::SpanishText, size).yAdvance / 2);
  for (uint8_t p = 0; p < paragraphCount_ && pageCount_ < kMaxPages; ++p) {
    const uint16_t count = loadParagraph(p);
    tx::Position at{0, 0};
    for (;;) {
      tx::Typesetter typesetter(sharedRuns(), kSharedRunCap);
      const tx::Layout& layout = typesetter.layout(sharedSpans(), count, frameAt(area, y), at);
      if (layout.lineCount == 0) {
        // Not one line fits below what is on this page: a new page.
        if (y == area.y || pageCount_ >= kMaxPages) break;
        pages_[pageCount_++] = PageStart{p, at.span, at.offset};
        y = area.y;
        continue;
      }
      if (layout.complete()) {
        y = i16(y + layout.height + paragraphGap);
        break;
      }
      at = layout.next;
      if (pageCount_ >= kMaxPages) break;
      pages_[pageCount_++] = PageStart{p, at.span, at.offset};
      y = area.y;
    }
  }
  if (page_ >= pageCount_) page_ = static_cast<uint8_t>(pageCount_ - 1);
  platform::log("reader pages %u", pageCount_);
}

uint16_t ReaderScreen::lemmaOf(const uint16_t token, uint16_t* sentenceOut) const {
  core::reader::WordRef ref;
  pk::Sentence sentence;
  pk::Token t;
  const pk::Pack& pack = app_.pack();
  if (!core::reader::findWord(pack, record_, token, ref) || !pack.sentence(ref.sentence, sentence) ||
      !pack.token(sentence, ref.token, t)) {
    return pk::kNone16;
  }
  if (sentenceOut) *sentenceOut = ref.sentence;
  return lib::tokenLemma(pack, sentence, t);
}

bool ReaderScreen::glossable(const uint16_t token) const { return lemmaOf(token) != pk::kNone16; }

void ReaderScreen::drawPage(app::UiScreen& screen, const Rect area) {
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  const PageStart start = pages_[page_];
  const bool last = page_ + 1 >= pageCount_;
  const PageStart end = last ? PageStart{paragraphCount_, 0, 0} : pages_[page_ + 1];
  int16_t y = area.y;
  if (page_ == 0) {
    Column head = columnIn(area);
    drawText(t, head, app_.pack().str(record_.titleEn), FontRole::EnglishTranslation, size, 2, kGap);
    y = head.y;
  }
  const int16_t paragraphGap = i16(font(FontRole::SpanishText, size).yAdvance / 2);
  wordCount_ = 0;
  for (uint8_t p = start.paragraph; p < paragraphCount_ && p <= end.paragraph; ++p) {
    if (p == end.paragraph && end.span == 0 && end.offset == 0 && !last) break;
    const uint16_t count = loadParagraph(p);
    const tx::Position at = p == start.paragraph ? tx::Position{start.span, start.offset} : tx::Position{0, 0};
    tx::Typesetter typesetter(sharedRuns(), kSharedRunCap);
    const tx::Layout& layout = typesetter.layout(sharedSpans(), count, frameAt(area, y), at);
    if (layout.lineCount == 0) break;
    drawTypeset(t, typesetter);
    const tx::Run* runs = typesetter.runs();
    for (uint16_t k = 0; k < typesetter.runCount() && wordCount_ < kMaxWords; ++k) {
      const uint16_t token = sharedSpans()[runs[k].span].token;
      if (token == tx::kNoToken) continue;
      words_[wordCount_++] =
          WordBox{token, runs[k].x, runs[k].lineTop, runs[k].width, i16(runs[k].lineBottom - runs[k].lineTop)};
    }
    if (!layout.complete()) break;
    y = i16(y + layout.height + paragraphGap);
  }

  // The key cursor: on the first word of the page when it arrives, the last
  // when the page was turned back to.
  if (app_.keyDevice()) {
    bool onPage = false;
    for (uint8_t i = 0; i < wordCount_; ++i) onPage = onPage || words_[i].token == cursor_;
    if (!onPage && wordCount_ > 0) {
      cursor_ = -1;
      for (uint8_t n = 0; n < wordCount_; ++n) {
        const uint8_t i = cursorToEnd_ ? static_cast<uint8_t>(wordCount_ - 1 - n) : n;
        if (glossable(words_[i].token)) {
          cursor_ = words_[i].token;
          break;
        }
      }
      cursorToEnd_ = false;
      if (cursor_ >= 0) platform::log("reader word %ld", static_cast<long>(cursor_));
    }
    for (uint8_t i = 0; i < wordCount_; ++i) {
      if (words_[i].token != cursor_) continue;
      t.fill(Rect{words_[i].x, i16(words_[i].y + words_[i].h - 3), words_[i].w, 3}, Paint::solid(Color::Black));
    }
  } else if (platform::kSimulator && traceNext_) {
    for (uint8_t i = 0; i < wordCount_; ++i) {
      const WordBox& w = words_[i];
      platform::log("target word/%u %d %d", w.token, w.x + w.w / 2, w.y + w.h / 2);
    }
  }
  (void)screen;
}

// A box over the half of the page the word is not in.
void ReaderScreen::drawGloss(app::UiScreen& screen, const Rect area) {
  freeink::ui::DisplayTarget& t = app_.target();
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  uint16_t sentenceId = pk::kNone16;
  const uint16_t lemmaId = lemmaOf(static_cast<uint16_t>(glossToken_), &sentenceId);
  pk::Lemma lemma;
  if (!pack.lemma(lemmaId, lemma)) return;
  int16_t wordY = area.y;
  for (uint8_t i = 0; i < wordCount_; ++i) {
    if (words_[i].token == glossToken_) wordY = words_[i].y;
  }
  const int16_t boxH = i16(area.height / 2);
  const bool atTop = wordY > area.y + area.height / 2;
  const Rect box{i16(area.x - 4), atTop ? area.y : i16(area.bottom() - boxH), i16(area.width + 8), boxH};
  t.fill(box, Paint::solid(Color::White));
  t.stroke(box, Paint::solid(Color::Black), 2);
  Column col = columnIn(Rect{i16(box.x + 12), i16(box.y + 10), i16(box.width - 24), i16(box.height - 20)});
  const bool hidden = lemma.reg == pk::Register::Vulgar && !app_.profile().showVulgar;
  const App::DeckState deck = hidden ? App::DeckState::None : app_.deckState(lemmaId);

  char text[96];
  formatLemmaLabel(lemma, text, sizeof text);
  drawText(t, col, text, FontRole::Label, size, 1, 4);
  // The star sits right of the headword, where a thumb finds it.
  const int16_t starW = 96;
  Column head = col;
  head.width = i16(col.width - starW);
  formatHeadword(pack, lemma, true, text, sizeof text);
  drawText(t, head, text, FontRole::SpanishEmphasis, size, 2, 0);
  starRect_ = Rect{};
  if (deck == App::DeckState::Open || deck == App::DeckState::Starred) {
    const Rect star{i16(col.x + col.width - starW), col.y, starW, i16(head.y - col.y + 8)};
    starRect_ = star;
    drawIcon(t, Rect{star.x, star.y, 32, star.height},
             deck == App::DeckState::Starred ? icons::kStar24 : icons::kStarOutline24);
    t.text(Rect{i16(star.x + 30), star.y, i16(starW - 30), star.height},
           tr(deck == App::DeckState::Starred ? Str::DeckShortIn : Str::DeckShortAdd), styled(kSlotSmall));
    if (!app_.keyDevice() && platform::kSimulator && traceNext_) {
      platform::log("target gloss/star %d %d", star.x + star.width / 2, star.y + star.height / 2);
    }
  }
  col.y = head.y;
  drawText(t, col, pack.str(lemma.pron), FontRole::Respelling, size, 1, 6);
  if (!hidden) drawText(t, col, pack.str(lemma.en), FontRole::EnglishGloss, size, 2, 6);
  if (hidden) drawText(t, col, pack.str(lemma.note), FontRole::EnglishTranslation, size, 3, 6);
  if (deck == App::DeckState::Learnt) drawTextIn(t, col, tr(Str::DeckLearnt), font(FontRole::ChromeSmall), 1, 6);
  if (deck == App::DeckState::Starred) drawTextIn(t, col, tr(Str::DeckIn), font(FontRole::ChromeSmall), 2, 6);
  // The sentence's English, under a rule.
  pk::Sentence sentence;
  if (pack.sentence(sentenceId, sentence)) {
    drawRule(t, col, 6);
    drawText(t, col, pack.str(sentence.en), FontRole::EnglishTranslation, size, 3, 0);
  }
  platform::log("gloss %s %s", pack.str(lemma.es),
                hidden                            ? "hidden"
                : deck == App::DeckState::Starred ? "starred"
                : deck == App::DeckState::Learnt  ? "learnt"
                : deck == App::DeckState::Open    ? "open"
                                                  : "none");
  (void)screen;
}

// Touch: the English of one sentence, in a box at the bottom.
void ReaderScreen::drawEnglish(app::UiScreen& screen, const Rect area) {
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  core::reader::WordRef ref;
  pk::Sentence sentence;
  if (!core::reader::findWord(app_.pack(), record_, static_cast<uint16_t>(englishToken_), ref) ||
      !app_.pack().sentence(ref.sentence, sentence)) {
    return;
  }
  const int16_t boxH = i16(area.height / 3);
  const Rect box{i16(area.x - 4), i16(area.bottom() - boxH), i16(area.width + 8), boxH};
  t.fill(box, Paint::solid(Color::White));
  t.stroke(box, Paint::solid(Color::Black), 2);
  Column col = columnIn(Rect{i16(box.x + 12), i16(box.y + 10), i16(box.width - 24), i16(box.height - 20)});
  drawText(t, col, app_.pack().str(sentence.es), FontRole::SpanishText, size, 3, 6);
  drawText(t, col, app_.pack().str(sentence.en), FontRole::EnglishTranslation, size, 3, 0);
  platform::log("reader english line %u", ref.line);
  (void)screen;
}

void ReaderScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DisplayTarget& t = app_.target();
  const int16_t numberH = i16(t.lineHeight(kSlotSmall) + 2);
  body_ = screen.body();
  Rect area = inset(screen.body(), theme.margin);
  area.height = i16(area.height - numberH);
  if (laidOutHeight_ != area.height) {
    laidOutHeight_ = area.height;
    paginate(area);
    if (!opened_) app_.usage().storyOpen(key_, pageCount_);
    opened_ = true;
    logPage();
  }
  starRect_ = Rect{};
  drawPage(screen, area);
  if (glossToken_ >= 0) {
    drawGloss(screen, area);
  } else if (englishToken_ >= 0) {
    drawEnglish(screen, area);
  }
  // The page number, or on the last page what comes next.
  char text[48];
  if (page_ + 1 >= pageCount_) {
    if (record_.questionCount > 0) {
      snprintf(text, sizeof text, tr(Str::ReaderQuestionsFmt), record_.questionCount);
    } else {
      snprintf(text, sizeof text, "%s", tr(Str::ReaderTheEnd));
    }
  } else {
    snprintf(text, sizeof text, tr(Str::PageFmt), page_ + 1, pageCount_);
  }
  const Rect r = screen.takeBottom(t.lineHeight(kSlotSmall), 2);
  t.text(inset(r, theme.margin), text, styled(kSlotSmall, Color::Black, TextAlign::Right));
  traceNext_ = false;
}

void ReaderScreen::logPage() const {
  platform::log("reader %u page %u/%u", story_, page_ + 1, pageCount_);
  app_.usage().storyPage(key_, page_, pageCount_);
}

const ChoiceBar* ReaderScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  bar.cells[0].icon = &icons::kChevronLeft24;
  bar.cells[0].enabled = page_ > 0;
  bar.cells[1].label = tr(Str::English);
  const bool last = page_ + 1 >= pageCount_ && pageCount_ > 0;
  if (last) {
    bar.cells[3].label = tr(record_.questionCount > 0 ? Str::ReaderQuestions : Str::Done);
  } else {
    bar.cells[3].icon = &icons::kChevronRight24;
  }
  return &bar_;
}

bool ReaderScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  const bool last = page_ + 1 >= pageCount_ && pageCount_ > 0;
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Back:
        if (glossToken_ >= 0) {
          cell = CellSpec{};
          cell.label = tr(Str::Close);
        }
        break;
      case Key::Confirm: {
        cell = CellSpec{};
        if (glossToken_ >= 0) {
          const uint16_t lemma = lemmaOf(static_cast<uint16_t>(glossToken_));
          const App::DeckState deck = const_cast<App&>(app_).deckState(lemma);
          if (deck == App::DeckState::Open) cell.label = tr(Str::DeckShortAdd);
          if (deck == App::DeckState::Starred) cell.label = tr(Str::DeckShortRemove);
        } else {
          // Always: a window refresh for the cursor must not have to update
          // the footer.
          cell.label = tr(Str::Gloss);
        }
        break;
      }
      case Key::Left:
        cell = CellSpec{};
        cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (last && glossToken_ < 0) {
          cell.label = tr(record_.questionCount > 0 ? Str::ReaderQuestions : Str::Done);
        } else {
          cell.icon = &icons::kChevronRight24;
        }
        break;
      default:
        break;
    }
  }
  return true;
}

void ReaderScreen::turn(const int8_t dir) {
  const int next = page_ + dir;
  if (next < 0) return;
  if (next >= pageCount_) {
    toEnd();
    return;
  }
  page_ = static_cast<uint8_t>(next);
  glossToken_ = -1;
  englishToken_ = -1;
  cursorToEnd_ = dir < 0;
  traceNext_ = true;
  logPage();
  app_.clearTapFlash();
  app_.invalidateCard();
}

// Past the last page: the questions, or the reading is done.
void ReaderScreen::toEnd() {
  app_.clearTapFlash();
  if (record_.questionCount > 0) {
    app_.push(ScreenId::Quiz);
    return;
  }
  markRead(app_, story_, 0, 0);
  app_.pop();
}

void ReaderScreen::moveCursor(const int8_t dir) {
  // The next glossable word on the page in reading order; past either end,
  // the page turns.
  int16_t at = -1;
  for (uint8_t i = 0; i < wordCount_; ++i) {
    if (words_[i].token == cursor_) {
      at = i;
      if (dir < 0) break;
    }
  }
  for (int16_t i = static_cast<int16_t>(at + dir); i >= 0 && i < wordCount_; i = static_cast<int16_t>(i + dir)) {
    if (words_[i].token == cursor_ || !glossable(words_[i].token)) continue;
    // Only the underline moves: a window over the old and the new one where
    // the panel can refresh one, else a fast refresh of the frame.
    const int32_t from = cursor_;
    cursor_ = words_[i].token;
    platform::log("reader word %ld", static_cast<long>(cursor_));
    for (uint8_t k = 0; k < wordCount_; ++k) {
      const WordBox& w = words_[k];
      if (w.token != from && w.token != cursor_) continue;
      app_.invalidateWindow(Rect{w.x, i16(w.y + w.h - 3), w.w, 3});
    }
    return;
  }
  if (dir > 0 && page_ + 1 < pageCount_) turn(1);
  if (dir < 0 && page_ > 0) turn(-1);
}

void ReaderScreen::openGloss(const uint16_t token) {
  if (!glossable(token)) return;
  const uint16_t lemma = lemmaOf(token);
  pk::Lemma record;
  app_.usage().gloss(lemma, core::usage::Source::Reader,
                     app_.pack().lemma(lemma, record) ? app_.pack().str(record.es) : "");
  glossToken_ = token;
  englishToken_ = -1;
  traceNext_ = true;
  app_.clearTapFlash();
  app_.invalidateCard();
}

void ReaderScreen::closeGloss() {
  if (glossToken_ < 0 && englishToken_ < 0) return;
  glossToken_ = -1;
  englishToken_ = -1;
  traceNext_ = true;
  app_.clearTapFlash();
  app_.invalidateCard();
}

void ReaderScreen::star() {
  if (glossToken_ < 0) return;
  // The box gains or loses a line: a new screen's refresh.
  if (app_.toggleStar(lemmaOf(static_cast<uint16_t>(glossToken_)))) app_.invalidateCard();
}

void ReaderScreen::onAction(const app::ActionEvent& event) {
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == 3) turn(1);
  if (event.value == 1) {
    // The English of the last word's sentence, or the page's first.
    const int32_t token = lastTapped_ >= 0 ? lastTapped_ : wordCount_ > 0 ? words_[0].token : -1;
    if (token < 0) return;
    if (englishToken_ == token) {
      closeGloss();
      return;
    }
    glossToken_ = -1;
    englishToken_ = token;
    core::reader::WordRef ref;
    if (core::reader::findWord(app_.pack(), record_, static_cast<uint16_t>(token), ref)) {
      app_.usage().sentenceEnglish(key_, ref.sentence, core::usage::Source::Reader);
    }
    app_.clearTapFlash();
    app_.invalidateCard();
  }
}

bool ReaderScreen::onInput(const InputEvent& event) {
  if (swipe(event, freeink::ui::SwipeDir::Left)) {
    turn(1);
    return true;
  }
  if (swipe(event, freeink::ui::SwipeDir::Right)) {
    turn(-1);
    return true;
  }
  if (event.kind == InputEvent::Kind::Tap) {
    // Taps on the text are resolved here against the words drawn, so a page
    // of a hundred words costs no touch targets.
    if (!body_.contains(event.x, event.y)) return false;
    if (glossToken_ >= 0) {
      // The star; anywhere else closes the box.
      const Rect hit{i16(starRect_.x - kTapSlop), i16(starRect_.y - kTapSlop), i16(starRect_.width + 2 * kTapSlop),
                     i16(starRect_.height + 2 * kTapSlop)};
      if (!starRect_.empty() && hit.contains(event.x, event.y)) {
        star();
        return true;
      }
      closeGloss();
      return true;
    }
    for (uint8_t i = 0; i < wordCount_; ++i) {
      const WordBox& w = words_[i];
      if (event.x >= w.x - kTapSlop && event.x < w.x + w.w + kTapSlop && event.y >= w.y - kTapSlop &&
          event.y < w.y + w.h + kTapSlop) {
        lastTapped_ = w.token;
        openGloss(w.token);
        return true;
      }
    }
    closeGloss();
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Up:
      turn(-1);
      return true;
    case Key::Down:
      turn(1);
      return true;
    case Key::Left:
      if (glossToken_ >= 0) closeGloss();
      moveCursor(-1);
      return true;
    case Key::Right: {
      if (glossToken_ >= 0) closeGloss();
      const bool last = page_ + 1 >= pageCount_;
      bool atLastWord = true;
      for (uint8_t i = 0; i < wordCount_; ++i) {
        if (words_[i].token > cursor_ && glossable(words_[i].token)) atLastWord = false;
      }
      if (last && atLastWord) {
        toEnd();
      } else {
        moveCursor(1);
      }
      return true;
    }
    case Key::Confirm:
      if (glossToken_ >= 0) {
        star();
      } else if (cursor_ >= 0) {
        openGloss(static_cast<uint16_t>(cursor_));
      }
      return true;
    default:
      return false;
  }
}

void ReaderScreen::onBack() {
  if (glossToken_ >= 0 || englishToken_ >= 0) {
    closeGloss();
    return;
  }
  app_.clearTapFlash();
  app_.pop();
}

// ── Questions ────────────────────────────────────────────────────────────────

const char* QuizScreen::title() const { return tr(Str::ReaderQuestions); }

void QuizScreen::enter(const bool returning) {
  traceNext_ = true;
  if (returning) return;
  story_ = gStory;
  app_.pack().story(story_, record_);
  key_ = lib::storyKey(app_.pack(), record_);
  question_ = 0;
  chosen_ = -1;
  right_ = 0;
  done_ = record_.questionCount == 0;
  logQuestion();
}

void QuizScreen::logQuestion() const {
  pk::Question q;
  if (!app_.pack().question(record_, question_, q)) return;
  // Flows answer right or wrong by it.
  platform::log("quiz %u/%u options %u right %u", question_ + 1, record_.questionCount, q.optionCount, q.answer + 1);
}

void QuizScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  Column col = columnIn(inset(screen.body(), theme.margin));
  char text[64];
  if (done_) {
    snprintf(text, sizeof text, tr(Str::QuizScoreFmt), right_, record_.questionCount);
    drawTextIn(t, col, text, font(FontRole::ChromeTitle), 1, kGap);
    drawText(t, col, pack.str(record_.title), FontRole::SpanishEmphasis, size, 2, 4);
    drawText(t, col, pack.str(record_.titleEn), FontRole::EnglishTranslation, size, 2, kGap);
    return;
  }
  pk::Question q;
  if (!pack.question(record_, question_, q)) return;
  snprintf(text, sizeof text, tr(Str::QuestionFmt), question_ + 1, record_.questionCount);
  drawTextIn(t, col, text, font(FontRole::ChromeSmall), 1, 6);
  const bool spanish = (q.flags & pk::kQuestionSpanish) != 0;
  drawText(t, col, pack.str(q.text), spanish ? FontRole::SpanishEmphasis : FontRole::EnglishGloss, size, 3, kGap);
  const BitmapFont& optionFont = font(spanish ? FontRole::SpanishText : FontRole::EnglishGloss, size);
  const bool touch = !app_.keyDevice();
  const int16_t minRow = i16(touch ? 52 : 40);
  for (uint8_t i = 0; i < q.optionCount && i < 4; ++i) {
    const int16_t rowH = i16(optionFont.yAdvance + 14 < minRow ? minRow : optionFont.yAdvance + 14);
    if (col.room() < rowH) break;
    const Rect row{col.x, col.y, col.width, rowH};
    const Rect box{row.x, i16(row.y + (rowH - 28) / 2), 30, 28};
    t.stroke(box, Paint::solid(Color::Black), 1);
    char digit[4];
    snprintf(digit, sizeof digit, "%u", i + 1);
    t.text(box, digit, styled(kSlotBodyBold, Color::Black, TextAlign::Center));
    t.setFont(kSlotScratch, optionFont);
    TextStyle option = styled(kSlotScratch);
    option.maxLines = 2;
    t.text(Rect{i16(row.x + 40), row.y, i16(row.width - 72), rowH}, pack.str(q.options[i]), option);
    if (chosen_ >= 0 && i == q.answer) {
      drawIcon(t, Rect{i16(row.right() - 26), row.y, 24, rowH}, icons::kCheck24);
    }
    if (chosen_ >= 0 && i == chosen_ && chosen_ != q.answer) {
      drawIcon(t, Rect{i16(row.right() - 26), row.y, 24, rowH}, icons::kCross24);
    }
    t.fill(Rect{row.x, i16(row.bottom() - 2), row.width, 2}, Paint::dither(Color::LightGray));
    if (touch && chosen_ < 0) {
      screen.frame().hit(row, kActionOption, i, freeink::ui::InputTouch);
      if (platform::kSimulator && traceNext_) {
        platform::log("target quiz/option%u %d %d", i + 1, row.x + row.width / 2, row.y + row.height / 2);
      }
    }
    col.skip(rowH);
  }
  if (chosen_ >= 0 && col.room() > 0) {
    col.skip(kGap);
    drawTextIn(t, col, tr(chosen_ == q.answer ? Str::Right : Str::ReaderWrong), font(FontRole::ChromeBodyBold), 1, 0);
  }
  traceNext_ = false;
}

const ChoiceBar* QuizScreen::choiceBar() const {
  if (done_) return nullptr;
  pk::Question q;
  if (!app_.pack().question(record_, question_, q)) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  for (uint8_t i = 0; i < q.optionCount && i < 4; ++i) {
    snprintf(cells_[i], sizeof cells_[i], "%u", i + 1);
    bar.cells[i].label = cells_[i];
  }
  return &bar_;
}

bool QuizScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    const Key key = app_.keys().footerCell(i).key;
    if (key == Key::Back) continue;
    out.cells[i] = CellSpec{};
    if (key == Key::Confirm) out.cells[i].label = tr(Str::Done);
  }
  return true;
}

void QuizScreen::choose(const uint8_t option) {
  pk::Question q;
  if (done_ || !app_.pack().question(record_, question_, q) || option >= q.optionCount) return;
  if (chosen_ >= 0) {
    goOn();
    return;
  }
  chosen_ = static_cast<int8_t>(option);
  if (option == q.answer) ++right_;
  app_.usage().quizAnswer(key_, question_, option, option == q.answer);
  platform::log("quiz %u/%u answer %u %s", question_ + 1, record_.questionCount, option + 1,
                option == q.answer ? "right" : "wrong");
  // Marks and a line added: a fast refresh.
  app_.invalidate();
}

void QuizScreen::goOn() {
  app_.clearTapFlash();
  if (done_) {
    app_.pop();
    app_.pop();
    return;
  }
  if (chosen_ < 0) return;
  chosen_ = -1;
  ++question_;
  traceNext_ = true;
  if (question_ >= record_.questionCount) {
    done_ = true;
    markRead(app_, story_, right_, record_.questionCount);
    platform::log("quiz %u/%u done", right_, record_.questionCount);
  } else {
    logQuestion();
  }
  app_.invalidateCard();
}

void QuizScreen::onAction(const app::ActionEvent& event) {
  if (event.action == kActionOption) {
    choose(static_cast<uint8_t>(event.value));
    return;
  }
  if (event.action == app::kActionChoice && event.value >= 0) {
    if (chosen_ >= 0 || done_) {
      goOn();
    } else {
      choose(static_cast<uint8_t>(event.value));
    }
  }
}

bool QuizScreen::onInput(const InputEvent& event) {
  // After an answer, any key goes on; at the end, back to the readings.
  if (event.kind == InputEvent::Kind::Tap && (chosen_ >= 0 || done_)) {
    const int16_t top = i16(app_.theme().statusHeight + 4);
    if (event.y < top) return false;
    goOn();
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  if (event.key == Key::Back && (chosen_ >= 0 || !done_)) return false;
  if (chosen_ >= 0 || done_) {
    goOn();
    return true;
  }
  return false;
}

}  // namespace tinta::ui
