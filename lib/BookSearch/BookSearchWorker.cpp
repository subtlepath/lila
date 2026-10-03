#include "BookSearchWorker.h"

#include <Arduino.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace booksearch {

namespace {

// Reading a chapter out of the EPUB takes the inflater's 32 KB window (one block) and ~11 KB state, plus
// two ZIP_CHUNK buffers. Below this the search stops rather than skip chapters silently.
constexpr size_t ZIP_STREAM_MIN_BLOCK = 34 * 1024;
constexpr size_t ZIP_STREAM_MIN_FREE = 64 * 1024;
constexpr size_t ZIP_CHUNK = 4096;

}  // namespace

BookSearchWorker::BookSearchWorker() : bookMutex(xSemaphoreCreateMutex()), doneSemaphore(xSemaphoreCreateBinary()) {}

BookSearchWorker::~BookSearchWorker() {
  stop();
  if (bookMutex) vSemaphoreDelete(bookMutex);
  if (doneSemaphore) vSemaphoreDelete(doneSemaphore);
}

bool BookSearchWorker::start(const std::shared_ptr<Epub>& book, const std::string& query) {
  stop();
  state = Status::Failed;
  if (!book || !bookMutex || !doneSemaphore) return false;
  if (!scanner) scanner = makeUniqueNoThrow<SpineTextScanner>(static_cast<HitSink&>(*this));
  if (!readChunk) readChunk = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  if (!scanner || !readChunk) {
    LOG_ERR("BSW", "OOM: search scanner");
    return false;
  }
  if (!scanner->setQuery(query)) {
    state = Status::Idle;
    return false;
  }

  epub = book;
  path = resultsPath(epub->getCachePath());
  uint16_t spineCount;
  {
    // The result list may be reading section titles on the render task.
    BookLock lock(*this);
    spineCount = static_cast<uint16_t>(epub->getSpineItemsCount());
    bookBytes = static_cast<uint32_t>(epub->getBookSize());
  }

  char wanted[QUERY_MAX_BYTES + 1];
  const size_t length = std::min(query.size(), QUERY_MAX_BYTES);
  memcpy(wanted, query.data(), length);
  wanted[utf8SafeTruncateBuffer(wanted, static_cast<int>(length))] = '\0';

  ResultsHeader saved;
  if (readHeader(path, saved) && saved.spineCount == spineCount && strcmp(saved.query, wanted) == 0) {
    header = saved;
    if (header.state != SearchState::Running) {
      published = header.count;
      percent = 100;
      state = header.state == SearchState::Full ? Status::Full : Status::Complete;
      LOG_DBG("BSW", "Reusing %u results for \"%s\"", header.count, wanted);
      return true;
    }
    LOG_DBG("BSW", "Resuming \"%s\" at spine %u with %u results", wanted, header.nextSpine, header.count);
  } else {
    header = ResultsHeader{};
    header.spineCount = spineCount;
    memcpy(header.query, wanted, sizeof(wanted));
  }

  published = header.count;
  percent = 0;
  stopRequested = false;
  writeFailed = false;
  state = Status::Running;
  // Idle priority: page turns, list scrolling and rendering always preempt the search, which takes
  // whatever CPU is left. The idle task time-slices with it, so the watchdog's idle hook still runs.
  if (xTaskCreate(&BookSearchWorker::taskTrampoline, "BookSearch", TASK_STACK_BYTES, this, tskIDLE_PRIORITY, &task) !=
      pdPASS) {
    LOG_ERR("BSW", "Could not start the search task");
    task = nullptr;
    state = Status::Failed;
    return false;
  }
  return true;
}

void BookSearchWorker::stop() {
  if (!task) return;
  stopRequested = true;
  // The task signals once it has written the results header and closed the file; deleting it is then safe.
  xSemaphoreTake(doneSemaphore, portMAX_DELAY);
  vTaskDelete(task);
  task = nullptr;
}

void BookSearchWorker::taskTrampoline(void* self) {
  static_cast<BookSearchWorker*>(self)->run();
  xSemaphoreGive(static_cast<BookSearchWorker*>(self)->doneSemaphore);
  // stop() deletes this task; it never returns from its function.
  vTaskSuspend(nullptr);
}

void BookSearchWorker::run() {
  const unsigned long startMs = millis();
  const bool resume = header.nextSpine > 0 || header.count > 0;
  HalFile results = Storage.open(path.c_str(), resume ? O_RDWR : (O_RDWR | O_CREAT | O_TRUNC));
  if (!results) {
    LOG_ERR("BSW", "Cannot open %s", path.c_str());
    state = Status::Failed;
    return;
  }
  resultsFile = &results;
  resultsToSkip = header.count - header.spineFirstResult;
  writeFailed = !sync();

  Status outcome = Status::Complete;
  int16_t tag = header.tagAtNextSpine;
  for (int spine = header.nextSpine; spine < header.spineCount && !writeFailed; spine++) {
    if (stopRequested) {
      outcome = Status::Stopped;
      break;
    }
    const uint16_t spineFirst = header.spineFirstResult;
    if (!scanSpine(spine, tag)) {
      outcome = writeFailed ? Status::Failed : Status::Stopped;
      break;
    }
    if (resultsToSkip > 0) {
      // A resumed spine found fewer results than before; drop the ones it did not find again.
      header.count = static_cast<uint16_t>(header.count - resultsToSkip);
      resultsToSkip = 0;
      results.seek(ResultsHeader::SIZE + static_cast<size_t>(header.count) * ResultRecord::SIZE);
    }
    header.nextSpine = static_cast<uint16_t>(spine + 1);
    header.spineFirstResult = header.count;
    header.tagAtNextSpine = tag;
    if (header.count >= MAX_RESULTS) {
      outcome = Status::Full;
      break;
    }
    if (header.count != spineFirst && millis() - lastSyncMs >= SYNC_INTERVAL_MS && !sync()) writeFailed = true;
  }
  if (writeFailed) outcome = Status::Failed;

  header.state = outcome == Status::Complete ? SearchState::Complete
                 : outcome == Status::Full   ? SearchState::Full
                                             : SearchState::Running;
  sync();
  results.close();
  resultsFile = nullptr;
  if (outcome == Status::Complete || outcome == Status::Full) percent = 100;
  LOG_DBG("BSW", "Search \"%s\": %u results, outcome %d, %lums, stack left %u", header.query, header.count,
          static_cast<int>(outcome), millis() - startMs, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  state = outcome;
}

bool BookSearchWorker::scanSpine(const int spine, int16_t& tag) {
  std::string href;
  int16_t initialTag = tag;
  {
    BookLock lock(*this);
    href = epub->getSpineItem(spine).href;
    spineStartBytes = spine > 0 ? static_cast<uint32_t>(epub->getCumulativeSpineItemSize(spine - 1)) : 0;
    // The spine's own table-of-contents entry (or the last one before it), then the entries that point
    // into it by id, so each result names the section it is in.
    const int tocStart = epub->getTocIndexForSpineIndex(spine);
    if (tocStart >= 0) initialTag = static_cast<int16_t>(tocStart);
    anchorCount = 0;
    const int tocCount = epub->getTocItemsCount();
    for (int i = std::max(tocStart, 0); i < tocCount && anchorCount < MAX_ANCHORS; i++) {
      const auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spine) {
        if (i > tocStart) break;
        continue;
      }
      if (!entry.anchor.empty()) anchors[anchorCount++] = {hashAnchorId(entry.anchor), static_cast<int16_t>(i)};
    }
  }

  currentSpine = spine;
  spineBytesFed = 0;
  if (!scanner->begin(anchors, anchorCount, initialTag)) {
    LOG_ERR("BSW", "OOM: XML parser");
    writeFailed = true;
    return false;
  }

  Pump pump(*this);
  const std::string cached = epub->getCachePath() + "/html/" + std::to_string(spine) + ".html";
  bool streamed = false;
  HalFile html;
  if (Storage.exists(cached.c_str()) && Storage.openFileForRead("BSW", cached, html)) {
    streamed = true;
    for (;;) {
      const int n = html.read(readChunk.get(), READ_CHUNK);
      if (n <= 0 || pump.write(readChunk.get(), static_cast<size_t>(n)) != static_cast<size_t>(n)) break;
    }
  } else if (ESP.getMaxAllocHeap() < ZIP_STREAM_MIN_BLOCK || ESP.getFreeHeap() < ZIP_STREAM_MIN_FREE) {
    LOG_ERR("BSW", "Not enough heap to inflate spine %d (free %u, block %u)", spine,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    scanner->end();
    writeFailed = true;
    return false;
  } else {
    streamed = epub->readItemContentsToStream(href, pump, ZIP_CHUNK, /*allowEarlyStop=*/true);
  }
  if (!streamed) LOG_ERR("BSW", "Could not read spine %d (%s); skipped", spine, href.c_str());
  if (!scanner->finish()) LOG_DBG("BSW", "Spine %d did not parse to its end", spine);
  tag = scanner->currentTag();
  return !stopRequested.load() && !writeFailed;
}

size_t BookSearchWorker::Pump::write(const uint8_t* buffer, const size_t size) {
  if (worker.stopRequested.load(std::memory_order_relaxed)) return 0;
  const size_t taken = worker.scanner->write(buffer, size);
  worker.spineBytesFed += static_cast<uint32_t>(size);
  if (worker.bookBytes > 0) {
    const uint64_t done = static_cast<uint64_t>(worker.spineStartBytes) + worker.spineBytesFed;
    worker.percent = static_cast<uint8_t>(std::min<uint64_t>(99, done * 100 / worker.bookBytes));
  }
  return taken;
}

bool BookSearchWorker::onHit(const Hit& hit) {
  if (resultsToSkip > 0) {
    resultsToSkip--;
    return true;
  }
  if (header.count >= MAX_RESULTS || writeFailed) return false;

  record.spineIndex = static_cast<uint16_t>(currentSpine);
  record.tocIndex = hit.tag;
  record.start = hit.start;
  record.end = hit.end;
  const uint64_t at = static_cast<uint64_t>(spineStartBytes) + hit.byteIndex;
  record.bookPercent = bookBytes > 0 ? static_cast<uint8_t>(std::min<uint64_t>(100, at * 100 / bookBytes)) : 0;
  strncpy(record.excerpt, hit.excerpt ? hit.excerpt : "", ResultRecord::EXCERPT_BYTES - 1);
  record.excerpt[ResultRecord::EXCERPT_BYTES - 1] = '\0';
  record.serialize(recordBytes);
  if (resultsFile->write(recordBytes, sizeof(recordBytes)) != sizeof(recordBytes)) {
    LOG_ERR("BSW", "Results write failed");
    writeFailed = true;
    return false;
  }
  header.count++;
  if (millis() - lastSyncMs >= SYNC_INTERVAL_MS && !sync()) {
    writeFailed = true;
    return false;
  }
  return header.count < MAX_RESULTS;
}

// Writes the header and flushes, which makes the results so far readable through another handle, then
// publishes their count to the screen.
bool BookSearchWorker::sync() {
  HalFile& results = *resultsFile;
  header.serialize(headerBytes);
  const size_t end = ResultsHeader::SIZE + static_cast<size_t>(header.count) * ResultRecord::SIZE;
  const bool ok =
      results.seek(0) && results.write(headerBytes, sizeof(headerBytes)) == sizeof(headerBytes) && results.seek(end);
  results.flush();
  lastSyncMs = millis();
  if (!ok) {
    LOG_ERR("BSW", "Results header write failed");
    return false;
  }
  published = header.count;
  return true;
}

}  // namespace booksearch
