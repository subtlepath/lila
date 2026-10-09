#pragma once

#if LILA_TINTA

#include "CompanionCourseBaselinePublication.h"
#include "HalCourseBaselineImportConsentStore.h"
#include "HalCoursePackArchive.h"
#include "HalCourseValidation.h"

namespace companion {
// Admit the owner and borrowed parser off stack. Caller excludes writers and
// establishes native journal readiness and verifies fresh/Prepared recovery before
// publication. Historical verification never grants fresh approval.
class HalCourseBaselineArchiveSession final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineArchiveSession(const Identity& reader, const Identity& generation, const Identity& owner,
                                  std::span<uint8_t> scratch, tinta::core::pack::Pack& parser, Permission permitted,
                                  void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        parser(parser),
        permitted(permitted),
        context(context),
        consent(reader, generation, owner, scratch, allowed, this),
        backups(scratch, allowed, this),
        archive(scratch, allowed, this),
        metadata(allowed, this) {}
  ~HalCourseBaselineArchiveSession() { close(); }
  HalCourseBaselineArchiveSession(const HalCourseBaselineArchiveSession&) = delete;
  HalCourseBaselineArchiveSession& operator=(const HalCourseBaselineArchiveSession&) = delete;
  bool verify(const CourseBaselinePublicationRecord& request) {
    if (operating) return failure("reentry");
    if (!valid(request)) return failure("context/workspace");
    selected = request;
    operating = true;
    bool verified = evidence() && archive.open(selected.request.manifest.logicalIdentity,
                                               selected.request.manifest.contentHash) == CourseArchiveResult::Ok;
    if (verified) {
      const auto* manifest = archive.manifest();
      const auto* path = archive.path();
      CourseCandidateDetails details;
      verified = manifest && *manifest == selected.request.manifest && path && guard() &&
                 validateStagedCourse(path, parser, scratch, details) &&
                 details.major == selected.request.manifest.formatVersion && guard();
    }
    return finish(verified, "historical evidence");
  }
  bool verifyPrepared(const CourseBaselinePublicationRecord& request, bool recovering) {
    if (operating) return failure("reentry");
    if (!valid(request) || request.phase != CourseBaselinePublicationPhase::Prepared)
      return failure("Prepared context");
    selected = request;
    operating = true;
    bool verified = (!recovering || intent()) && evidence() && noReceipts();
    if (verified) {
      const auto result =
          recovering ? archive.inspectPrepared(selected.request.manifest)
                     : archive.open(selected.request.manifest.logicalIdentity, selected.request.manifest.contentHash);
      if (result == CourseArchiveResult::Missing) {
        verified = backups.verifyCurrent(selected.request.reviewHash, reader, generation,
                                         selected.request.manifest.logicalIdentity);
      } else if (recovering && result == CourseArchiveResult::Ok) {
        const auto* manifest = archive.manifest();
        const auto* path = archive.path();
        CourseCandidateDetails details;
        verified = manifest && *manifest == selected.request.manifest && path && guard() &&
                   validateStagedCourse(path, parser, scratch, details) &&
                   details.major == selected.request.manifest.formatVersion && guard() &&
                   backups.verifyCurrent(selected.request.reviewHash, reader, generation,
                                         selected.request.manifest.logicalIdentity, &archive);
      } else {
        verified = false;
      }
    }
    return finish(verified, "Prepared evidence");
  }
  static bool verifyPreparation(void* context, const CourseBaselinePublicationRecord& request, bool recovering) {
    return context && static_cast<HalCourseBaselineArchiveSession*>(context)->verifyPrepared(request, recovering);
  }
  // The selected transaction must already have a matching canonical Prepared
  // intent. Caller additionally verifies the live cohort and owned recovery scope.
  bool publishArchive(const CourseBaselinePublicationRecord& request, std::string_view source) {
    if (operating) return failure("reentry");
    if (!valid(request) || request.phase != CourseBaselinePublicationPhase::Prepared || source.empty() ||
        source.size() >= sourcePath.size() || source.find('\0') != std::string_view::npos ||
        course_baseline_detail::overlaps(source.data(), source.size(), this, sizeof(*this)))
      return failure("publication context/source");
    selected = request;
    std::copy(source.begin(), source.end(), sourcePath.begin());
    sourcePath[source.size()] = 0;
    operating = true;
    CourseCandidateDetails details;
    const bool published = intent() && evidence() && sourceMatches() && guard() &&
                           validateStagedCourse(sourcePath.data(), parser, scratch, details) &&
                           details.major == selected.request.manifest.formatVersion && guard() &&
                           archive.publish(selected.request.manifest, sourcePath.data()) == CourseArchiveResult::Ok;
    return finish(published, "archive publication");
  }
  bool close() {
    parser.close();
    const bool consentClosed = consent.close();
    const bool backupsClosed = backups.closeReaders();
    const bool archiveClosed = archive.closeReaders();
    const bool sourceClosed = !sourceReader.isOpen() || sourceReader.close();
    const bool metadataClosed = metadata.closeReaders();
    return consentClosed && backupsClosed && archiveClosed && sourceClosed && metadataClosed;
  }
  static bool verifyPublication(void* context, const CourseBaselinePublicationRecord& request) {
    return context && static_cast<HalCourseBaselineArchiveSession*>(context)->verify(request);
  }

 private:
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  tinta::core::pack::Pack& parser;
  Permission permitted;
  void* context;
  HalCourseBaselineImportConsentStore consent;
  HalCourseBaselineReviewBackup backups;
  HalCoursePackArchive archive;
  HalCourseRemovalMetadata metadata;
  HalFile sourceReader;
  std::array<char, COURSE_ARCHIVE_PATH_SIZE> sourcePath{};
  std::array<char, 96> intentPath{};
  CourseBaselinePublicationRecord selected, observed;
  CourseBaselineImportRequest loaded;
  bool operating = false;
  bool valid(const CourseBaselinePublicationRecord& request) const {
    return validCourseBaselinePublicationRecord(request) && request.reader == reader &&
           request.request.generation == generation && request.request.owner == owner &&
           scratch.size() >= COURSE_BASELINE_REVIEW_MAX_SIZE + 512 &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), &parser, sizeof(parser));
  }
  bool noReceipts() {
    if (!guard() || !admitCompanionHeap(sizeof(HalHistoricalCourseHistory), sizeof(HalHistoricalCourseHistory)))
      return false;
    auto receipts = makeUniqueNoThrow<HalHistoricalCourseHistory>(generation, selected.request.manifest.logicalIdentity,
                                                                  scratch, allowed, this);
    if (!receipts) return failure("OOM: Prepared receipt reader");
    const auto result = receipts->inspect();
    const bool closed = receipts->closeReaders();
    return result == HistoricalCourseHistoryResult::Missing && closed && guard();
  }
  bool evidence() {
    return guard() && consent.load(selected.request.transaction, loaded) == CourseBaselineConsentResult::Ok &&
           loaded == selected.request && guard() &&
           backups.verifyStored(selected.request.reviewHash, reader, generation,
                                selected.request.manifest.logicalIdentity) &&
           guard();
  }
  bool intent() {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::copy(PREFIX.begin(), PREFIX.end(), intentPath.begin());
    size_t at = PREFIX.size();
    for (const auto byte : selected.request.transaction) {
      intentPath[at++] = HEX_DIGITS[byte >> 4];
      intentPath[at++] = HEX_DIGITS[byte & 15];
    }
    static constexpr char CANONICAL[] = ".prepared", STAGE[] = ".prepared.tmp";
    std::copy_n(STAGE, sizeof(STAGE), intentPath.begin() + at);
    uint64_t size = 0;
    if (!guard() || metadata.stat(intentPath.data(), size) != FileStatus::Missing) return false;
    std::copy_n(CANONICAL, sizeof(CANONICAL), intentPath.begin() + at);
    if (!guard() || metadata.stat(intentPath.data(), size) != FileStatus::Present ||
        size != COURSE_BASELINE_PUBLICATION_SIZE || !guard() ||
        !metadata.read(intentPath.data(), 0, scratch.first(COURSE_BASELINE_PUBLICATION_SIZE)) || !guard())
      return false;
    return decodeCourseBaselinePublicationRecord(scratch.first(COURSE_BASELINE_PUBLICATION_SIZE), observed) &&
           observed == selected && guard();
  }
  bool sourceMatches() {
    uint64_t size = 0, length = 0;
    Digest hash{};
    if (!guard() || metadata.stat(sourcePath.data(), size) != FileStatus::Present ||
        size != selected.request.manifest.length || !guard() ||
        !Storage.openFileForReadReusing("COMPANION", sourcePath.data(), sourceReader))
      return false;
    const bool matched = !sourceReader.isDirectory() &&
                         hashInventoryFile(sourceReader, scratch, length, hash, allowed, this) && sourceReader.sync();
    const bool closed = sourceReader.close();
    return matched && closed && guard() && length == selected.request.manifest.length &&
           hash == selected.request.manifest.contentHash;
  }
  bool finish(bool success, const char* operation) {
    const bool closed = close();
    operating = false;
    return (success && closed && guard()) || failure(operation);
  }
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineArchiveSession*>(context)->guard(); }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Baseline archive operation failed: %s", operation);
    return false;
  }
};
}  // namespace companion

#endif  // LILA_TINTA
