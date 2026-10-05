#pragma once

// The words around the speed reader's place in the book: a ring of words already shown (for rewinding and
// the paused view) and words read ahead, filled a few hundred bytes at a time from the chapter's unzipped
// HTML on the SD card. A chapter without one is inflated out of the EPUB into the same cache a section
// build uses, so opening the chapter in the reader afterwards skips the inflate.
//
// Heap: the ring (~8 KB) and expat's parser, held while the stream is open; the inflater (~43 KB) only
// while a chapter is unzipped. Single-threaded: call from one task.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "SpineWordScanner.h"

class Epub;

namespace speedread {

class WordStream final : private WordSink {
 public:
  enum class Status : uint8_t { Reading, BookEnd, Failed };

  static constexpr size_t CAPACITY = 192;

  WordStream() = default;
  ~WordStream();
  WordStream(const WordStream&) = delete;
  WordStream& operator=(const WordStream&) = delete;

  // Opens the book at the word containing visible-text offset `startOffset` of `spine`. The chapter's words
  // before it stay behind the cursor, so rewinding reaches back past the first word shown. False when memory
  // runs out or nothing can be read from there to the end of the book.
  bool begin(const std::shared_ptr<Epub>& book, int spine, uint32_t startOffset);
  void end();

  // Reads ahead until `count` words wait past the cursor, the book ends, or (unless `crossChapters`) the
  // chapter does. Opening a chapter that has no HTML cache yet inflates it: up to a second or two.
  void fill(size_t count, bool crossChapters);

  // Words are numbered from the start of the session; those from first() to last() (exclusive) are held.
  uint32_t first() const { return head; }
  uint32_t last() const { return tail; }
  uint32_t cursor() const { return position; }
  size_t ahead() const { return tail - position; }
  bool holds(const uint32_t index) const { return index >= head && index < tail; }
  const Word& word(const uint32_t index) const { return ring[index % CAPACITY]; }
  // first() <= index <= last().
  void moveTo(uint32_t index);

  Status status() const { return state; }
  // The words held end with a chapter, and the next one has not been opened yet.
  bool atChapterEnd() const { return state == Status::Reading && !file && opened; }

 private:
  static constexpr size_t READ_CHUNK = 128;
  // A chunk yields at most READ_CHUNK / 2 + 1 words, so filling to FILL_LIMIT ahead leaves room behind.
  static constexpr size_t FILL_LIMIT = CAPACITY - READ_CHUNK;

  void onWord(const Word& word, uint32_t end) override;
  // Opens the next spine with words; false at the end of the book or on failure (state says which).
  bool openNextSpine();
  bool openSpine(int spine);
  void closeSpine();

  std::shared_ptr<Epub> epub;
  std::unique_ptr<Word[]> ring;
  std::unique_ptr<SpineWordScanner> scanner;
  uint8_t chunk[READ_CHUNK] = {};
  HalFile file;
  // A parse-from temp copy to delete once read (the HTML cache could not be promoted).
  std::string tempPath;
  int spineCount = 0;
  int nextSpine = 0;
  bool opened = false;
  Status state = Status::Reading;

  uint32_t head = 0;
  uint32_t position = 0;
  uint32_t tail = 0;
  // While seeking the start: words ending at or before it go behind the cursor.
  bool seeking = false;
  uint32_t seekOffset = 0;
};

}  // namespace speedread
