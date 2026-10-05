#include "SpeedReadingActivity.h"

#include <FreeInkUI.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

using speedread::Word;
using speedread::WordStream;

namespace {

// The words being read; everything around them is set smaller so the band stands out.
constexpr int WORDS_FONT = TIMES_16_FONT_ID;
constexpr int CONTEXT_FONT = TIMES_14_FONT_ID;
constexpr int STATUS_FONT = TIMES_12_FONT_ID;
constexpr int HINT_FONT = TIMES_9_FONT_ID;
constexpr int SIDE_MARGIN = 24;
// Gap between the band and the notches above and below it.
constexpr int NOTCH_GAP = 3;

constexpr fui::ActionId ACTION_REWIND = 1;
constexpr fui::ActionId ACTION_TOGGLE = 2;
constexpr fui::ActionId ACTION_SKIP = 3;

// Context shown while paused: from the start of the sentence on screen, up to this many words back, to the end
// of the sentence after it, up to this many words on.
constexpr uint32_t CONTEXT_BEHIND = 40;
constexpr uint32_t CONTEXT_AHEAD = 40;

}  // namespace

SpeedReadingActivity::SpeedReadingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::shared_ptr<Epub> epub, const int spineIndex, const uint32_t startOffset)
    : Activity("SpeedReading", renderer, mappedInput),
      UiAppHost(renderer),
      epub(std::move(epub)),
      startSpine(spineIndex),
      startOffset(startOffset) {}

void SpeedReadingActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&SpeedReadingActivity::tapScreen, this, fui::RefreshHint::Fast);
  wpm = std::clamp(SETTINGS.speedReadingWpm, CrossPointSettings::SPEED_READING_MIN_WPM,
                   CrossPointSettings::SPEED_READING_MAX_WPM);

  // Reads the chapter up to the start, unzipping it first when it has no HTML cache.
  stream = makeUniqueNoThrow<WordStream>();
  if (!stream) {
    LOG_ERR("SPR", "OOM: word stream");
    mode = Mode::Failed;
  } else if (!stream->begin(epub, startSpine, startOffset)) {
    mode = Mode::Failed;
  } else if (stream->ahead() == 0) {
    mode = Mode::End;
  }

  // Opens paused on the first words, the sentence around them below, on a cleaned panel: the page underneath
  // may have been drawn in grayscale. Text is measured only while holding the render lock.
  RenderLock lock;
  computeLayout();
  if (mode == Mode::Paused) nextChunk();
  cleanWanted = true;
  publish(Frame::Kind::Screen);
  cleanWanted = false;
  lock.unlock();
  requestUpdate();
}

void SpeedReadingActivity::onExit() {
  if (mode == Mode::Playing) playedMs += millis() - playStartedMs;
  if (playedMs > 0) {
    LOG_DBG("SPR", "Read %u words in %lus over %u flashes (%u wpm), flash %ums", static_cast<unsigned>(wordsPlayed),
            playedMs / 1000, static_cast<unsigned>(flashesPlayed),
            static_cast<unsigned>(static_cast<uint64_t>(wordsPlayed) * 60000 / std::max<unsigned long>(playedMs, 1)),
            static_cast<unsigned>(refreshMs.load()));
  }
  if (SETTINGS.speedReadingWpm != wpm) {
    SETTINGS.speedReadingWpm = wpm;
    SETTINGS.saveToFile();
  }
  stream.reset();
  Activity::onExit();
}

void SpeedReadingActivity::computeLayout() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int bezelTop = 0;
  int bezelRight = 0;
  int bezelBottom = 0;
  int bezelLeft = 0;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();

  Layout& l = layout;
  l.lineHeight = renderer.getLineHeight(WORDS_FONT);
  l.contextLineHeight = renderer.getLineHeight(CONTEXT_FONT);
  l.statusLineHeight = renderer.getLineHeight(STATUS_FONT);
  l.hintLineHeight = renderer.getLineHeight(HINT_FONT);
  l.left = std::max(safe.x, bezelLeft) + SIDE_MARGIN;
  l.right = std::min(safe.x + safe.width, width - bezelRight) - SIDE_MARGIN;
  l.top = std::max(safe.y, bezelTop) + l.statusLineHeight / 2;
  l.bottom = std::min(safe.y + safe.height, height - bezelBottom) - l.statusLineHeight / 2;

  // The band sits a little above the middle, where the eye rests on a page.
  const int pad = l.lineHeight / 4;
  l.notch = l.lineHeight / 3;
  l.bandHeight = l.lineHeight + 2 * pad;
  l.bandY = l.top + (l.bottom - l.top) * 3 / 8 - l.bandHeight / 2;
  l.textY = l.bandY + pad;
  l.ruleTopY = l.bandY - NOTCH_GAP - l.notch;
  l.ruleBottomY = l.bandY + l.bandHeight + NOTCH_GAP + l.notch;
  // A little left of centre, like the recognition point within a word.
  l.fixationX = l.left + (l.right - l.left) * 3 / 8;

  l.readoutY = l.bottom - l.statusLineHeight;
  l.hintLines = renderer.getTextWidth(HINT_FONT, hintText()) <= l.right - l.left ? 1 : 2;
  l.hintY = l.readoutY - l.hintLineHeight / 2 - l.hintLines * l.hintLineHeight;
  l.contextY = l.ruleBottomY + l.contextLineHeight * 3 / 4;
  l.contextBottom = l.hintY - l.contextLineHeight / 2;
}

const char* SpeedReadingActivity::hintText() const {
  return mappedInput.hasTouch() ? tr(STR_SPEED_HINT_TOUCH) : tr(STR_SPEED_HINT_BUTTONS);
}

// --- Input and pacing (loop task) -------------------------------------------------------------------------

void SpeedReadingActivity::loop() {
  if (!handleInput()) return;
  step();
}

bool SpeedReadingActivity::handleInput() {
  const auto route = routeTouch(mappedInput);
  if (route) {
    app.clearTapFlash();
    switch (route.event.action) {
      case ACTION_REWIND:
        rewind();
        break;
      case ACTION_TOGGLE:
        togglePlay();
        break;
      case ACTION_SKIP:
        skip();
        break;
      default:
        break;
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitToBook();
    return false;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) togglePlay();
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) rewind();
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) skip();

  // The side buttons set the pace: up is faster on the X4's rocker; on edge-button boards (X3, X4 Pro) the
  // "up" button sits on the left, so it slows down.
  const int upStep = gpio.hasEdgeSideButtons() ? -PACE_STEP : PACE_STEP;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) changePace(upStep);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) changePace(-upStep);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) changePace(PACE_STEP);
  if (swipe == MappedInputManager::SwipeDir::Down) changePace(-PACE_STEP);
  return true;
}

bool SpeedReadingActivity::handleForcedRefresh() {
  screenWanted = true;
  cleanWanted = true;
  return true;
}

bool SpeedReadingActivity::handleHomeGesture() {
  exitToBook();
  return true;
}

void SpeedReadingActivity::togglePlay() {
  switch (mode) {
    case Mode::Paused:
      mode = Mode::Playing;
      playStartedMs = millis();
      // The first flash also clears the paused view.
      screenWanted = true;
      break;
    case Mode::Playing:
    case Mode::Chapter:
      mode = Mode::Paused;
      playedMs += millis() - playStartedMs;
      screenWanted = true;
      savePosition();
      LOG_DBG("SPR", "Paused: %u wpm, flash %ums, %u words in %u flashes", wpm, static_cast<unsigned>(refreshMs.load()),
              static_cast<unsigned>(wordsPlayed), static_cast<unsigned>(flashesPlayed));
      break;
    case Mode::End:
    case Mode::Failed:
      break;
  }
}

bool SpeedReadingActivity::endsSentence(const uint32_t index) const {
  const Word& word = stream->word(index);
  return (word.flags & (speedread::PARAGRAPH_END | speedread::CHAPTER_END)) ||
         speedread::trailingPause(word.text, word.length) == speedread::Pause::Sentence;
}

// Back to the start of the sentence on screen, or of the one before when the words on screen open theirs.
void SpeedReadingActivity::rewind() {
  if (!stream || (mode != Mode::Paused && mode != Mode::Playing)) return;
  const uint32_t from = chunk.count > 0 ? chunk.index : stream->cursor();
  const uint32_t first = stream->first();
  uint32_t target = from > first ? from - 1 : first;
  while (target > first && !endsSentence(target - 1)) target--;
  seek(target);
}

// On to the start of the next sentence.
void SpeedReadingActivity::skip() {
  if (!stream || (mode != Mode::Paused && mode != Mode::Playing)) return;
  stream->fill(FILL_AHEAD, false);
  const uint32_t from = chunk.count > 0 ? chunk.index : stream->cursor();
  for (uint32_t index = std::max(from, stream->first()) + 1; index < stream->last(); index++) {
    if (endsSentence(index - 1)) {
      seek(index);
      return;
    }
  }
}

void SpeedReadingActivity::seek(const uint32_t index) {
  stream->moveTo(index);
  if (mode == Mode::Paused) {
    replan = true;
    screenWanted = true;
  } else {
    // The next flash comes from here, as soon as the panel is free.
    chunk.dwellMs = 0;
  }
}

void SpeedReadingActivity::changePace(const int delta) {
  const int next = std::clamp<int>(wpm + delta, CrossPointSettings::SPEED_READING_MIN_WPM,
                                   CrossPointSettings::SPEED_READING_MAX_WPM);
  if (next == wpm) return;
  wpm = static_cast<uint16_t>(next);
  // The status line shows the pace: repainted at once when paused, with the next flash when playing.
  screenWanted = true;
}

const Word* SpeedReadingActivity::currentWord() const {
  // The words on screen, or the chapter about to start.
  if (!stream) return nullptr;
  const uint32_t index = mode == Mode::Chapter || chunk.count == 0 ? stream->cursor() : chunk.index;
  return stream->holds(index) ? &stream->word(index) : nullptr;
}

// Saved on pause and at each chapter (never per flash, which would write the SD card several times a second),
// so a sleep from here reopens the book at the words last read.
void SpeedReadingActivity::savePosition() const {
  if (const Word* word = currentWord()) EpubReaderUtils::saveProgress(*epub, word->spine, 0, 0, word->offset);
}

void SpeedReadingActivity::exitToBook() {
  if (const Word* word = currentWord()) {
    ProgressChangeResult position;
    position.spineIndex = word->spine;
    position.hasVisibleTextOffset = true;
    position.visibleTextOffset = word->offset;
    setResult(std::move(position));
  } else {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
  }
  finish();
}

bool SpeedReadingActivity::flashDue(const uint32_t now) const {
  // The next flash starts early by the panel's refresh time, so the words on screen stay their full time.
  const uint32_t refresh = refreshMs.load();
  const uint32_t wait = chunk.dwellMs > refresh ? chunk.dwellMs - refresh : 0;
  return now - shownAtMs.load() >= wait;
}

void SpeedReadingActivity::step() {
  if (!stream) return;
  const uint32_t now = millis();
  const bool shown = frameSeq == shownSeq.load();

  if (mode == Mode::Playing) {
    stream->fill(FILL_AHEAD, /*crossChapters=*/false);
    if (stream->ahead() == 0) {
      // The chapter's last words keep their time before the next chapter opens.
      if (!shown || !flashDue(now)) return;
      if (stream->atChapterEnd()) {
        // May unzip the next chapter: a second or two on a long one, behind the chapter's last words.
        stream->fill(FILL_AHEAD, /*crossChapters=*/true);
        if (stream->ahead() > 0) {
          mode = Mode::Chapter;
          screenWanted = true;
          cleanWanted = true;
          savePosition();
        }
      }
      if (stream->ahead() == 0) {
        mode = stream->status() == WordStream::Status::Failed ? Mode::Failed : Mode::End;
        playedMs += now - playStartedMs;
        screenWanted = true;
      }
    }
  } else if (mode == Mode::Chapter && !screenWanted && shown && now - shownAtMs.load() >= CHAPTER_CARD_MS) {
    // The title card has had its time; the next flash replaces it in the band.
    mode = Mode::Playing;
    chunk.dwellMs = 0;
  }

  const bool flash = mode == Mode::Playing && shown && flashDue(now) && stream->ahead() > 0;
  // While playing, a repaint waits for the next flash rather than holding the words on screen longer.
  const bool screen = screenWanted && mode != Mode::Playing;
  if (!flash && !screen) return;

  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;  // the panel is still refreshing; try again next pass
  if (flash) {
    if (!nextChunk()) return;
    publish(screenWanted ? Frame::Kind::Screen : Frame::Kind::Flash);
    wordsPlayed += chunk.count;
    flashesPlayed++;
  } else {
    if (replan) nextChunk();
    publish(Frame::Kind::Screen);
  }
  screenWanted = false;
  cleanWanted = false;
  replan = false;
  lock.unlock();
  requestUpdate();
}

size_t SpeedReadingActivity::joinWords(const uint32_t index, const size_t count, char* out,
                                       const size_t capacity) const {
  size_t length = 0;
  out[0] = '\0';
  for (size_t i = 0; i < count && stream->holds(index + i); i++) {
    const Word& word = stream->word(static_cast<uint32_t>(index + i));
    const size_t needed = word.length + (length > 0 ? 1 : 0);
    if (length + needed >= capacity) break;
    if (length > 0) out[length++] = ' ';
    memcpy(out + length, word.text, word.length);
    length += word.length;
    out[length] = '\0';
  }
  return length;
}

bool SpeedReadingActivity::chunkFits(void* self, const size_t words) {
  auto* activity = static_cast<SpeedReadingActivity*>(self);
  char line[CHUNK_BYTES];
  activity->joinWords(activity->stream->cursor(), words, line, sizeof(line));
  return activity->renderer.getTextWidth(WORDS_FONT, line) <= activity->layout.right - activity->layout.left;
}

bool SpeedReadingActivity::nextChunk() {
  const uint32_t index = stream->cursor();
  const size_t available = std::min(stream->ahead(), MAX_CHUNK_WORDS);
  if (available == 0) return false;
  const Word* words[MAX_CHUNK_WORDS];
  for (size_t i = 0; i < available; i++) words[i] = &stream->word(static_cast<uint32_t>(index + i));
  const speedread::ChunkRules rules{wpm, refreshMs.load(), static_cast<uint8_t>(MAX_CHUNK_WORDS)};
  uint32_t dwell = 0;
  const size_t count = speedread::planChunk(words, available, rules, &SpeedReadingActivity::chunkFits, this, dwell);
  chunk = {index, static_cast<uint8_t>(count), dwell};
  stream->moveTo(index + static_cast<uint32_t>(count));
  return count > 0;
}

void SpeedReadingActivity::loadChapterInfo(const int spine) {
  if (spine == infoSpine) return;
  infoSpine = spine;
  const int tocIndex = epub->getTocIndexForSpineIndex(spine);
  infoTitle = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : std::string();
  if (infoTitle.empty()) infoTitle = epub->getTitle();
  infoSpineStart = spine > 0 ? static_cast<uint32_t>(epub->getCumulativeSpineItemSize(spine - 1)) : 0;
  const auto spineEnd = static_cast<uint32_t>(epub->getCumulativeSpineItemSize(spine));
  infoSpineSize = spineEnd > infoSpineStart ? spineEnd - infoSpineStart : 0;
  infoBookSize = static_cast<uint32_t>(epub->getBookSize());
}

void SpeedReadingActivity::publish(const Frame::Kind kind) {
  frame.kind = kind;
  frame.mode = mode;
  frame.seq = ++frameSeq;
  frame.clean = cleanWanted;
  frame.wpm = wpm;
  frame.words[0] = '\0';
  frame.pivotStart = 0;
  frame.pivotLength = 0;
  frame.context[0] = '\0';
  frame.markStart = frame.markEnd = 0;
  frame.sentenceStart = false;
  if (!stream) {
    snprintf(frame.words, sizeof(frame.words), "%s", tr(STR_SPEED_UNREADABLE));
    snprintf(frame.chapter, sizeof(frame.chapter), "%s", epub ? epub->getTitle().c_str() : "");
    return;
  }

  // Where the readout points: the words on screen, or the chapter about to start.
  uint32_t at = chunk.index;
  if (mode == Mode::Chapter || chunk.count == 0) at = stream->cursor();
  if (stream->holds(at)) {
    const Word& word = stream->word(at);
    loadChapterInfo(word.spine);
    const uint64_t done = infoSpineStart + static_cast<uint64_t>(infoSpineSize) * word.permille / 1000;
    frame.bookPercent = infoBookSize > 0 ? static_cast<uint8_t>(std::min<uint64_t>(100, done * 100 / infoBookSize)) : 0;
  }
  snprintf(frame.chapter, sizeof(frame.chapter), "%s", infoTitle.c_str());

  switch (mode) {
    case Mode::Chapter:
      // The title card.
      snprintf(frame.words, sizeof(frame.words), "%s", infoTitle.c_str());
      return;
    case Mode::End:
      snprintf(frame.words, sizeof(frame.words), "%s", tr(STR_END_OF_BOOK));
      return;
    case Mode::Failed:
      snprintf(frame.words, sizeof(frame.words), "%s", tr(STR_SPEED_UNREADABLE));
      return;
    case Mode::Paused:
    case Mode::Playing:
      break;
  }
  if (chunk.count == 0 || !stream->holds(chunk.index)) return;
  joinWords(chunk.index, chunk.count, frame.words, sizeof(frame.words));
  if (chunk.count == 1) {
    const auto pivot = speedread::pivotOf(frame.words, strlen(frame.words));
    frame.pivotStart = pivot.start;
    frame.pivotLength = pivot.length;
  }
  frame.sentenceStart = chunk.index == stream->first() || endsSentence(chunk.index - 1);
  if (mode == Mode::Paused) buildContext();
}

void SpeedReadingActivity::buildContext() {
  stream->fill(FILL_AHEAD, false);
  const uint32_t first = stream->first();
  // Back to the sentence's start, keeping at least half the buffer for the words on screen and after them.
  uint32_t start = chunk.index;
  size_t behindBytes = 0;
  while (start > first && chunk.index - start < CONTEXT_BEHIND && !endsSentence(start - 1)) {
    behindBytes += stream->word(start - 1).length + 1;
    if (behindBytes >= sizeof(frame.context) / 2) break;
    start--;
  }
  const uint32_t chunkEnd = chunk.index + chunk.count;
  uint32_t end = chunkEnd;
  while (end < stream->last() && end - chunkEnd < CONTEXT_AHEAD && !endsSentence(end - 1)) end++;

  size_t length = 0;
  for (uint32_t index = start; index < end; index++) {
    const Word& word = stream->word(index);
    if (length + word.length + 2 >= sizeof(frame.context)) break;
    if (index == chunk.index) frame.markStart = static_cast<uint16_t>(length);
    memcpy(frame.context + length, word.text, word.length);
    length += word.length;
    if (index + 1 == chunkEnd) frame.markEnd = static_cast<uint16_t>(length);
    frame.context[length++] = (word.flags & speedread::PARAGRAPH_END) ? '\n' : ' ';
  }
  frame.context[length] = '\0';
  if (frame.markEnd < frame.markStart) frame.markEnd = static_cast<uint16_t>(length);
}

// --- Drawing (render task) --------------------------------------------------------------------------------

void SpeedReadingActivity::tapScreen(UiScreen& screen, void* user) {
  static_cast<SpeedReadingActivity*>(user)->buildTapZones(screen);
}

// Touch: the left third rewinds, the middle plays and pauses, the right third skips. Nothing is drawn.
void SpeedReadingActivity::buildTapZones(UiScreen& screen) {
  const fui::Rect area = screen.frame().device().screen();
  const auto third = static_cast<int16_t>(area.width / 3);
  const fui::TapZone zones[] = {
      {fui::Rect{area.x, area.y, third, area.height}, ACTION_REWIND},
      {fui::Rect{static_cast<int16_t>(area.x + third), area.y, static_cast<int16_t>(area.width - 2 * third),
                 area.height},
       ACTION_TOGGLE},
      {fui::Rect{static_cast<int16_t>(area.x + area.width - third), area.y, third, area.height}, ACTION_SKIP},
  };
  fui::TapZonesProps props;
  props.zones = zones;
  props.count = static_cast<uint8_t>(std::size(zones));
  fui::tapZones(screen.frame(), area, props);
}

int SpeedReadingActivity::cleanInterval() const { return std::max(SETTINGS.getRefreshFrequency(), 4); }

void SpeedReadingActivity::noteRefresh(const unsigned long elapsedMs) {
  const auto sample = static_cast<uint32_t>(std::clamp<unsigned long>(elapsedMs, 50, 3000));
  refreshMs.store((refreshMs.load() * 3 + sample) / 4);
}

void SpeedReadingActivity::render(RenderLock&&) {
  // Only frames the loop published are drawn. A repaint asked for elsewhere (USB plugged, say) would show
  // the same thing again and hold up the next flash.
  if (frame.seq == renderedSeq) return;
  renderedSeq = frame.seq;
  const unsigned long started = millis();
  const int bandWidth = layout.right - layout.left;

  if (frame.kind == Frame::Kind::Flash) {
    const int every = cleanInterval();
    if (flashesSinceClean >= every && (frame.sentenceStart || flashesSinceClean >= 2 * every)) {
      // Drive the band black and back: every pixel in it goes through a full transition, which wipes the
      // ghosts of earlier flashes. The return pass is a whole-frame refresh that brings the readout up to date.
      renderer.fillRect(layout.left, layout.bandY, bandWidth, layout.bandHeight, true);
      renderer.displayWindow(layout.left, layout.bandY, bandWidth, layout.bandHeight);
      drawScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      flashesSinceClean = 0;
    } else {
      drawBand();
      renderer.displayWindow(layout.left, layout.bandY, bandWidth, layout.bandHeight);
      flashesSinceClean++;
      noteRefresh(millis() - started);
    }
  } else {
    drawScreen();
    renderUi();
    const bool clean = frame.clean || flashesSinceClean >= cleanInterval();
    renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    flashesSinceClean = clean ? 0 : flashesSinceClean + 1;
  }
  shownAtMs.store(millis());
  shownSeq.store(frame.seq);
}

void SpeedReadingActivity::drawScreen() {
  renderer.clearScreen();
  drawStatus();
  drawFixationMarks();
  drawBand();
  if (frame.mode == Mode::Paused) {
    drawContext();
    const Rect hintArea{layout.left, layout.hintY, layout.right - layout.left,
                        layout.hintLines * layout.hintLineHeight};
    UITheme::drawCenteredWrappedText(renderer, hintArea, HINT_FONT, hintText(), layout.hintLines);
  }

  const char* confirm = "";
  if (frame.mode == Mode::Paused) confirm = tr(STR_SPEED_PLAY);
  if (frame.mode == Mode::Playing || frame.mode == Mode::Chapter) confirm = tr(STR_SPEED_PAUSE);
  const bool moves = frame.mode == Mode::Paused || frame.mode == Mode::Playing;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK_TO_BOOK), confirm, moves ? tr(STR_SPEED_REWIND) : "",
                                            moves ? tr(STR_SPEED_SKIP) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// The chapter across the top; pace and place in the book across the bottom.
void SpeedReadingActivity::drawStatus() {
  const int width = layout.right - layout.left;
  if (frame.chapter[0] != '\0') {
    const std::string title = renderer.truncatedText(STATUS_FONT, frame.chapter, width);
    renderer.drawText(STATUS_FONT, layout.left, layout.top, title.c_str());
  }
  char readout[48];
  const int length = snprintf(readout, sizeof(readout), tr(STR_SPEED_WPM), frame.wpm);
  if (length > 0 && length < static_cast<int>(sizeof(readout))) {
    snprintf(readout + length, sizeof(readout) - length, "  \xC2\xB7  %u%%", static_cast<unsigned>(frame.bookPercent));
  }
  renderer.drawText(STATUS_FONT, layout.left + (width - renderer.getTextWidth(STATUS_FONT, readout)) / 2,
                    layout.readoutY, readout);
}

void SpeedReadingActivity::drawFixationMarks() {
  renderer.drawLine(layout.left, layout.ruleTopY, layout.right - 1, layout.ruleTopY);
  renderer.drawLine(layout.left, layout.ruleBottomY, layout.right - 1, layout.ruleBottomY);
  renderer.fillRect(layout.fixationX - 1, layout.ruleTopY, 2, layout.notch);
  renderer.fillRect(layout.fixationX - 1, layout.ruleBottomY - layout.notch + 1, 2, layout.notch);
}

void SpeedReadingActivity::drawBand() {
  const int bandWidth = layout.right - layout.left;
  renderer.fillRect(layout.left, layout.bandY, bandWidth, layout.bandHeight, false);
  if (frame.words[0] == '\0') return;

  if (frame.mode != Mode::Paused && frame.mode != Mode::Playing) {
    // Chapter title, end of book, or a failure: centred.
    const auto style = frame.mode == Mode::Chapter ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string text = renderer.truncatedText(WORDS_FONT, frame.words, bandWidth, style);
    const int width = renderer.getTextWidth(WORDS_FONT, text.c_str(), style);
    renderer.drawText(WORDS_FONT, layout.left + (bandWidth - width) / 2, layout.textY, text.c_str(), true, style);
    return;
  }

  const int width = renderer.getTextWidth(WORDS_FONT, frame.words);
  int x;
  if (frame.pivotLength > 0) {
    // One word: its recognition point sits on the fixation column.
    char part[CHUNK_BYTES];
    memcpy(part, frame.words, frame.pivotStart);
    part[frame.pivotStart] = '\0';
    const int before = frame.pivotStart > 0 ? renderer.getTextWidth(WORDS_FONT, part) : 0;
    memcpy(part, frame.words + frame.pivotStart, frame.pivotLength);
    part[frame.pivotLength] = '\0';
    x = layout.fixationX - before - renderer.getTextWidth(WORDS_FONT, part) / 2;
  } else {
    // A phrase: the column falls a third of the way in, where the eye lands on a short line.
    x = layout.fixationX - width * 3 / 10;
  }
  x = std::clamp(x, layout.left, std::max(layout.left, layout.right - width));
  renderer.drawText(WORDS_FONT, x, layout.textY, frame.words);
}

// The sentence around the words on screen, wrapped, with those words in bold; scrolled so they show.
void SpeedReadingActivity::drawContext() {
  const int lineHeight = layout.contextLineHeight;
  const int maxLines = (layout.contextBottom - layout.contextY) / lineHeight;
  if (maxLines <= 0 || frame.context[0] == '\0') return;
  const int maxWidth = layout.right - layout.left;
  const int space = renderer.getSpaceWidth(CONTEXT_FONT);

  int count = 0;
  int line = 0;
  int x = 0;
  int markLine = 0;
  bool breakAfter = false;
  char word[speedread::Word::TEXT_BYTES * 2];
  const char* text = frame.context;
  for (size_t i = 0; text[i] != '\0' && count < MAX_CONTEXT_WORDS;) {
    const size_t start = i;
    while (text[i] != '\0' && text[i] != ' ' && text[i] != '\n') i++;
    const size_t length = std::min(i - start, sizeof(word) - 1);
    const char separator = text[i];
    if (text[i] != '\0') i++;
    if (length == 0) continue;
    memcpy(word, text + start, length);
    word[length] = '\0';
    const bool bold = start >= frame.markStart && start < frame.markEnd;
    const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int width = renderer.getTextWidth(CONTEXT_FONT, word, style);
    if (breakAfter || (x > 0 && x + width > maxWidth)) {
      line++;
      x = 0;
    }
    breakAfter = separator == '\n';
    if (bold && start == frame.markStart) markLine = line;
    contextWords[count++] = {static_cast<uint16_t>(start), static_cast<uint8_t>(length), bold, static_cast<int16_t>(x),
                             static_cast<int16_t>(line)};
    x += width + space;
  }

  const int totalLines = line + 1;
  const int firstLine = std::clamp(markLine - maxLines / 2, 0, std::max(0, totalLines - maxLines));
  for (int n = 0; n < count; n++) {
    const ContextWord& p = contextWords[n];
    if (p.line < firstLine || p.line >= firstLine + maxLines) continue;
    memcpy(word, text + p.start, p.length);
    word[p.length] = '\0';
    renderer.drawText(CONTEXT_FONT, layout.left + p.x, layout.contextY + (p.line - firstLine) * lineHeight, word, true,
                      p.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}
