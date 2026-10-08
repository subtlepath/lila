#pragma once

#include "CompanionZipEntryMetadataValidation.h"

namespace companion {
struct DictionaryZipMembers {
  ZipEntrySpan definitions, index, info, synonyms;
  bool compressed = false, hasSynonyms = false;
};
// Names must already pass whole-archive normalization/duplicate validation.
// Session-owned base path avoids an archive-sized candidate table. Two passes
// permit arbitrary central-directory order without per-entry allocations.
class DictionaryZipSelection final {
 public:
  void begin() {
    phase = Phase::Headers;
    baseBytes = 0;
    header = false;
    spans.fill({});
    found.fill(false);
    directories.fill(false);
  }
  bool add(std::span<const uint8_t> name, const ZipEntryMetadata& entry) {
    if (phase == Phase::Headers) {
      if (!suffix(name, ".ifo", 4)) return true;
      if (header || entry.directory || entry.payload.expandedBytes > 65536 || name.size() > base.size() + 4)
        return fail();
      baseBytes = name.size() - 4;
      std::copy_n(name.begin(), baseBytes, base.begin());
      spans[2] = entry.payload;
      header = true;
      return true;
    }
    if (phase != Phase::Members) return false;
    if (name.size() < baseBytes || !std::equal(base.begin(), base.begin() + baseBytes, name.begin())) return true;
    const auto tail = name.subspan(baseBytes);
    int slot = -1;
    if (exact(tail, ".dict", 5))
      slot = 0;
    else if (exact(tail, ".idx", 4))
      slot = 1;
    else if (exact(tail, ".ifo", 4))
      slot = 2;
    else if (exact(tail, ".syn", 4))
      slot = 3;
    else if (exact(tail, ".dict.dz", 8))
      slot = 4;
    if (slot < 0) return true;
    if (found[slot] || (slot == 2 && !same(spans[2], entry.payload))) return fail();
    found[slot] = true;
    directories[slot] = entry.directory;
    spans[slot] = entry.payload;
    return true;
  }
  bool finishHeaders() {
    if (phase != Phase::Headers || !header) return fail();
    phase = Phase::Members;
    return true;
  }
  bool finishMembers(DictionaryZipMembers& output) {
    if (phase != Phase::Members || !found[1] || directories[1] || !found[2] || directories[2] ||
        (found[3] && directories[3]))
      return fail();
    const bool plain = found[0] && !directories[0];
    if (!plain && (!found[4] || directories[4])) return fail();
    output = {spans[plain ? 0 : 4], spans[1], spans[2], spans[3], !plain, found[3]};
    phase = Phase::Complete;
    return true;
  }
  std::span<const uint8_t> basePath() const { return std::span(base).first(baseBytes); }

 private:
  enum class Phase { Failed, Headers, Members, Complete };
  Phase phase = Phase::Failed;
  std::array<uint8_t, 3072> base{};
  std::array<ZipEntrySpan, 5> spans{};
  std::array<bool, 5> found{}, directories{};
  size_t baseBytes = 0;
  bool header = false;
  bool fail() {
    phase = Phase::Failed;
    return false;
  }
  static bool exact(std::span<const uint8_t> name, const char* ending, size_t count) {
    return name.size() == count && std::equal(name.begin(), name.end(), ending);
  }
  static bool suffix(std::span<const uint8_t> name, const char* ending, size_t count) {
    return name.size() >= count && exact(name.last(count), ending, count);
  }
  static bool same(const ZipEntrySpan& a, const ZipEntrySpan& b) {
    return a.offset == b.offset && a.compressedBytes == b.compressedBytes && a.expandedBytes == b.expandedBytes &&
           a.crc == b.crc && a.method == b.method && a.flags == b.flags;
  }
};
}  // namespace companion
