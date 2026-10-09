#pragma once

#include "HalHistoricalCourseBaseline.h"

namespace companion {
enum class HistoricalCourseHistoryResult { Ok, Missing, Invalid, Busy, Corrupt, Incompatible, IoError };

// Retain off stack after heap admission. Visitors are read-only and borrow each
// verified baseline until return. Caller excludes namespace/state writers.
class HalHistoricalCourseHistory final {
 public:
  using Permission = HalHistoricalCourseBaseline::Permission;
  using Visitor = bool (*)(void*, const ContentManifest&, const char*);
  HalHistoricalCourseHistory(const Identity& generation, const Identity& course, std::span<uint8_t> scratch,
                             Permission permitted, void* context)
      : generation(generation),
        course(course),
        scratch(scratch),
        permitted(permitted),
        context(context),
        source(generation, course, scratch, permitted, context),
        metadata(permitted, context) {}
  ~HalHistoricalCourseHistory() { closeReaders(); }
  HalHistoricalCourseHistory(const HalHistoricalCourseHistory&) = delete;
  HalHistoricalCourseHistory& operator=(const HalHistoricalCourseHistory&) = delete;
  HistoricalCourseHistoryResult visit(Visitor visitor, void* visitorContext) {
    if (visiting) return HistoricalCourseHistoryResult::Busy;
    if (generation == Identity{} || course == Identity{} || scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !visitor)
      return HistoricalCourseHistoryResult::Invalid;
    visiting = true;
    matches = 0;
    auto result = scan(false, visitor, visitorContext);
    if (result == HistoricalCourseHistoryResult::Ok)
      result = matches ? scan(true, visitor, visitorContext) : HistoricalCourseHistoryResult::Missing;
    return finish(result);
  }
  HistoricalCourseHistoryResult inspect() {
    if (visiting) return HistoricalCourseHistoryResult::Busy;
    if (generation == Identity{} || course == Identity{} || scratch.size() < CONTENT_REMOVAL_RECORD_SIZE)
      return HistoricalCourseHistoryResult::Invalid;
    visiting = true;
    matches = 0;
    auto result = scan(false, nullptr, nullptr);
    if (result == HistoricalCourseHistoryResult::Ok && !matches) result = HistoricalCourseHistoryResult::Missing;
    return finish(result);
  }
  bool closeReaders() {
    const bool sourceClosed = source.closeReaders();
    const bool metadataClosed = metadata.closeReaders();
    const bool entryClosed = !entry.isOpen() || entry.close();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    return sourceClosed && metadataClosed && entryClosed && directoryClosed;
  }

 private:
  Identity generation, course, transaction{};
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalHistoricalCourseBaseline source;
  HalCourseRemovalMetadata metadata;
  HalFile directory, entry;
  std::array<char, 256> name{};
  std::array<char, 112> receiptPath{};
  ContentRemovalRecord receipt;
  uint32_t matches = 0;
  bool visiting = false;
  enum class NameKind { Other, Receipt, Stage, Invalid };
  bool guard() const { return permitted && permitted(context); }
  static uint32_t fold(uint32_t value) { return Storage.foldFilenameCodepoint(value); }
  NameKind classify(std::string_view filename) {
    static constexpr std::string_view PREFIX = "removal-done-";
    for (const auto expected : PREFIX) {
      uint32_t value = 0;
      if (filename.empty()) return NameKind::Other;
      if (!hal_filename::next(filename, value)) return NameKind::Invalid;
      if (fold(value) != fold(static_cast<uint8_t>(expected))) return NameKind::Other;
    }
    for (auto& byte : transaction) {
      byte = 0;
      for (unsigned digit = 0; digit < 2; ++digit) {
        uint32_t value = 0;
        if (!hal_filename::next(filename, value)) return NameKind::Invalid;
        value = fold(value);
        unsigned hex = 0;
        if (value >= '0' && value <= '9')
          hex = value - '0';
        else if (value >= fold('a') && value <= fold('f'))
          hex = value - fold('a') + 10;
        else
          return NameKind::Invalid;
        byte = static_cast<uint8_t>((byte << 4) | hex);
      }
    }
    if (transaction == Identity{}) return NameKind::Invalid;
    if (filename.empty()) return NameKind::Receipt;
    return hal_filename::compare(filename, ".tmp", fold) == hal_filename::Comparison::Equal ? NameKind::Stage
                                                                                            : NameKind::Invalid;
  }
  void address() {
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-done-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 32 <= 112);
    size_t at = sizeof(PREFIX) - 1;
    std::copy_n(PREFIX, at, receiptPath.begin());
    for (const auto byte : transaction) {
      receiptPath[at++] = HEX_DIGITS[byte >> 4];
      receiptPath[at++] = HEX_DIGITS[byte & 15];
    }
    receiptPath[at] = 0;
  }
  HistoricalCourseHistoryResult scan(bool validate, Visitor visitor, void* visitorContext) {
    if (!guard()) return HistoricalCourseHistoryResult::Busy;
    if (!closeReaders() || !Storage.openFileForReadReusing("COMPANION", TRANSFER_DIRECTORY, directory) || !guard() ||
        !directory.isDirectory() || !entry.prepareDirectoryEntry())
      return HistoricalCourseHistoryResult::IoError;
    uint32_t visited = 0;
    unsigned steps = 0;
    for (;;) {
      if (!guard()) return HistoricalCourseHistoryResult::Busy;
      if (entry.isOpen() && !entry.close()) return HistoricalCourseHistoryResult::IoError;
      const auto next = directory.nextEntry(entry);
      if (!guard()) return HistoricalCourseHistoryResult::Busy;
      if (next == HalDirectoryResult::Error) return HistoricalCourseHistoryResult::IoError;
      if (next == HalDirectoryResult::End) {
        if (!closeReaders()) return HistoricalCourseHistoryResult::IoError;
        if (!guard()) return HistoricalCourseHistoryResult::Busy;
        return !validate || visited == matches ? HistoricalCourseHistoryResult::Ok
                                               : HistoricalCourseHistoryResult::Corrupt;
      }
      const auto length = entry.getName(name.data(), name.size());
      if (!guard() || !length || length >= name.size() || name[length] != 0 ||
          !hal_filename::valid(std::string_view(name.data(), length)))
        return HistoricalCourseHistoryResult::Corrupt;
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias)) || !guard()) return HistoricalCourseHistoryResult::IoError;
      const auto aliasLength = strnlen(alias, sizeof(alias));
      if (aliasLength == sizeof(alias) || !hal_filename::valid(std::string_view(alias, aliasLength)))
        return HistoricalCourseHistoryResult::Corrupt;
      const auto kind = classify(std::string_view(name.data(), length));
      if (kind == NameKind::Invalid) return HistoricalCourseHistoryResult::Corrupt;
      if (kind != NameKind::Other) {
        if (entry.isDirectory()) return HistoricalCourseHistoryResult::Corrupt;
        if (kind == NameKind::Stage) return HistoricalCourseHistoryResult::Busy;
        address();
        uint64_t size = 0;
        if (metadata.stat(receiptPath.data(), size) != FileStatus::Present || size != CONTENT_REMOVAL_RECORD_SIZE ||
            !metadata.read(receiptPath.data(), 0, scratch.first(CONTENT_REMOVAL_RECORD_SIZE)) || !guard() ||
            !decodeContentRemovalRecord(scratch.first(CONTENT_REMOVAL_RECORD_SIZE), receipt) ||
            receipt.request.transaction != transaction || receipt.phase != ContentRemovalPhase::Retired)
          return HistoricalCourseHistoryResult::Corrupt;
        if (receipt.request.manifest.kind == ContentKind::Course &&
            receipt.request.manifest.logicalIdentity == course) {
          if (receipt.request.generation != generation || receipt.request.manifest.formatVersion != 1)
            return HistoricalCourseHistoryResult::Corrupt;
          if (!validate) {
            if (matches == UINT32_MAX) return HistoricalCourseHistoryResult::Corrupt;
            ++matches;
          } else {
            if (!source.open(receipt))
              return guard() ? HistoricalCourseHistoryResult::Corrupt : HistoricalCourseHistoryResult::Busy;
            const auto* manifest = source.manifest();
            const auto* path = source.path();
            if (!manifest || !path || !guard()) return HistoricalCourseHistoryResult::Busy;
            if (!visitor(visitorContext, *manifest, path)) return HistoricalCourseHistoryResult::Incompatible;
            if (!guard() || !source.path()) return HistoricalCourseHistoryResult::Busy;
            if (!source.closeReaders()) return HistoricalCourseHistoryResult::IoError;
            if (visited == UINT32_MAX) return HistoricalCourseHistoryResult::Corrupt;
            ++visited;
          }
        }
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }
  HistoricalCourseHistoryResult finish(HistoricalCourseHistoryResult result) {
    if (!closeReaders())
      result = HistoricalCourseHistoryResult::IoError;
    else if (!guard())
      result = HistoricalCourseHistoryResult::Busy;
    if (result != HistoricalCourseHistoryResult::Ok && result != HistoricalCourseHistoryResult::Missing)
      LOG_ERR("COMPANION", "Historical course history refused: %u", static_cast<unsigned>(result));
    visiting = false;
    return result;
  }
};
}  // namespace companion
