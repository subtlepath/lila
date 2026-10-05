#pragma once

#include <Epub.h>
#include <SpeedReadingStream.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Speed reading: the book's words flash a few at a time at one place on the screen, so the eye stays still
// while the text moves through it (rapid serial visual presentation). The words are set in Times 16; the frame
// around them in smaller Times.
//
// The panel shapes how it works. While reading, only a band across the screen ever changes: the frame around
// it (fixation marks, chapter, pace) is drawn once, and each flash redraws the band alone with the fast
// differential waveform, which drives only the pixels that change and, on the X4, sends only the band over
// SPI. A flash cannot show for less than the panel takes to refresh, measured as reading goes, so at paces
// beyond that, words join into short phrases (never across punctuation) instead of the pace dropping. Fast
// refreshes leave ghosts where the same pixels keep switching: after the reader's refresh-frequency count of
// flashes the band is cleaned at the next sentence start by driving it black and back, which clears the
// ghosts without flashing the rest of the screen.
//
// Back returns to the book on the page showing the words on screen.
class SpeedReadingActivity final : public Activity, private UiAppHost {
 public:
  // Starts at the word holding visible-text offset `startOffset` of spine `spineIndex`.
  SpeedReadingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                       int spineIndex, uint32_t startOffset);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return mode == Mode::Playing || mode == Mode::Chapter; }
  bool handleForcedRefresh() override;
  bool handleHomeGesture() override;

 private:
  enum class Mode : uint8_t { Paused, Playing, Chapter, End, Failed };

  static constexpr size_t MAX_CHUNK_WORDS = 4;
  static constexpr size_t CHUNK_BYTES = MAX_CHUNK_WORDS * speedread::Word::TEXT_BYTES;
  static constexpr size_t TITLE_BYTES = 96;
  static constexpr size_t CONTEXT_BYTES = 768;

  // What the next render draws. The loop task writes it holding the render lock; render() reads it.
  struct Frame {
    // A whole screen, or a flash of the band alone.
    enum class Kind : uint8_t { Screen, Flash };
    Kind kind = Kind::Screen;
    Mode mode = Mode::Paused;
    uint32_t seq = 0;
    // Draw the screen with the panel's full clean waveform.
    bool clean = false;
    // The flash opens a sentence: a pause in which cleaning the band goes unnoticed.
    bool sentenceStart = false;
    char words[CHUNK_BYTES] = {};
    // A single word's recognition point (byte range in `words`); length 0 for a phrase.
    uint8_t pivotStart = 0;
    uint8_t pivotLength = 0;
    uint16_t wpm = 0;
    uint8_t bookPercent = 0;
    char chapter[TITLE_BYTES] = {};
    // Paused: the sentence around the flash, with its words at [markStart, markEnd). '\n' ends a paragraph.
    char context[CONTEXT_BYTES] = {};
    uint16_t markStart = 0;
    uint16_t markEnd = 0;
  };

  // Screen geometry in the current orientation, fixed while the activity runs.
  struct Layout {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    // Line heights of the words in the band, the paused context, the chapter and readout, and the hint.
    int lineHeight = 0;
    int contextLineHeight = 0;
    int statusLineHeight = 0;
    int hintLineHeight = 0;
    // The band a flash redraws, and the text line inside it.
    int bandY = 0;
    int bandHeight = 0;
    int textY = 0;
    // The column the eye rests on, marked by notches on the rules above and below the band.
    int fixationX = 0;
    int ruleTopY = 0;
    int ruleBottomY = 0;
    int notch = 0;
    int contextY = 0;
    int contextBottom = 0;
    // Paused: the hint, one or two lines above the readout of pace and place.
    int hintY = 0;
    int hintLines = 1;
    int readoutY = 0;
  };

  // The words on screen: stream indices [index, index + count), shown for dwellMs.
  struct Chunk {
    uint32_t index = 0;
    uint8_t count = 0;
    uint32_t dwellMs = 0;
  };

  static void tapScreen(UiScreen& screen, void* user);
  void buildTapZones(UiScreen& screen);
  void computeLayout();
  const char* hintText() const;

  // Loop task.
  bool handleInput();
  void step();
  void togglePlay();
  void rewind();
  void skip();
  void seek(uint32_t index);
  void changePace(int delta);
  void exitToBook();
  const speedread::Word* currentWord() const;
  void savePosition() const;
  bool flashDue(uint32_t now) const;
  // Plans the flash at the stream's cursor and moves past it. Holds the render lock (measures text).
  bool nextChunk();
  static bool chunkFits(void* self, size_t words);
  size_t joinWords(uint32_t index, size_t count, char* out, size_t capacity) const;
  // Fills `frame` for the current state and publishes it. Holds the render lock.
  void publish(Frame::Kind kind);
  void buildContext();
  void loadChapterInfo(int spine);
  bool endsSentence(uint32_t index) const;

  // Render task.
  void drawScreen();
  void drawStatus();
  void drawFixationMarks();
  void drawBand();
  void drawContext();
  void noteRefresh(unsigned long elapsedMs);
  int cleanInterval() const;

  std::shared_ptr<Epub> epub;
  int startSpine;
  uint32_t startOffset;
  std::unique_ptr<speedread::WordStream> stream;
  Layout layout;

  // Loop task state.
  Mode mode = Mode::Paused;
  uint16_t wpm = 0;
  Chunk chunk;
  // Requests for the next publish: a whole screen (cleaned), and a flash re-planned at the cursor.
  bool screenWanted = false;
  bool cleanWanted = false;
  bool replan = false;
  uint32_t frameSeq = 0;
  // Chapter readout for the spine on screen; reading the book's metadata touches the SD card.
  int infoSpine = -1;
  std::string infoTitle;
  uint32_t infoSpineStart = 0;
  uint32_t infoSpineSize = 0;
  uint32_t infoBookSize = 0;
  // Reading statistics for the log.
  unsigned long playStartedMs = 0;
  unsigned long playedMs = 0;
  uint32_t wordsPlayed = 0;
  uint32_t flashesPlayed = 0;

  Frame frame;

  // Written by the render task.
  std::atomic<uint32_t> shownSeq{0};
  std::atomic<uint32_t> shownAtMs{0};
  // The panel's flash time: render start to refresh end, smoothed.
  std::atomic<uint32_t> refreshMs{INITIAL_REFRESH_MS};
  uint32_t renderedSeq = 0;
  int flashesSinceClean = 0;
  // Where drawContext() places each word of the paused view.
  struct ContextWord {
    uint16_t start;
    uint8_t length;
    bool bold;
    int16_t x;
    int16_t line;
  };
  static constexpr int MAX_CONTEXT_WORDS = 96;
  ContextWord contextWords[MAX_CONTEXT_WORDS] = {};

  static constexpr uint32_t INITIAL_REFRESH_MS = 450;
  static constexpr uint32_t CHAPTER_CARD_MS = 1500;
  static constexpr int PACE_STEP = 25;
  // Words read ahead of the cursor; refilled every loop pass while playing.
  static constexpr size_t FILL_AHEAD = 48;
};
