#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionLegacyTintaJournal.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class LegacyTintaReadResult : uint8_t { Record, End, Invalid, IoError, Unavailable };
// Borrowed file/scratch outlive this reader; caller excludes writers and output/scratch overlap.
class HalLegacyTintaJournalReader {
 public:
  bool attach(HalFile& source, std::span<uint8_t> scratch, bool (*progress)(void*) = nullptr, void* context = nullptr) {
    ready = false;
    verified = endVerified = false;
    hashScratch = {};
    if (!source.isOpen() || source.isDirectory() || scratch.size() < LegacyTintaJournalDecoder::RECORD_SIZE)
      return failure("arguments");
    const auto length = source.fileSize64();
    if (length > LegacyTintaJournalDecoder::MAX_BYTES || (progress && !progress(context)) || !source.seek64(0))
      return failure("extent/cancel/seek");
    file = &source;
    buffer = scratch.first(std::min<size_t>(32, scratch.size() / 12) * 12);
    this->progress = progress;
    this->context = context;
    extent = static_cast<uint32_t>(length);
    offset = available = cursor = reads = 0;
    decoder = {};
    ready = true;
    return true;
  }
  bool attachVerified(HalFile& source, std::span<uint8_t> scratch, uint64_t expectedLength, const Digest& expectedHash,
                      bool (*progress)(void*) = nullptr, void* context = nullptr) {
    ready = false;
    if (expectedLength > LegacyTintaJournalDecoder::MAX_BYTES || !tinta_body_detail::nonzero(expectedHash) ||
        !source.isOpen() || source.isDirectory() || source.fileSize64() != expectedLength ||
        scratch.size() < LegacyTintaJournalDecoder::RECORD_SIZE)
      return failure("expected binding");
    uint64_t actualLength = 0;
    Digest actualHash{};
    if (!hashInventoryFile(source, scratch, actualLength, actualHash, progress, context) ||
        actualLength != expectedLength || actualHash != expectedHash || !attach(source, scratch, progress, context))
      return failure("backup binding");
    expected = expectedHash;
    hashScratch = scratch;
    verified = true;
    return true;
  }
  LegacyTintaReadResult next(LegacyTintaEntry& output) {
    if (!ready) return LegacyTintaReadResult::Unavailable;
    for (;;) {
      if (!file || !file->isOpen() || file->fileSize64() != extent) return error("extent changed");
      if (cursor == available) {
        if (offset == extent) {
          if (verified && !endVerified) {
            uint64_t length = 0;
            Digest hash{};
            if (!hashInventoryFile(*file, hashScratch, length, hash, progress, context) || length != extent ||
                hash != expected)
              return error("backup changed");
            endVerified = true;
          }
          return LegacyTintaReadResult::End;
        }
        if (progress && !progress(context)) return error("cancelled");
        const auto count = std::min<size_t>(buffer.size(), extent - offset);
        if (!file->seek64(offset) || file->read(buffer.data(), count) != static_cast<int>(count) ||
            file->fileSize64() != extent)
          return error("chunk read");
        offset += static_cast<uint32_t>(count);
        available = static_cast<uint32_t>(count);
        cursor = 0;
        if (++reads == 32) {
          reads = 0;
          vTaskDelay(1);
        }
      }
      const auto size = std::min<size_t>(LegacyTintaJournalDecoder::RECORD_SIZE, available - cursor);
      const auto result = decoder.next(buffer.subspan(cursor, size), output);
      cursor += static_cast<uint32_t>(size);
      if (result == LegacyTintaDecodeResult::Record) return LegacyTintaReadResult::Record;
      if (result == LegacyTintaDecodeResult::ZeroTail) continue;
      ready = false;
      LOG_ERR("COMPANION", "Invalid legacy Tinta journal record: %lu", static_cast<unsigned long>(decoder.count()));
      return LegacyTintaReadResult::Invalid;
    }
  }
  uint32_t count() const { return decoder.count(); }
  void detach() {
    ready = false;
    file = nullptr;
    buffer = {};
    hashScratch = {};
  }

 private:
  bool failure(const char* operation) {
    ready = false;
    LOG_ERR("COMPANION", "Legacy Tinta journal reader failed: %s", operation);
    return false;
  }
  LegacyTintaReadResult error(const char* operation) {
    failure(operation);
    return LegacyTintaReadResult::IoError;
  }
  HalFile* file = nullptr;
  std::span<uint8_t> buffer;
  std::span<uint8_t> hashScratch;
  Digest expected{};
  bool (*progress)(void*) = nullptr;
  void* context = nullptr;
  LegacyTintaJournalDecoder decoder;
  uint32_t extent = 0, offset = 0, available = 0, cursor = 0;
  uint8_t reads = 0;
  bool ready = false;
  bool verified = false, endVerified = false;
};
}  // namespace companion
