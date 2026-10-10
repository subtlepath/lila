#pragma once

#if LILA_TINTA
#include "CompanionCourseValidation.h"
#include "CompanionStoredCourseContinuity.h"
#include "CompanionUnboundCourseMigrationIntent.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"
#include "HalInventoryFileHash.h"

namespace companion {
// Read-only pack-pair evidence, not migration authorization. Caller owns Pack off
// stack and excludes writers. Learner references, review/backups and provenance
// require separate verification before binding or moving state.
class HalUnboundCoursePackVerification final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCoursePackVerification(std::span<uint8_t> scratch, tinta::core::pack::Pack& parser, Permission permitted,
                                   void* context)
      : scratch(scratch),
        parser(parser),
        permitted(permitted),
        context(context),
        metadata(allowed, this),
        source(*this) {}
  ~HalUnboundCoursePackVerification() { closeReaders(); }
  bool verify(const UnboundCourseMigrationIntent& input, std::string_view originalPath) {
    if (operating) return failure("reentry");
    ready = false;
    if (!validUnboundCourseMigrationIntent(input) || scratch.size() < 512 || originalPath.empty() ||
        originalPath.size() >= path.size() || originalPath.find('\0') != std::string_view::npos ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &parser, sizeof(parser)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return failure("arguments");
    selected = input;
    std::copy(originalPath.begin(), originalPath.end(), path.begin());
    path[originalPath.size()] = 0;
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && hash(path.data(), selected.request.original.manifest) &&
                 validate(path.data(), selected.request.original.manifest, originalLocale) &&
                 hash(ACTIVE_COURSE_PATH, selected.activePack) &&
                 validate(ACTIVE_COURSE_PATH, selected.activePack, activeLocale);
    if (valid)
      for (size_t i = 0; i < originalLocale.size(); ++i)
        if (lower(originalLocale[i]) != lower(activeLocale[i])) {
          valid = false;
          break;
        }
    if (valid) {
      const auto continuity =
          compareStoredCourseItemIdentities(metadata, path.data(), ACTIVE_COURSE_PATH, yield, scratch);
      valid = continuity == CourseItemContinuity::Compatible ||
              (continuity == CourseItemContinuity::MissingHistory &&
               sameLegacyCourseRecords(metadata, path.data(), ACTIVE_COURSE_PATH, scratch, yield));
    }
    if (valid)
      valid = hash(path.data(), selected.request.original.manifest) && hash(ACTIVE_COURSE_PATH, selected.activePack);
    const bool closed = releaseReaders();
    ready = valid && closed && guard() && !cancelled;
    operating = false;
    return ready || failure("pack compatibility");
  }
  bool verified(const UnboundCourseMigrationIntent& input) const {
    if (operating || !ready || input != selected) return false;
    operating = true;
    const bool allowed = guard();
    if (!allowed || cancelled) ready = false;
    operating = false;
    return ready && input == selected;
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    return releaseReaders();
  }

 private:
  class Source final : public tinta::core::pack::PackSource {
   public:
    explicit Source(HalUnboundCoursePackVerification& owner) : owner(owner) {}
    uint32_t size() const override { return length; }
    bool read(uint32_t offset, void* output, uint32_t count) override {
      if (!owner.guard() || offset > length || count > length - offset || (count && !output) ||
          !owner.metadata.read(path, offset, {static_cast<uint8_t*>(output), count}) || !owner.guard())
        return false;
      if (++reads == 32) {
        reads = 0;
        yield();
      }
      return true;
    }
    const char* path = nullptr;
    uint32_t length = 0;
    uint8_t reads = 0;

   private:
    HalUnboundCoursePackVerification& owner;
  };
  std::span<uint8_t> scratch;
  tinta::core::pack::Pack& parser;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalFile reader;
  Source source;
  UnboundCourseMigrationIntent selected;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  std::array<char, 9> originalLocale{}, activeLocale{};
  mutable bool operating = false;
  bool cancelled = false;
  mutable bool ready = false;
  bool releaseReaders() {
    ready = false;
    parser.close();
    source.length = 0;
    const bool fileClosed = !reader.isOpen() || reader.close();
    const bool metadataClosed = metadata.closeReaders();
    return fileClosed && metadataClosed;
  }
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCoursePackVerification*>(context)->guard(); }
  static void yield() { vTaskDelay(1); }
  static unsigned char lower(unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; }
  bool hash(const char* file, const ContentManifest& manifest) {
    uint64_t size = 0, length = 0;
    Digest actual{};
    if (!releaseReaders() || !guard() || metadata.stat(file, size) != FileStatus::Present || size != manifest.length ||
        !guard() || !Storage.openFileForReadReusing("COMPANION", file, reader))
      return failure("hash open");
    const bool hashed = !reader.isDirectory() && hashInventoryFile(reader, scratch, length, actual, allowed, this);
    const bool synced = hashed && reader.sync();
    const bool closed = reader.close();
    return (hashed && synced && closed && guard() && length == manifest.length && actual == manifest.contentHash) ||
           failure("hash");
  }
  bool validate(const char* file, const ContentManifest& manifest, std::array<char, 9>& locale) {
    parser.close();
    source.path = file;
    source.length = manifest.length;
    source.reads = 0;
    const bool valid = guard() && validateCourseCandidate(parser, source, scratch) == CourseValidationResult::Ok &&
                       parser.formatMajor() == manifest.formatVersion && guard();
    if (valid) std::copy_n(parser.locale(), locale.size(), locale.begin());
    parser.close();
    source.length = 0;
    const bool closed = metadata.closeReaders();
    return (valid && closed && guard()) || failure("full validation");
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Unbound course pack %s refused", operation);
    return false;
  }
};
}  // namespace companion
#endif
