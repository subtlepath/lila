#pragma once

// Searches a whole book on a FreeRTOS task of its own, so the result list stays live and Back stops it at
// once. Each spine item streams from the unzipped HTML cache when a section build has left one, otherwise
// straight out of the EPUB, through a SpineTextScanner; results go to the SD results file
// (BookSearchResults.h). Peak heap is the inflater (~40 KB, only without an HTML cache), expat, and the
// scanner (~3 KB); nothing grows with the book or the number of results.

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

#include "BookSearchResults.h"
#include "SpineTextScanner.h"

class Epub;
class HalFile;

namespace booksearch {

class BookSearchWorker final : private HitSink {
 public:
  enum class Status : uint8_t { Idle, Running, Complete, Full, Stopped, Failed };

  BookSearchWorker();
  ~BookSearchWorker();
  BookSearchWorker(const BookSearchWorker&) = delete;
  BookSearchWorker& operator=(const BookSearchWorker&) = delete;

  // Searches for `query`, or carries on the search in the results file when it is for the same query:
  // a finished one is reused as it is, an unfinished one resumes where it stopped. False when the query
  // has nothing to search for, or memory or the SD card fail.
  bool start(const std::shared_ptr<Epub>& epub, const std::string& query);
  // Asks the task to stop and waits for it. What was found stays in the file, resumable.
  void stop();

  Status status() const { return state.load(); }
  bool running() const { return state.load() == Status::Running; }
  // Results written and synced, so the screen may read them from the file.
  uint16_t resultCount() const { return published.load(); }
  uint8_t progressPercent() const { return percent.load(); }

  // Epub's metadata lookups (spine, table of contents) share one file handle; anything else calling them
  // while the task runs must hold this.
  class BookLock {
   public:
    explicit BookLock(const BookSearchWorker& worker) : mutex(worker.bookMutex) {
      if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    }
    ~BookLock() {
      if (mutex) xSemaphoreGive(mutex);
    }
    BookLock(const BookLock&) = delete;
    BookLock& operator=(const BookLock&) = delete;

   private:
    SemaphoreHandle_t mutex;
  };

 private:
  static void taskTrampoline(void* self);
  void run();
  // False when a stop request ended the spine early.
  bool scanSpine(int spine, int16_t& tag);
  bool sync();
  bool onHit(const Hit& hit) override;

  // Feeds the scanner, counting bytes for progress and stopping the source when asked to.
  class Pump final : public Print {
   public:
    explicit Pump(BookSearchWorker& worker) : worker(worker) {}
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* buffer, size_t size) override;

   private:
    BookSearchWorker& worker;
  };

  static constexpr size_t MAX_ANCHORS = 48;
  // Results synced to the file (and so shown) at most this often.
  static constexpr uint32_t SYNC_INTERVAL_MS = 400;
  // Reads from the unzipped HTML cache; the zip path brings its own buffers.
  static constexpr size_t READ_CHUNK = 2048;
  // expat's callbacks, the scanner and the SD stack sit on this; measured in run()'s high-water log.
  static constexpr uint32_t TASK_STACK_BYTES = 6144;

  std::shared_ptr<Epub> epub;
  std::string path;
  ResultsHeader header;
  std::unique_ptr<SpineTextScanner> scanner;

  SemaphoreHandle_t bookMutex = nullptr;
  SemaphoreHandle_t doneSemaphore = nullptr;
  TaskHandle_t task = nullptr;
  std::atomic<bool> stopRequested{false};
  std::atomic<Status> state{Status::Idle};
  std::atomic<uint16_t> published{0};
  std::atomic<uint8_t> percent{0};

  // Task-side scan state.
  HalFile* resultsFile = nullptr;
  AnchorTag anchors[MAX_ANCHORS] = {};
  size_t anchorCount = 0;
  int currentSpine = 0;
  uint32_t spineStartBytes = 0;
  uint32_t spineBytesFed = 0;
  uint32_t bookBytes = 0;
  // Results a resumed spine finds again before it reaches new ones.
  uint16_t resultsToSkip = 0;
  bool writeFailed = false;
  uint32_t lastSyncMs = 0;
  // Heap-side scratch, so the task stack holds no buffers.
  std::unique_ptr<uint8_t[]> readChunk;
  ResultRecord record;
  uint8_t recordBytes[ResultRecord::SIZE] = {};
  uint8_t headerBytes[ResultsHeader::SIZE] = {};
};

}  // namespace booksearch
