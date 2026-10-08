#pragma once

#include "CompanionReaderPreferenceEncoding.h"
#include "CompanionTintaWriter.h"

namespace companion {
// Retain off-stack with the serialized settings editor. Initialize from recovered
// settings. A partial batch requires journal replay before any derived save.
class ReaderPreferenceCapture final {
 public:
  explicit ReaderPreferenceCapture(TintaWriter& writer) : writer(writer) {}
  bool initialize(const ReaderPreferenceValues& values, std::string_view tag, std::span<const uint8_t> fontHash,
                  std::span<const uint8_t> dictionaryHash) {
    if (initialized || !encode(values, tag, fontHash, dictionaryHash)) return false;
    previous = candidate;
    previousLengths = lengths;
    ready = initialized = true;
    return true;
  }
  bool hasChanges(const ReaderPreferenceValues& values, std::string_view tag, std::span<const uint8_t> fontHash,
                  std::span<const uint8_t> dictionaryHash, bool& output) {
    if (!ready || !writer.available() || !encode(values, tag, fontHash, dictionaryHash)) return false;
    bool changed = false;
    for (size_t at = 0; at < candidate.size(); ++at) changed |= differs(at);
    output = changed;
    return true;
  }
  TintaJournalResult persist(const ReaderPreferenceValues& values, std::string_view tag,
                             std::span<const uint8_t> fontHash, std::span<const uint8_t> dictionaryHash, uint32_t day,
                             uint64_t timestamp, ClockQuality quality) {
    if (!ready || !writer.available()) return TintaJournalResult::Unavailable;
    if (!encode(values, tag, fontHash, dictionaryHash)) return TintaJournalResult::Invalid;
    for (size_t at = 0; at < candidate.size(); ++at) {
      if (!differs(at)) continue;
      const auto result = writer.recordPreference(std::span(candidate[at]).first(lengths[at]), day, timestamp, quality);
      if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) {
        ready = false;
        writer.stop();
        return result;
      }
      previous[at] = candidate[at];
      previousLengths[at] = lengths[at];
    }
    return TintaJournalResult::Ok;
  }

 private:
  bool encode(const ReaderPreferenceValues& values, std::string_view tag, std::span<const uint8_t> fontHash,
              std::span<const uint8_t> dictionaryHash) {
    for (size_t at = 0; at < candidate.size(); ++at) {
      const auto length =
          encodeReaderPreference(values, static_cast<uint8_t>(at + 1), tag, fontHash, dictionaryHash, candidate[at]);
      if (!length) return false;
      lengths[at] = static_cast<uint8_t>(length);
    }
    return true;
  }
  bool differs(size_t at) const {
    return lengths[at] != previousLengths[at] ||
           !std::equal(candidate[at].begin(), candidate[at].begin() + lengths[at], previous[at].begin());
  }
  TintaWriter& writer;
  std::array<std::array<uint8_t, 69>, 14> previous{}, candidate{};
  std::array<uint8_t, 14> previousLengths{}, lengths{};
  bool ready = false, initialized = false;
};
}  // namespace companion
