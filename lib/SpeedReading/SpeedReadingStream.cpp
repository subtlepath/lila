#include "SpeedReadingStream.h"

#include <Arduino.h>
#include <Epub.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

namespace speedread {

namespace {

// Unzipping a chapter takes the inflater's 32 KB window (one block), ~11 KB of state and the 8 KB copy
// buffers. Below this the stream stops rather than skip chapters.
constexpr size_t ZIP_MIN_BLOCK = 34 * 1024;
constexpr size_t ZIP_MIN_FREE = 64 * 1024;

}  // namespace

WordStream::~WordStream() { end(); }

bool WordStream::begin(const std::shared_ptr<Epub>& book, const int spine, const uint32_t startOffset) {
  end();
  if (!book) return false;
  epub = book;
  spineCount = epub->getSpineItemsCount();
  ring = makeUniqueNoThrow<Word[]>(CAPACITY);
  scanner = makeUniqueNoThrow<SpineWordScanner>(static_cast<WordSink&>(*this));
  if (!ring || !scanner) {
    LOG_ERR("SPR", "OOM: word ring");
    state = Status::Failed;
    return false;
  }
  head = position = tail = 0;
  opened = false;
  state = Status::Reading;
  nextSpine = std::clamp(spine, 0, std::max(spineCount - 1, 0));
  seeking = true;
  seekOffset = startOffset;
  const unsigned long started = millis();
  fill(1, /*crossChapters=*/true);
  seeking = false;
  LOG_DBG("SPR", "Opened spine %d at %u: %u words behind, %lums", spine, static_cast<unsigned>(startOffset),
          static_cast<unsigned>(position - head), millis() - started);
  return state != Status::Failed;
}

void WordStream::end() {
  closeSpine();
  scanner.reset();
  ring.reset();
  epub.reset();
  head = position = tail = 0;
}

void WordStream::fill(size_t count, const bool crossChapters) {
  if (!ring) return;
  count = std::min(count, FILL_LIMIT);
  while (state == Status::Reading && ahead() < count) {
    if (!file) {
      if (opened && !crossChapters) return;
      if (!openNextSpine()) return;
      continue;
    }
    const int read = file.read(chunk, READ_CHUNK);
    if (read > 0) {
      scanner->write(chunk, static_cast<size_t>(read));
      continue;
    }
    if (!scanner->finish()) LOG_DBG("SPR", "Spine %d did not parse to its end", nextSpine - 1);
    closeSpine();
  }
}

void WordStream::moveTo(const uint32_t index) { position = std::clamp(index, head, tail); }

void WordStream::onWord(const Word& word, const uint32_t end) {
  if (tail - head == CAPACITY) {
    if (head == position) {
      // fill() stops FILL_LIMIT words ahead, which leaves a chunk's worth of room; never reached.
      LOG_ERR("SPR", "Word ring full");
      return;
    }
    head++;
  }
  ring[tail % CAPACITY] = word;
  tail++;
  if (seeking) {
    if (end <= seekOffset) {
      position = tail;
    } else {
      seeking = false;
    }
  }
}

bool WordStream::openNextSpine() {
  while (nextSpine < spineCount) {
    const int spine = nextSpine++;
    // Only the chapter reading starts in has words before the start.
    if (opened) seeking = false;
    if (openSpine(spine)) {
      opened = true;
      return true;
    }
    if (state == Status::Failed) return false;
  }
  state = Status::BookEnd;
  return false;
}

bool WordStream::openSpine(const int spine) {
  const std::string href = epub->getSpineItem(spine).href;
  const std::string dir = epub->getCachePath() + "/html";
  const std::string path = dir + "/" + std::to_string(spine) + ".html";
  std::string source = path;
  if (!Storage.exists(path.c_str())) {
    if (ESP.getMaxAllocHeap() < ZIP_MIN_BLOCK || ESP.getFreeHeap() < ZIP_MIN_FREE) {
      LOG_ERR("SPR", "Not enough heap to unzip spine %d (free %u, block %u)", spine,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      state = Status::Failed;
      return false;
    }
    Storage.mkdir(dir.c_str());
    const std::string temp = dir + "/.speed_" + std::to_string(spine) + ".html";
    if (Storage.exists(temp.c_str())) Storage.remove(temp.c_str());
    if (!epub->extractItemToFile(href, temp)) {
      LOG_ERR("SPR", "Could not unzip spine %d (%s); skipped", spine, href.c_str());
      return false;
    }
    // Promoted like a section build's copy: a file at the cache path is known to be complete.
    if (!Storage.rename(temp.c_str(), path.c_str())) {
      source = temp;
      tempPath = temp;
    }
  }
  if (!Storage.openFileForRead("SPR", source, file)) {
    LOG_ERR("SPR", "Could not open spine %d", spine);
    closeSpine();
    return false;
  }
  if (!scanner->begin(static_cast<uint16_t>(spine), static_cast<uint32_t>(file.size()))) {
    LOG_ERR("SPR", "OOM: XML parser");
    closeSpine();
    state = Status::Failed;
    return false;
  }
  return true;
}

void WordStream::closeSpine() {
  if (file) file.close();
  if (!tempPath.empty()) {
    Storage.remove(tempPath.c_str());
    tempPath.clear();
  }
}

}  // namespace speedread
