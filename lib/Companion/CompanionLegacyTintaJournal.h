#pragma once

#include "CompanionTintaBody.h"

namespace companion {
enum class LegacyTintaOperation : uint8_t { Review, Undo, Flags };
enum class LegacyTintaDecodeResult : uint8_t { Record, ZeroTail, Invalid, Exhausted };
struct LegacyTintaEntry {
  uint32_t uid = 0, timestamp = 0, undoRecord = 0;
  uint16_t studyDay = 0;
  LegacyTintaOperation operation = LegacyTintaOperation::Review;
  uint8_t grade = 0, format = 0, responseQuarterSeconds = 0, flags = 0;
  bool operator==(const LegacyTintaEntry&) const = default;
};

// One record at a time; legacy timestamps and undo indices confer no distributed provenance.
class LegacyTintaJournalDecoder {
 public:
  static constexpr size_t RECORD_SIZE = 12;
  static constexpr uint32_t MAX_BYTES = 16 * 1024 * 1024;
  LegacyTintaDecodeResult next(std::span<const uint8_t> bytes, LegacyTintaEntry& output) {
    if (bytes.empty() || bytes.size() > RECORD_SIZE) return LegacyTintaDecodeResult::Invalid;
    if (bytes.size() > MAX_BYTES - consumedBytes) return LegacyTintaDecodeResult::Exhausted;
    const bool zero = std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
    if (zero) {
      ended = true;
      consumedBytes += static_cast<uint32_t>(bytes.size());
      return LegacyTintaDecodeResult::ZeroTail;
    }
    if (ended || bytes.size() != RECORD_SIZE) return LegacyTintaDecodeResult::Invalid;
    if (records == MAX_BYTES / RECORD_SIZE) return LegacyTintaDecodeResult::Exhausted;
    LegacyTintaEntry parsed;
    parsed.uid = tinta_body_detail::read(bytes, 0, 4);
    parsed.timestamp = tinta_body_detail::read(bytes, 4, 4);
    parsed.studyDay = static_cast<uint16_t>(tinta_body_detail::read(bytes, 8, 2));
    const uint8_t grade = bytes[10] & 7, code = bytes[10] >> 3, argument = bytes[11];
    if (parsed.uid == 0 || parsed.uid == UINT32_MAX) return LegacyTintaDecodeResult::Invalid;
    if (grade >= 1 && grade <= 4) {
      if (code > 9) return LegacyTintaDecodeResult::Invalid;
      parsed.grade = grade;
      parsed.format = code;
      parsed.responseQuarterSeconds = argument;
    } else if (grade == 0 && code == 1) {
      if (argument != 0 || !undoAvailable || parsed.uid != lastReviewUid) return LegacyTintaDecodeResult::Invalid;
      parsed.operation = LegacyTintaOperation::Undo;
      parsed.undoRecord = lastReview;
    } else if (grade == 0 && code == 2) {
      if (argument & ~7U) return LegacyTintaDecodeResult::Invalid;
      parsed.operation = LegacyTintaOperation::Flags;
      parsed.flags = argument;
    } else {
      return LegacyTintaDecodeResult::Invalid;
    }
    undoAvailable = parsed.operation == LegacyTintaOperation::Review;
    if (undoAvailable) {
      lastReview = records;
      lastReviewUid = parsed.uid;
    }
    ++records;
    consumedBytes += RECORD_SIZE;
    output = parsed;
    return LegacyTintaDecodeResult::Record;
  }
  uint32_t count() const { return records; }

 private:
  uint32_t records = 0, lastReview = 0, lastReviewUid = 0, consumedBytes = 0;
  bool undoAvailable = false, ended = false;
};
}  // namespace companion
