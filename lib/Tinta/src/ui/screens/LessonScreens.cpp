#include "ui/screens/LessonScreens.h"

#include <stdio.h>
#include <string.h>

#include "app/App.h"
#include "core/lang/Charset.h"
#include "core/session/Exercise.h"
#include "core/session/Lessons.h"
#include "fonts/Strikes.h"
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
using app::App;
using app::ScreenId;
using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

enum : app::ActionId { kActionStart = app::kFirstViewAction, kActionLesson };

constexpr int16_t kGap = 10;

// The lesson LessonScreen shows: Home's or the course map's choice.
uint16_t gLesson = 0;

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

// ── Notes ────────────────────────────────────────────────────────────────────
// A note is typeset from its spans: the shared buffer (ui/TextBuffers),
// filled per note. The longest note in the course has 75 spans; a bullet
// takes two (the break and the bullet sign).

constexpr uint16_t kNoteSpanCap = kSharedSpanCap;
constexpr uint16_t kNoteRunCap = kSharedRunCap;
pk::SpanStyle gNoteStyles[kNoteSpanCap];

// Note text by style and text size.
const BitmapFont* noteFont(const pk::SpanStyle style, const TextSize size) {
  using namespace tinta::fonts;
  static const BitmapFont* const kFonts[6][kTextSizeCount] = {
      {&kHelvetica20, &kHelvetica22, &kHelvetica25},                       // Text
      {&kHelveticaBold20, &kHelveticaBold20, &kHelveticaBold25},           // Strong
      {&kTimesRoman22, &kTimesRoman25, &kTimesRoman29},                    // Spanish
      {&kTimesBold22, &kTimesBold25, &kTimesBold29},                       // SpanishStrong
      {&kHelveticaOblique20, &kHelveticaOblique22, &kHelveticaOblique22},  // Respelling
      {&kTimesItalic22, &kTimesItalic25, &kTimesItalic29},                 // NotMexican, struck through
  };
  const uint8_t s = static_cast<uint8_t>(style) < 6 ? static_cast<uint8_t>(style) : 0;
  const uint8_t z = static_cast<uint8_t>(size) < kTextSizeCount ? static_cast<uint8_t>(size) : 1;
  return kFonts[s][z];
}

bool isBreak(const tx::Span& span) { return span.length > 0 && span.text[0] == '\n'; }

// Starts a page after a break, not on the empty line it leaves.
tx::Position skipBreaks(tx::Position at, uint16_t count) {
  while (at.span < count && isBreak(sharedSpans()[at.span])) at = tx::Position{static_cast<uint16_t>(at.span + 1), 0};
  return at;
}

tx::Frame noteFrame(const Column& col) {
  tx::Frame frame;
  frame.x = col.x;
  frame.y = col.y;
  frame.width = col.width;
  frame.height = col.room();
  frame.lineGap = 4;
  return frame;
}

const char* noteKindLabel(const pk::NoteKind kind) {
  switch (kind) {
    case pk::NoteKind::Grammar:
      return tr(Str::NoteGrammar);
    case pk::NoteKind::Culture:
      return tr(Str::NoteCulture);
    case pk::NoteKind::Pronunciation:
      return tr(Str::NotePronunciation);
    case pk::NoteKind::Usage:
      return tr(Str::NoteUsage);
  }
  return "";
}

// "2/7" in the bottom right corner of the body.
void drawPageNumber(app::UiScreen& screen, const Theme& theme, uint8_t page, uint8_t count) {
  if (count <= 1) return;
  char text[16];
  snprintf(text, sizeof text, tr(Str::PageFmt), page + 1, count);
  freeink::ui::DrawTarget& t = screen.target();
  const Rect r = screen.takeBottom(t.lineHeight(kSlotSmall), 2);
  t.text(inset(r, theme.margin), text, styled(kSlotSmall, Color::Black, TextAlign::Right));
}

const char* const kPageKinds[] = {"cover", "note", "dialogue", "word", "practice"};

}  // namespace

void showLesson(App& app, const uint16_t lesson) {
  gLesson = lesson;
  app.clearTapFlash();
  app.push(ScreenId::Lesson);
}

// ── Lesson ───────────────────────────────────────────────────────────────────

const char* LessonScreen::title() const {
  char code[16];
  lessonCode(app_.pack(), lesson_, code, sizeof code);
  snprintf(title_, sizeof title_, tr(Str::LessonFmt), code);
  return title_;
}

void LessonScreen::enter(const bool returning) {
  traceNext_ = true;
  if (returning && lesson_ == gLesson) {
    logPage();
    return;
  }
  lesson_ = gLesson;
  page_ = 0;
  pageCount_ = 0;
  english_ = false;
  laidOutHeight_ = -1;
  const pk::Pack& pack = app_.pack();
  const bool showVulgar = app_.profile().showVulgar;
  pk::Lesson lesson;
  wordCount_ = 0;
  if (pack.lesson(lesson_, lesson)) {
    for (uint16_t k = 0; k < lesson.newCount && k < lesson.itemCount && wordCount_ < kMaxWords; ++k) {
      pk::Item item;
      if (!pack.item(lesson.firstItem + k, item) || item.kind != core::ItemKind::VocabRecognise) continue;
      // Vulgar words are never introduced with the setting off.
      if (!showVulgar && core::session::vulgarItem(pack, item)) continue;
      words_[wordCount_++] = lesson.firstItem + k;
    }
  }
  uint32_t items[32];
  practiceCount_ = core::session::lessonPractice(pack, lesson_, showVulgar, items, 32);
  platform::log("lesson %u %s, %u new words, %u to practise", lesson_, pack.str(lesson.title), wordCount_,
                practiceCount_);
}

void LessonScreen::addPage(const PageKind kind, const uint8_t index, const uint16_t at, const uint16_t offset) {
  if (pageCount_ >= kMaxPages) return;
  pages_[pageCount_++] = Page{kind, index, at, offset};
}

uint16_t LessonScreen::loadNote(const uint8_t index) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lesson lesson;
  pk::Note note;
  if (!pack.lesson(lesson_, lesson) || !pack.note(static_cast<uint16_t>(lesson.firstNote + index), note)) return 0;
  uint16_t count = 0;
  const auto add = [&](const char* text, uint16_t length, pk::SpanStyle style) {
    if (count >= kNoteSpanCap) return;
    tx::Span& span = sharedSpans()[count];
    span = tx::Span{};
    span.text = text;
    span.length = length;
    span.font = noteFont(
        style == pk::SpanStyle::Paragraph || style == pk::SpanStyle::Bullet ? pk::SpanStyle::Text : style, size);
    gNoteStyles[count++] = style;
  };
  for (uint16_t i = 0; i < note.spanCount; ++i) {
    pk::NoteSpan ns;
    if (!pack.noteSpan(note, i, ns)) break;
    switch (ns.style) {
      case pk::SpanStyle::Paragraph:
        add("\n\n", 2, ns.style);
        break;
      case pk::SpanStyle::Bullet:
        // The first span of a note needs no break before its bullet.
        if (i > 0) add("\n", 1, ns.style);
        add(TINTA_BULLET " ", 3, pk::SpanStyle::Text);
        break;
      default: {
        const char* text = pack.str(ns.text);
        const size_t length = strlen(text);
        add(text, static_cast<uint16_t>(length > 0xFFFF ? 0xFFFF : length), ns.style);
        break;
      }
    }
  }
  return count;
}

// The note's kind and title, above its first page.
int16_t LessonScreen::noteHead(freeink::ui::DisplayTarget& t, Column& col, const uint8_t index) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lesson lesson;
  pk::Note note;
  if (!pack.lesson(lesson_, lesson) || !pack.note(static_cast<uint16_t>(lesson.firstNote + index), note)) return 0;
  const int16_t top = col.y;
  drawText(t, col, noteKindLabel(note.kind), FontRole::Label, size, 1, 4);
  drawTextIn(t, col, pack.str(note.title), font(FontRole::ChromeTitle), 2, kGap);
  return i16(col.y - top);
}

int16_t LessonScreen::dialogueHead(freeink::ui::DisplayTarget& t, Column& col) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lesson lesson;
  pk::Story story;
  if (!pack.lesson(lesson_, lesson) || !pack.story(lesson.dialogue, story)) return 0;
  const int16_t top = col.y;
  drawText(t, col, tr(Str::DialogueLabel), FontRole::Label, size, 1, 4);
  drawText(t, col, pack.str(story.title), FontRole::SpanishEmphasis, size, 2, 0);
  drawText(t, col, pack.str(story.titleEn), FontRole::EnglishTranslation, size, 2, kGap);
  return i16(col.y - top);
}

// Whole lines of the dialogue from `first` while they fit (at least one);
// returns how many.
uint16_t LessonScreen::dialogueLines(freeink::ui::DisplayTarget& t, Column& col, const uint16_t first) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lesson lesson;
  pk::Story story;
  if (!pack.lesson(lesson_, lesson) || !pack.story(lesson.dialogue, story)) return 0;
  const auto line = [&](Column& c, const pk::StoryLine& l) {
    pk::Sentence sentence;
    if (!pack.sentence(l.sentence, sentence)) return true;
    const char* speaker = l.speaker ? pack.str(l.speaker) : tr(Str::Speaker);
    if (!drawText(t, c, speaker, FontRole::Label, size, 1, 2)) return false;
    if (!drawSentence(t, c, pack, sentence, pk::kNone16, -1, false, size, english_ ? 2 : kGap)) return false;
    if (english_ && !drawText(t, c, pack.str(sentence.en), FontRole::EnglishTranslation, size, 0, kGap)) return false;
    return true;
  };
  uint16_t n = 0;
  for (uint16_t i = first; i < story.lineCount; ++i) {
    pk::StoryLine l;
    if (!pack.storyLine(story, i, l)) break;
    Column probe = col;
    probe.dryRun = true;
    probe.whole = true;
    if (!line(probe, l)) {
      if (n > 0) break;
      // A line taller than a page: what fits of it.
      col.whole = false;
      line(col, l);
      col.whole = true;
      ++n;
      break;
    }
    if (col.dryRun) {
      col.y = probe.y;
    } else {
      line(col, l);
    }
    ++n;
  }
  return n;
}

void LessonScreen::paginate(const Rect area) {
  freeink::ui::DisplayTarget& t = app_.target();
  const pk::Pack& pack = app_.pack();
  pk::Lesson lesson;
  pageCount_ = 0;
  if (!pack.lesson(lesson_, lesson)) {
    addPage(PageKind::Cover, 0);
    return;
  }
  addPage(PageKind::Cover, 0);

  // Each note: pages until its text runs out.
  for (uint16_t n = 0; n < lesson.noteCount && pageCount_ < kMaxPages; ++n) {
    const uint16_t count = loadNote(static_cast<uint8_t>(n));
    tx::Position at{0, 0};
    bool first = true;
    while (pageCount_ < kMaxPages) {
      addPage(PageKind::Note, static_cast<uint8_t>(n), at.span, at.offset);
      Column col = columnIn(area);
      col.dryRun = true;
      if (first) noteHead(t, col, static_cast<uint8_t>(n));
      tx::Typesetter typesetter(sharedRuns(), kNoteRunCap);
      const tx::Layout& layout = typesetter.layout(sharedSpans(), count, noteFrame(col), at);
      if (layout.complete() || layout.lineCount == 0) break;
      at = skipBreaks(layout.next, count);
      if (at.span >= count) break;
      first = false;
    }
  }

  // The dialogue: whole lines per page.
  pk::Story story;
  if (lesson.dialogue != pk::kNone16 && pack.story(lesson.dialogue, story)) {
    uint16_t line = 0;
    while (line < story.lineCount && pageCount_ < kMaxPages) {
      addPage(PageKind::Dialogue, 0, line);
      Column col = columnIn(area);
      col.dryRun = true;
      if (line == 0) dialogueHead(t, col);
      const uint16_t n = dialogueLines(t, col, line);
      if (n == 0) break;
      line = static_cast<uint16_t>(line + n);
    }
  }

  for (uint8_t w = 0; w < wordCount_; ++w) addPage(PageKind::Word, w);
  addPage(PageKind::Practice, 0);
  if (page_ >= pageCount_) page_ = static_cast<uint8_t>(pageCount_ - 1);
  platform::log("lesson pages %u", pageCount_);
}

void LessonScreen::drawCover(freeink::ui::DisplayTarget& t, Column& col) {
  const pk::Pack& pack = app_.pack();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  pk::Lesson lesson;
  pk::Unit unit;
  if (!pack.lesson(lesson_, lesson)) return;
  char text[96];
  if (pack.unit(lesson.unit, unit)) {
    snprintf(text, sizeof text, tr(Str::UnitFmt), unit.number, pack.str(unit.title));
    drawText(t, col, text, FontRole::Label, size, 1, 6);
  }
  drawTextIn(t, col, pack.str(lesson.title), displayStrike(kDisplayStrikeCount - 1), 3, 2);
  drawText(t, col, pack.str(lesson.titleEn), FontRole::EnglishGloss, size, 2, kGap);
  drawRule(t, col, kGap);

  drawTextIn(t, col, tr(Str::InThisLesson), font(FontRole::ChromeBodyBold), 1, 6);
  for (uint16_t n = 0; n < lesson.noteCount; ++n) {
    pk::Note note;
    if (!pack.note(static_cast<uint16_t>(lesson.firstNote + n), note)) break;
    snprintf(text, sizeof text, TINTA_BULLET " %s", pack.str(note.title));
    drawText(t, col, text, FontRole::EnglishTranslation, size, 2, 4);
  }
  pk::Story story;
  if (lesson.dialogue != pk::kNone16 && pack.story(lesson.dialogue, story)) {
    snprintf(text, sizeof text, TINTA_BULLET " %s: %s", tr(Str::DialogueLabel), pack.str(story.titleEn));
    drawText(t, col, text, FontRole::EnglishTranslation, size, 2, 4);
  }
  // "• 11 new words": the bullet, then the formatted count after it.
  constexpr size_t kBullet = sizeof(TINTA_BULLET " ") - 1;
  snprintf(text, sizeof text, TINTA_BULLET " ");
  if (wordCount_ > 0) {
    snprintf(text + kBullet, sizeof text - kBullet, tr(Str::NewWordsFmt), wordCount_);
    drawText(t, col, text, FontRole::EnglishTranslation, size, 1, 4);
  }
  if (practiceCount_ > 0) {
    snprintf(text + kBullet, sizeof text - kBullet, tr(Str::PracticeFmt), practiceCount_);
    drawText(t, col, text, FontRole::EnglishTranslation, size, 1, kGap);
  }
}

void LessonScreen::drawNote(freeink::ui::DisplayTarget& t, Column& col, const Page& page) {
  const uint16_t count = loadNote(page.index);
  if (page.at == 0 && page.offset == 0) noteHead(t, col, page.index);
  tx::Typesetter typesetter(sharedRuns(), kNoteRunCap);
  typesetter.layout(sharedSpans(), count, noteFrame(col), tx::Position{page.at, page.offset});
  drawTypeset(t, typesetter);
  // A word quoted as not Mexican is struck through, as the review page does.
  const tx::Run* runs = typesetter.runs();
  for (uint16_t k = 0; k < typesetter.runCount(); ++k) {
    if (gNoteStyles[runs[k].span] != pk::SpanStyle::NotMexican) continue;
    const BitmapFont& f = *sharedSpans()[runs[k].span].font;
    t.fill(Rect{runs[k].x, i16(runs[k].baseline - f.ascent / 3), runs[k].width, 2}, Paint::solid(Color::Black));
  }
}

void LessonScreen::drawPractice(app::UiScreen& screen, Column& col) {
  freeink::ui::DisplayTarget& t = app_.target();
  const TextSize size = static_cast<TextSize>(app_.profile().textSize);
  drawTextIn(t, col, tr(Str::PracticeTitle), font(FontRole::ChromeTitle), 1, kGap);
  char text[64];
  snprintf(text, sizeof text, tr(Str::PracticeFmt), practiceCount_);
  drawText(t, col, text, FontRole::EnglishGloss, size, 1, kGap);
  drawText(t, col, tr(Str::PracticeText), FontRole::EnglishTranslation, size, 0, 2 * kGap);
  if (practiceCount_ == 0 || app_.keyDevice()) return;
  // Touch: a button as wide as the column.
  const app::SessionController& s = app_.session();
  const bool resume = s.active() && s.sessionLesson() == static_cast<int32_t>(lesson_);
  const int16_t h = 64;
  if (col.room() < h) return;
  const Rect button{col.x, col.y, col.width, h};
  t.fill(button, Paint::solid(Color::Black));
  TextStyle label = styled(kSlotBodyBold, Color::White, TextAlign::Center);
  t.text(button, tr(resume ? Str::Continue : Str::StartPractice), label);
  screen.frame().hit(button, kActionStart, 0, freeink::ui::InputTouch);
  if (platform::kSimulator && traceNext_) {
    platform::log("target lesson/start %d %d", button.x + button.width / 2, button.y + button.height / 2);
  }
  col.skip(h);
}

void LessonScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DisplayTarget& t = app_.target();
  const int16_t numberH = i16(t.lineHeight(kSlotSmall) + 2);
  Rect area = inset(screen.body(), theme.margin);
  area.height = i16(area.height - numberH);
  if (laidOutHeight_ != area.height) {
    laidOutHeight_ = area.height;
    paginate(area);
    if (pendingLine_ >= 0) {
      for (uint8_t p = 0; p < pageCount_; ++p) {
        if (pages_[p].kind == PageKind::Dialogue && pages_[p].at <= pendingLine_) page_ = p;
      }
      pendingLine_ = -1;
    }
    logPage();
  }
  const Page& page = pages_[page_];
  Column col = columnIn(area);
  switch (page.kind) {
    case PageKind::Cover:
      drawCover(t, col);
      break;
    case PageKind::Note:
      drawNote(t, col, page);
      break;
    case PageKind::Dialogue:
      if (page.at == 0) dialogueHead(t, col);
      col.whole = true;
      dialogueLines(t, col, page.at);
      break;
    case PageKind::Word: {
      char text[48];
      snprintf(text, sizeof text, tr(Str::NewWordFmt), page.index + 1, wordCount_);
      drawTextIn(t, col, text, font(FontRole::ChromeSmall), 1, 6);
      CardInput card;
      card.pack = &app_.pack();
      card.index = words_[page.index];
      app_.pack().item(card.index, card.item);
      card.format = core::session::Format::Flashcard;
      card.size = static_cast<TextSize>(app_.profile().textSize);
      card.touch = !app_.keyDevice();
      card.showVulgar = app_.profile().showVulgar;
      present_.load(card);
      present_.draw(screen, Rect{col.x, col.y, col.width, col.room()}, true);
      break;
    }
    case PageKind::Practice:
      drawPractice(screen, col);
      break;
  }
  drawPageNumber(screen, theme, page_, pageCount_);
  traceNext_ = false;
}

bool LessonScreen::onPractice() const { return pageCount_ > 0 && pages_[page_].kind == PageKind::Practice; }
bool LessonScreen::onDialogue() const { return pageCount_ > 0 && pages_[page_].kind == PageKind::Dialogue; }

const ChoiceBar* LessonScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar = ChoiceBar{};
  bar.cells[0].icon = &icons::kChevronLeft24;
  bar.cells[0].enabled = page_ > 0;
  if (onDialogue()) bar.cells[1].label = tr(Str::English);
  bar.cells[3].icon = &icons::kChevronRight24;
  bar.cells[3].enabled = page_ + 1 < pageCount_ || pageCount_ == 0;
  return &bar_;
}

bool LessonScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        if (onDialogue()) {
          cell.label = tr(Str::English);
        } else if (onPractice()) {
          if (practiceCount_ > 0) cell.label = tr(Str::Start);
        } else {
          cell.label = tr(Str::Next);
        }
        break;
      case Key::Left:
        cell = CellSpec{};
        if (page_ > 0) cell.icon = &icons::kChevronLeft24;
        break;
      case Key::Right:
        cell = CellSpec{};
        // Before the first build has paginated, every lesson has more pages.
        if (page_ + 1 < pageCount_ || pageCount_ == 0) cell.icon = &icons::kChevronRight24;
        break;
      default:
        break;
    }
  }
  return true;
}

void LessonScreen::logPage() const {
  if (pageCount_ == 0) return;
  const Page& p = pages_[page_];
  platform::log("lesson %u page %u/%u %s %u", lesson_, page_ + 1, pageCount_, kPageKinds[static_cast<uint8_t>(p.kind)],
                p.kind == PageKind::Dialogue ? p.at : p.index);
  // Same order as usage::PageKind.
  app_.usage().lessonPage(lesson_, page_, pageCount_, static_cast<core::usage::PageKind>(p.kind));
}

void LessonScreen::turn(const int8_t dir) {
  const int next = page_ + dir;
  if (next < 0 || next >= pageCount_) return;
  page_ = static_cast<uint8_t>(next);
  traceNext_ = true;
  logPage();
  app_.clearTapFlash();
  app_.invalidateCard();
}

void LessonScreen::toggleEnglish() {
  if (!onDialogue()) return;
  english_ = !english_;
  // Re-paginate; stay on the page holding the line this one started with.
  pendingLine_ = static_cast<int32_t>(pages_[page_].at);
  laidOutHeight_ = -1;
  platform::log("lesson english %s", english_ ? "on" : "off");
  app_.usage().dialogueEnglish(lesson_, static_cast<uint16_t>(pendingLine_), english_);
  traceNext_ = true;
  app_.clearTapFlash();
  app_.invalidateCard();
}

void LessonScreen::startPractice() {
  if (!onPractice() || practiceCount_ == 0) return;
  app::SessionController& s = app_.session();
  app_.clearTapFlash();
  if (!(s.active() && s.sessionLesson() == static_cast<int32_t>(lesson_))) {
    // Another session in progress ends here; its grades are kept.
    if (s.active()) s.end(core::usage::SessionEndHow::Left);
    if (!s.startLesson(lesson_)) return;
  }
  const ScreenId ids[] = {app_.rootId(), ScreenId::Session};
  app_.resetTo(ids, 2);
}

void LessonScreen::onAction(const app::ActionEvent& event) {
  if (event.action == kActionStart) {
    startPractice();
    return;
  }
  if (event.action != app::kActionChoice) return;
  if (event.value == 0) turn(-1);
  if (event.value == 1) toggleEnglish();
  if (event.value == 3) turn(1);
}

bool LessonScreen::onInput(const InputEvent& event) {
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
      if (onDialogue()) {
        toggleEnglish();
      } else if (onPractice()) {
        startPractice();
      } else {
        turn(1);
      }
      return true;
    default:
      return false;
  }
}

// ── Course map ───────────────────────────────────────────────────────────────

const char* CourseScreen::title() const { return tr(Str::CourseMap); }

uint16_t CourseScreen::rowCount() const {
  const pk::Pack& pack = app_.pack();
  return static_cast<uint16_t>(pack.count(pk::Section::Unit) + pack.count(pk::Section::Less));
}

bool CourseScreen::rowAt(const uint16_t r, Row& out) const {
  const pk::Pack& pack = app_.pack();
  const uint32_t units = pack.count(pk::Section::Unit);
  uint32_t at = 0;
  for (uint16_t u = 0; u < units; ++u) {
    pk::Unit unit;
    if (!pack.unit(u, unit)) return false;
    if (r == at) {
      out = Row{-1, u};
      return true;
    }
    if (r <= at + unit.lessonCount) {
      out = Row{static_cast<int32_t>(unit.firstLesson + (r - at - 1)), u};
      return true;
    }
    at += 1u + unit.lessonCount;
  }
  return false;
}

uint16_t CourseScreen::rowOfLesson(const uint16_t lesson) const {
  const uint16_t count = rowCount();
  for (uint16_t r = 0; r < count; ++r) {
    Row row;
    if (rowAt(r, row) && row.lesson == lesson) return r;
  }
  return 0;
}

void CourseScreen::enter(const bool returning) {
  traceNext_ = true;
  bar_ = ChoiceBar{};
  bar_.cells[2].icon = &icons::kChevronUp24;
  bar_.cells[3].icon = &icons::kChevronDown24;
  if (returning) {
    logCursor();
    return;
  }
  // Start on the current lesson (the last one when the course is done).
  const uint16_t count = app_.lessonCount();
  uint16_t current = app_.profile().currentLesson;
  if (count > 0 && current >= count) current = static_cast<uint16_t>(count - 1);
  cursor_ = rowOfLesson(current);
  placeCursor_ = true;
  logCursor();
}

void CourseScreen::logCursor() const {
  Row row;
  if (!rowAt(cursor_, row) || row.lesson < 0) return;
  static const char* const kStates[] = {"done", "current", "open", "locked"};
  platform::log("course cursor %ld %s", static_cast<long>(row.lesson),
                kStates[static_cast<uint8_t>(app_.lessonState(static_cast<uint16_t>(row.lesson)))]);
}

const ChoiceBar* CourseScreen::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar.cells[2].enabled = top_ > 0;
  bar.cells[3].enabled = top_ + rows_ < rowCount();
  return &bar_;
}

bool CourseScreen::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm: {
        cell = CellSpec{};
        Row row;
        if (rowAt(cursor_, row) && row.lesson >= 0 &&
            app_.lessonState(static_cast<uint16_t>(row.lesson)) != App::LessonState::Locked) {
          cell.label = tr(Str::Open);
        }
        break;
      }
      case Key::Left:
        cell = CellSpec{};
        if (top_ > 0) cell.icon = &icons::kChevronUp24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (top_ + rows_ < rowCount()) cell.icon = &icons::kChevronDown24;
        break;
      default:
        break;
    }
  }
  return true;
}

void CourseScreen::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const pk::Pack& pack = app_.pack();
  freeink::ui::DisplayTarget& t = app_.target();
  const bool touch = !app_.keyDevice();
  const int16_t rowH = theme.rowHeight;
  const int16_t available = screen.body().height;
  rows_ = static_cast<uint8_t>(available / rowH > 0 ? (available / rowH < 30 ? available / rowH : 30) : 1);
  const uint16_t count = rowCount();
  if (placeCursor_) {
    top_ = static_cast<uint16_t>(cursor_ - cursor_ % rows_);
    // The unit heading above a lesson at the top of a page.
    placeCursor_ = false;
  }
  if (cursor_ < top_ || cursor_ >= top_ + rows_) cursor_ = top_;

  const BitmapFont& titleFont = font(FontRole::SpanishText);
  for (uint8_t i = 0; i < rows_ && top_ + i < count; ++i) {
    Row row;
    if (!rowAt(static_cast<uint16_t>(top_ + i), row)) break;
    const Rect r = screen.takeTop(rowH);
    const Rect inner = inset(r, theme.margin);
    char text[96];
    if (row.lesson < 0) {
      // The unit: its number and title, ruled off.
      pk::Unit unit;
      pack.unit(row.unit, unit);
      snprintf(text, sizeof text, tr(Str::UnitFmt), unit.number, pack.str(unit.title));
      t.text(inner, text, styled(kSlotBodyBold));
      t.fill(Rect{inner.x, i16(r.bottom() - 3), inner.width, 2}, Paint::solid(Color::Black));
      continue;
    }
    const uint16_t lesson = static_cast<uint16_t>(row.lesson);
    const App::LessonState state = app_.lessonState(lesson);
    const bool focused = !touch && top_ + i == cursor_;
    const Color ink = focused ? Color::White : Color::Black;
    if (focused) t.fill(r, Paint::solid(Color::Black));

    pk::Lesson record;
    pack.lesson(lesson, record);
    char code[16];
    lessonCode(pack, lesson, code, sizeof code);
    const int16_t codeW = 56;
    t.text(Rect{inner.x, inner.y, codeW, inner.height}, code, styled(kSlotBodyBold, ink));
    // The state at the right: a check, "now", or "locked".
    const int16_t stateW = 90;
    const Rect stateRect{i16(inner.right() - stateW), inner.y, stateW, inner.height};
    if (state == App::LessonState::Done) {
      drawIcon(t, Rect{i16(stateRect.right() - 24), inner.y, 24, inner.height}, icons::kCheck24, ink);
    } else if (state == App::LessonState::Current) {
      t.text(stateRect, tr(Str::LessonNow), styled(kSlotBodyBold, ink, TextAlign::Right));
    } else if (state == App::LessonState::Locked) {
      t.text(stateRect, tr(Str::Locked), styled(kSlotSmall, ink, TextAlign::Right));
    }
    t.setFont(kSlotScratch, titleFont);
    TextStyle title = styled(kSlotScratch, ink);
    title.maxLines = 1;
    t.text(Rect{i16(inner.x + codeW), inner.y, i16(inner.width - codeW - stateW), inner.height}, pack.str(record.title),
           title);
    if (!focused) t.fill(Rect{inner.x, i16(r.bottom() - 1), inner.width, 1}, Paint::dither(Color::LightGray));
    if (touch && state != App::LessonState::Locked) {
      screen.frame().hit(r, kActionLesson, lesson, freeink::ui::InputTouch);
      if (platform::kSimulator && traceNext_) {
        platform::log("target course/l%u %d %d", lesson, r.x + r.width / 2, r.y + r.height / 2);
      }
    }
  }
  traceNext_ = false;
}

void CourseScreen::moveCursor(const int8_t dir) {
  const uint16_t count = rowCount();
  int32_t r = cursor_;
  // The next lesson row; headings are skipped.
  do {
    r += dir;
    Row row;
    if (r < 0 || r >= count) return;
    if (rowAt(static_cast<uint16_t>(r), row) && row.lesson >= 0) break;
  } while (true);
  cursor_ = static_cast<uint16_t>(r);
  if (cursor_ < top_) top_ = cursor_ + 1 >= rows_ ? static_cast<uint16_t>(cursor_ + 1 - rows_) : 0;
  if (cursor_ >= top_ + rows_) top_ = static_cast<uint16_t>(cursor_ - 1);
  logCursor();
  app_.invalidate();
}

void CourseScreen::turnPage(const int8_t dir) {
  const int32_t next = static_cast<int32_t>(top_) + dir * rows_;
  if (next < 0 && top_ == 0) return;
  if (next >= rowCount()) return;
  top_ = static_cast<uint16_t>(next < 0 ? 0 : next);
  cursor_ = top_;
  Row row;
  if (rowAt(cursor_, row) && row.lesson < 0) cursor_ = static_cast<uint16_t>(cursor_ + 1);
  traceNext_ = true;
  logCursor();
  app_.clearTapFlash();
  app_.invalidateCard();
}

void CourseScreen::open(const uint16_t lesson) {
  if (app_.lessonState(lesson) == App::LessonState::Locked) {
    platform::log("course locked %u", lesson);
    return;
  }
  showLesson(app_, lesson);
}

void CourseScreen::onAction(const app::ActionEvent& event) {
  if (event.action == kActionLesson) {
    open(static_cast<uint16_t>(event.value));
    return;
  }
  if (event.action != app::kActionChoice) return;
  if (event.value == 2) turnPage(-1);
  if (event.value == 3) turnPage(1);
}

bool CourseScreen::onInput(const InputEvent& event) {
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
    case Key::Left:
      turnPage(-1);
      return true;
    case Key::Right:
      turnPage(1);
      return true;
    case Key::Confirm: {
      Row row;
      if (rowAt(cursor_, row) && row.lesson >= 0) open(static_cast<uint16_t>(row.lesson));
      return true;
    }
    default:
      return false;
  }
}

}  // namespace tinta::ui
