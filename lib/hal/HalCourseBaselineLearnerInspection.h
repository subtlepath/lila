#pragma once

#if LILA_TINTA

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <optional>

#include "CompanionCourseBaselinePublication.h"
#include "CompanionCourseValidation.h"
#include "HalCourseBaselineImportConsentStore.h"
#include "HalCourseBaselineReplayReceipt.h"
#include "HalCourseBaselineReviewedJournalAudit.h"
#include "HalTintaLegacyCourseReferences.h"
#include "HalTintaLegacyItemCatalogValidation.h"
#include "HalTintaLegacyReviewValidation.h"
#include "HalTintaLegacySessionValidation.h"
#include "HalTintaReplayDayCorrespondence.h"
#include "HalTintaReplayItemCorrespondence.h"
#include "HalTintaReplayLessonCorrespondence.h"
#include "HalTintaReplayMarkCorrespondence.h"

namespace companion {
struct CourseBaselineSessionLimits {
  uint8_t depth = 0, screens = 0;
  uint16_t queued = 0;
};
// Admit off stack. Caller proves native journal readiness and excludes all
// writers; inspection uses immutable reviewed copies, never current learner files.
class HalCourseBaselineLearnerInspection final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineLearnerInspection(const Identity& reader, const Identity& generation, const Identity& owner,
                                     std::span<uint8_t> scratch, tinta::core::pack::Pack& parser,
                                     CourseBaselineSessionLimits limits, Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        parser(parser),
        limits(limits),
        permitted(permitted),
        context(context),
        consent(reader, generation, owner, scratch, allowed, this),
        backups(scratch, allowed, this),
        review(scratch.size() > COURSE_BASELINE_REVIEW_MAX_SIZE ? scratch.subspan(COURSE_BASELINE_REVIEW_MAX_SIZE)
                                                                : std::span<uint8_t>{},
               allowed, this),
        metadata(allowed, this),
        source(*this) {}
  ~HalCourseBaselineLearnerInspection() { closeReaders(); }
  HalCourseBaselineLearnerInspection(const HalCourseBaselineLearnerInspection&) = delete;
  HalCourseBaselineLearnerInspection& operator=(const HalCourseBaselineLearnerInspection&) = delete;
  bool inspect(const CourseBaselinePublicationRecord& request, const char* path) {
    if (operating) return failure("reentry");
    inspected = false;
    if (!validCourseBaselinePublicationRecord(request) || request.reader != reader ||
        request.request.generation != generation || request.request.owner != owner || !path ||
        strnlen(path, sourcePath.size()) == sourcePath.size() || !limits.depth || !limits.screens || !limits.queued ||
        scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &parser, sizeof(parser)) ||
        course_baseline_detail::overlaps(path, std::strlen(path) + 1, this, sizeof(*this)))
      return failure("context or workspace");
    selected = request;
    std::copy_n(path, std::strlen(path) + 1, sourcePath.begin());
    operating = true;
    committedRecords = reviewRecords = 0;
    hasItems = hasReviews = false;
    receiptVerified = false;
    needsSessionRebuild = false;
    bool valid = closeReaders() && guard() &&
                 consent.load(selected.request.transaction, loaded) == CourseBaselineConsentResult::Ok &&
                 loaded == selected.request &&
                 backups.verifyStored(selected.request.reviewHash, reader, generation,
                                      selected.request.manifest.logicalIdentity) &&
                 sourceMatches() && sourceOpen();
    if (valid) valid = validateCourseCandidate(parser, source, scratch) == CourseValidationResult::Ok && guard();
    if (valid) valid = readCourseItemIdentityTable(source, identities, hasIdentities);
    CourseUidLookup catalog(source);
    if (valid) valid = catalog.begin() && loadReview();
    if (valid) valid = prepareJournalEvidence();
    const auto count = reviewCount;
    for (size_t index = 0; valid && index < count; ++index) {
      valid = loadEntry(index);
      if (valid && entry[0] == uint8_t(CourseBaselineReviewDomain::Learner)) valid = inspectFile(index, catalog);
    }
    if (valid && hasItems && committedRecords && !hasReviews) valid = false;
    if (valid)
      valid = catalog.valid() && parser.isOpen() && guard() &&
              backups.verifyStored(selected.request.reviewHash, reader, generation,
                                   selected.request.manifest.logicalIdentity);
    const bool closed = closeReaders();
    operating = false;
    inspected = valid && closed && guard();
    return inspected || failure("learner compatibility");
  }
  std::optional<bool> sessionRequiresRebuild() const {
    return inspected && guard() ? std::optional<bool>{needsSessionRebuild} : std::nullopt;
  }
  bool closeReaders() {
    replay.reset();
    parser.close();
    source.failed = false;
    const bool sourceClosed = !sourceFile.isOpen() || sourceFile.close();
    const bool fileClosed = !file.isOpen() || file.close();
    const bool consentClosed = consent.close(), backupsClosed = backups.closeReaders();
    const bool reviewClosed = review.closeReaders(), metadataClosed = metadata.closeReaders();
    return sourceClosed && fileClosed && consentClosed && backupsClosed && reviewClosed && metadataClosed;
  }
  static bool compatible(void* context, const CourseBaselinePublicationRecord& request, const char* path,
                         std::span<uint8_t> scratch) {
    auto* inspector = static_cast<HalCourseBaselineLearnerInspection*>(context);
    return inspector && scratch.data() == inspector->scratch.data() && scratch.size() == inspector->scratch.size() &&
           inspector->inspect(request, path);
  }

 private:
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  tinta::core::pack::Pack& parser;
  CourseBaselineSessionLimits limits;
  Permission permitted;
  void* context;
  HalCourseBaselineImportConsentStore consent;
  HalCourseBaselineReviewBackup backups;
  HalCourseBaselineReviewStore review;
  HalCourseRemovalMetadata metadata;
  HalFile sourceFile, file;
  std::array<char, 128> sourcePath{};
  std::array<char, 112> filePath{};
  std::array<uint8_t, COURSE_BASELINE_REVIEW_ENTRY_SIZE> entry{};
  CourseBaselinePublicationRecord selected;
  CourseBaselineImportRequest loaded;
  tinta::core::pack::DirEntry identities{};
  std::unique_ptr<HalCourseBaselineReplaySession> replay;
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  Digest sessionDigest{};
  bool receiptVerified = false;
  bool inspected = false, needsSessionRebuild = false;
  size_t reviewCount = 0;
  uint32_t committedRecords = 0, reviewRecords = 0;
  bool operating = false, hasIdentities = false, hasItems = false, hasReviews = false;
  class Source final : public tinta::core::pack::PackSource {
   public:
    explicit Source(HalCourseBaselineLearnerInspection& owner) : owner(owner) {}
    uint32_t size() const override {
      return failed || !owner.sourceFile.isOpen() ||
                     owner.sourceFile.fileSize64() != owner.selected.request.manifest.length
                 ? 0
                 : owner.selected.request.manifest.length;
    }
    bool read(uint32_t offset, void* output, uint32_t count) override {
      if (failed || !owner.guard() || offset > size() || count > size() - offset || !owner.sourceFile.isOpen() ||
          owner.sourceFile.fileSize64() != size() || !owner.sourceFile.seek64(offset) ||
          owner.sourceFile.read(output, count) != static_cast<int64_t>(count) || !owner.guard()) {
        failed = true;
        return false;
      }
      if (++readsSinceYield == 32) {
        readsSinceYield = 0;
        vTaskDelay(1);
      }
      return true;
    }
    bool failed = false;

   private:
    HalCourseBaselineLearnerInspection& owner;
    uint8_t readsSinceYield = 0;
  } source;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineLearnerInspection*>(context)->guard(); }
  static bool failure([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Native baseline learner inspection refused: %s", reason);
    return false;
  }
  bool sourceMatches() {
    uint64_t size = 0, length = 0;
    Digest digest{};
    if (!guard() || metadata.stat(sourcePath.data(), size) != FileStatus::Present ||
        size != selected.request.manifest.length ||
        !Storage.openFileForReadReusing("COMPANION", sourcePath.data(), sourceFile))
      return false;
    const bool matched = hashInventoryFile(sourceFile, scratch, length, digest, allowed, this) && sourceFile.sync();
    const bool closed = sourceFile.close();
    return matched && closed && guard() && length == selected.request.manifest.length &&
           digest == selected.request.manifest.contentHash;
  }
  bool sourceOpen() {
    source.failed = false;
    return guard() && Storage.openFileForReadReusing("COMPANION", sourcePath.data(), sourceFile) &&
           !sourceFile.isDirectory() && sourceFile.fileSize64() == selected.request.manifest.length;
  }
  bool loadReview() {
    if (!guard() ||
        review.open(selected.request.reviewHash, reader, generation, selected.request.manifest.logicalIdentity,
                    scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE)) != CourseBaselineReviewStoreResult::Ok)
      return false;
    reviewCount = course_review_detail::number(scratch, 56, 2);
    return review.closeReaders() && guard();
  }
  bool loadEntry(size_t index) {
    if (!loadReview() || index >= reviewCount) return false;
    std::copy_n(scratch.begin() + COURSE_BASELINE_REVIEW_HEADER_SIZE + index * entry.size(), entry.size(),
                entry.begin());
    return true;
  }
  bool prepareJournalEvidence() {
    const bool journalPresent = scratch[5];
    size_t receiptIndex = reviewCount;
    CourseBaselineReviewView view;
    if (!view.decode(scratch.first(64 + reviewCount * COURSE_BASELINE_REVIEW_ENTRY_SIZE))) return false;
    for (size_t at = 0; at < view.count(); ++at) {
      const auto item = view.entry(at);
      if (item[0] == uint8_t(CourseBaselineReviewDomain::Learner) &&
          std::string_view(reinterpret_cast<const char*>(item.data() + 4), item[2]) == "sync-receipt")
        receiptIndex = at;
    }
    if (receiptIndex == reviewCount) {
      if (!journalPresent) return guard();
      auto audit = createHalCourseBaselineReviewedJournalAudit(parser, source, allowed, this);
      return audit && audit->run(scratch.first(64 + reviewCount * COURSE_BASELINE_REVIEW_ENTRY_SIZE),
                                 selected.request.reviewHash, selected.request.manifest.logicalIdentity, scratch);
    }
    if (!journalPresent) return failure("receipt without reviewed journal");
    replay = createHalCourseBaselineReplaySession(parser, source, allowed, this);
    if (!replay ||
        !replay->run(scratch.first(64 + reviewCount * COURSE_BASELINE_REVIEW_ENTRY_SIZE), selected.request.reviewHash,
                     selected.request.manifest.logicalIdentity, scratch) ||
        !loadEntry(receiptIndex) || course_review_detail::number(entry, 28, 8) != receipt.size())
      return failure("receipt replay");
    backupPath(receiptIndex);
    std::copy_n(entry.begin() + 36, sessionDigest.size(), sessionDigest.begin());
    if (!guard() || !Storage.openFileForReadReusing("COMPANION", filePath.data(), file) || file.isDirectory() ||
        file.fileSize64() != receipt.size() || file.read(receipt.data(), receipt.size()) != int64_t(receipt.size()) ||
        !file.close() || !guard())
      return failure("receipt backup");
    auto proof = createHalCourseBaselineReplayReceipt(allowed, this);
    if (!proof ||
        !proof->run(*replay, receipt, sessionDigest, selected.request.manifest.logicalIdentity, generation,
                    selected.request.manifest.contentHash, scratch) ||
        !proof->sessionSnapshot())
      return failure("receipt correspondence");
    TintaDerivedManifestView manifest;
    if (!manifest.decode(receipt)) return false;
    static constexpr std::string_view NAMES[] = {"items.bin", "reviews.log", "lessons.bin", "readings.bin", "days.bin"};
    uint8_t found = 0;
    for (size_t at = 0; at < reviewCount; ++at) {
      if (!loadEntry(at)) return false;
      if (entry[0] != uint8_t(CourseBaselineReviewDomain::Learner)) continue;
      const std::string_view name(reinterpret_cast<const char*>(entry.data() + 4), entry[2]);
      for (unsigned kind = 0; kind < 5; ++kind) {
        if (name != NAMES[kind]) continue;
        const auto fileKind = static_cast<TintaDerivedFile>(kind);
        if (course_review_detail::number(entry, 28, 8) != manifest.length(fileKind) ||
            !std::equal(entry.begin() + 36, entry.begin() + 68, manifest.hash(fileKind).begin()))
          return failure("retained receipt file");
        found |= uint8_t(1u << kind);
      }
    }
    receiptVerified = found == 31 && guard();
    return receiptVerified || failure("incomplete receipt cohort");
  }
  bool diagnostic(std::string_view name) const {
    if (name == "usage.seq") return true;
    if (name.size() != 14 || !name.starts_with("usage-") || !name.ends_with(".log")) return false;
    unsigned number = 0;
    for (size_t i = 6; i < 10; ++i) {
      if (name[i] < '0' || name[i] > '9') return false;
      number = number * 10 + unsigned(name[i] - '0');
    }
    return number != 0;
  }
  void backupPath(size_t index) {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-review-state-";
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::copy(PREFIX.begin(), PREFIX.end(), filePath.begin());
    size_t at = PREFIX.size();
    for (const auto byte : selected.request.reviewHash) {
      filePath[at++] = DIGITS[byte >> 4];
      filePath[at++] = DIGITS[byte & 15];
    }
    filePath[at++] = '-';
    filePath[at++] = DIGITS[(index >> 4) & 15];
    filePath[at++] = DIGITS[index & 15];
    filePath[at] = 0;
  }
  bool covered(uint32_t uid, CourseUidLookup& catalog) {
    if (uid == UINT32_MAX) return true;
    int32_t index = -1;
    return uid && catalog.find(uid, index) && (index >= 0 || (hasIdentities && uid <= identities.count)) && guard();
  }
  bool inspectItems(CourseUidLookup& catalog) {
    LegacyItemCatalogReport report;
    if (!inspectTintaLegacyItemCatalog(file, catalog, scratch, report, allowed, this)) return false;
    HalTintaLegacyItemView view(file);
    if (!view.begin(scratch, allowed, this) || !view.header()) return false;
    committedRecords = view.header()->journalCount;
    const auto count = view.count();
    for (uint32_t i = 0; i < count; ++i) {
      tinta::core::ItemState item;
      if (!view.record(i, item) || !covered(item.uid, catalog)) return false;
    }
    hasItems = true;
    auto* projection = receiptVerified && replay ? replay->workingStore() : nullptr;
    return guard() &&
           (!receiptVerified ||
            (projection && compareTintaReplayItems(file, *projection, selected.request.manifest.logicalIdentity,
                                                   catalog, scratch, allowed, this)));
  }
  bool inspectReviews(CourseUidLookup& catalog) {
    LegacyReviewReport report;
    if (!inspectTintaLegacyReviews(file, catalog, committedRecords, scratch, report, allowed, this) || !file.seek64(0))
      return false;
    for (uint32_t i = 0; i < report.records; ++i) {
      if (!guard() || file.read(scratch.data(), 12) != 12 || !covered(binary_record::getU32(scratch.data()), catalog))
        return false;
    }
    hasReviews = true;
    reviewRecords = report.records;
    return guard();
  }
  bool inspectMarks(CourseUidLookup& catalog, bool readings) {
    HalTintaLegacyMarkView view(file);
    LegacyMarkCatalogReport report;
    if (!view.begin(scratch, allowed, this) ||
        !inspectTintaLegacyMarkReferences(view, catalog, parser, readings, report, allowed, this))
      return false;
    if (readings) return report.retired == 0;
    uint16_t count = 0;
    if (!view.entryCount(count)) return false;
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t uid = 0;
      if (!view.identityAt(i, uid) || !covered(uid, catalog)) return false;
    }
    return guard();
  }
  bool inspectSession(CourseUidLookup& catalog) {
    LegacySessionReport report;
    if (!inspectTintaLegacySession(file, catalog, hasReviews ? reviewRecords : committedRecords,
                                   parser.count(tinta::core::pack::Section::Less),
                                   parser.count(tinta::core::pack::Section::Phrs), limits.depth, limits.screens,
                                   limits.queued, scratch, report, allowed, this))
      return false;
    if (report.hasSnapshot && !receiptVerified) return failure("saved session without verified receipt");
    needsSessionRebuild = report.journalChanged;
    LegacySessionView view;
    if (!decodeTintaLegacySession(scratch.first(file.fileSize64()), limits.depth, limits.screens, limits.queued, view))
      return false;
    if (receiptVerified && !view.file.session.empty())
      needsSessionRebuild = needsSessionRebuild || tinta::core::sessionSnapshotChanged(view.file, sessionDigest);
    for (size_t at = 0; at < view.entries.size(); at += 5)
      if (!covered(binary_record::getU32(view.entries.data() + at), catalog)) return false;
    return guard();
  }
  bool inspectDays() {
    const auto length = file.fileSize64();
    if (length < 4 || (length - 4) % 12 || !file.seek64(0) || file.read(scratch.data(), 4) != 4 ||
        std::memcmp(scratch.data(), "TDL1", 4))
      return false;
    for (uint64_t at = 4; at < length; at += 12) {
      if ((at - 4) % (12 * 32) == 0) vTaskDelay(1);
      if (!guard() || file.read(scratch.data(), 12) != 12 ||
          binary_record::getU16(scratch.data() + 10) != uint16_t(binary_record::crc32(scratch.data(), 10)))
        return false;
    }
    return file.fileSize64() == length && guard();
  }
  bool inspectFile(size_t index, CourseUidLookup& catalog) {
    const std::string_view name(reinterpret_cast<const char*>(entry.data() + 4), entry[2]);
    if (diagnostic(name)) return guard();
    backupPath(index);
    const auto length = course_review_detail::number(entry, 28, 8);
    uint64_t actual = 0;
    if (!guard() || metadata.stat(filePath.data(), actual) != FileStatus::Present || actual != length ||
        !Storage.openFileForReadReusing("COMPANION", filePath.data(), file) || file.isDirectory() ||
        file.fileSize64() != length)
      return false;
    bool valid = false;
    auto* projection = receiptVerified && replay ? replay->workingStore() : nullptr;
    if (receiptVerified && !projection) return false;
    if (name == "items.bin")
      valid = inspectItems(catalog);
    else if (name == "reviews.log")
      valid = inspectReviews(catalog);
    else if (name == "profile.bin") {
      tinta::core::Profile profile;
      tinta::core::Profile::LoadResult status;
      valid = inspectTintaLegacyProfile(file, profile, status, allowed, this) &&
              inspectTintaLegacyLessonReferences(profile, parser, allowed, this) &&
              (!receiptVerified || compareTintaReplayLessons(profile, parser, *projection,
                                                             selected.request.manifest.logicalIdentity, allowed, this));
    } else if (name == "starred.bin" || name == "read.bin")
      valid = inspectMarks(catalog, name == "read.bin") &&
              (!receiptVerified ||
               compareTintaReplayMarks(file, *projection, selected.request.manifest.logicalIdentity, catalog, parser,
                                       name == "read.bin" ? TintaReplayMarkKind::Readings : TintaReplayMarkKind::Stars,
                                       scratch, allowed, this));
    else if (name == "session.bin")
      valid = inspectSession(catalog);
    else if (name == "days.bin")
      valid = inspectDays() &&
              (!receiptVerified || compareTintaReplayDays(file, *projection, selected.request.manifest.logicalIdentity,
                                                          scratch, allowed, this));
    else if (name == "sync-receipt")
      valid = receiptVerified;
    else if (name == "lessons.bin" || name == "readings.bin") {
      HalTintaCompletionSetView completions(file);
      valid = receiptVerified &&
              completions.begin(name == "lessons.bin" ? TintaCompletionKind::Lessons : TintaCompletionKind::Readings,
                                scratch, allowed, this);
    }
    const bool synced = file.sync(), closed = file.close();
    return valid && synced && closed && guard();
  }
};
// One retained import owner; its fixed readers exceed the native stack budget.
inline std::unique_ptr<HalCourseBaselineLearnerInspection> createHalCourseBaselineLearnerInspection(
    const Identity& reader, const Identity& generation, const Identity& owner, std::span<uint8_t> scratch,
    tinta::core::pack::Pack& parser, CourseBaselineSessionLimits limits,
    HalCourseBaselineLearnerInspection::Permission permitted, void* context) {
  if (!permitted || !permitted(context) || !limits.depth || !limits.screens || !limits.queued ||
      scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
      !admitCompanionHeap(sizeof(HalCourseBaselineLearnerInspection), sizeof(HalCourseBaselineLearnerInspection))) {
    LOG_ERR("COMPANION", "Native learner inspection admission failed");
    return nullptr;
  }
  auto inspection = makeUniqueNoThrow<HalCourseBaselineLearnerInspection>(reader, generation, owner, scratch, parser,
                                                                          limits, permitted, context);
  if (!inspection) LOG_ERR("COMPANION", "OOM: native baseline learner inspection");
  return inspection;
}
}  // namespace companion

#endif
