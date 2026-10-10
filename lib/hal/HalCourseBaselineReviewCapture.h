#pragma once

#include <mbedtls/sha256.h>

#include "CompanionCourseBaselineReview.h"
#include "CompanionTintaJournalPaths.h"
#include "CompanionUnboundCourseMigrationPaths.h"
#include "HalCoursePackArchive.h"
#include "HalCourseRemovalMetadata.h"
#include "HalCourseStateIsolation.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class CourseBaselineReviewResult { Ok, Invalid, Busy, Missing, Corrupt, TooManyFiles, IoError };
// Retain off stack after admission. Caller provides native reader/card identities,
// verifies canonical journal readiness, closes activities and excludes all state
// and namespace writers. Capture is read-only and does not authorize pack reuse.
class HalCourseBaselineReviewCapture final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineReviewCapture(std::span<uint8_t> scratch, Permission permitted, void* context)
      : scratch(scratch),
        permitted(permitted),
        context(context),
        metadata(permitted, context),
        isolation(metadata, io(), permitted, context) {
    mbedtls_sha256_init(&digestContext);
  }
  ~HalCourseBaselineReviewCapture() {
    closeReaders();
    mbedtls_sha256_free(&digestContext);
  }
  HalCourseBaselineReviewCapture(const HalCourseBaselineReviewCapture&) = delete;
  HalCourseBaselineReviewCapture& operator=(const HalCourseBaselineReviewCapture&) = delete;
  // An owned archive must already be fully verified for Prepared recovery. Only
  // its exact immutable reference is omitted from the reviewed learner cohort.
  CourseBaselineReviewResult capture(const Identity& reader, const Identity& generation, const Identity& course,
                                     HalCoursePackArchive* ownedArchive = nullptr) {
    if (capturing) {
      LOG_DBG("COMPANION", "Course baseline review already running");
      return CourseBaselineReviewResult::Busy;
    }
    ready = false;
    if (scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) || reader == Identity{} ||
        generation == Identity{} || !courseStateDirectory(course, scope)) {
      LOG_ERR("COMPANION", "Invalid course baseline review input");
      return CourseBaselineReviewResult::Invalid;
    }
    capturing = true;
    selectedReader = reader;
    selectedGeneration = generation;
    selectedCourse = course;
    unbound = namespaceUnbound();
    if (unbound) std::copy_n("/tinta", 7, scope.begin());
    count = 0;
    referencePresent = ownedArchive != nullptr;
    referenceSeen = false;
    if (referencePresent) {
      const auto* manifest = ownedArchive->manifest();
      const auto* reference = ownedArchive->referencePath();
      if (!manifest || !reference || manifest->logicalIdentity != selectedCourse ||
          strnlen(reference, referencePath.size()) == referencePath.size())
        return finish(CourseBaselineReviewResult::Invalid);
      referenceManifest = *manifest;
      referencePending = ownedArchive->referenceIsPending();
      referenceLength = ownedArchive->referenceLength();
      std::copy_n(reference, std::strlen(reference) + 1, referencePath.begin());
    }
    if (!guard()) return finish(CourseBaselineReviewResult::Busy);
    if (!closeReaders() || (unbound && referencePresent) || (referencePresent && !referenceMatches()) || !verifyScope())
      return finish(CourseBaselineReviewResult::Corrupt);
    auto result = scanScope();
    if (result != CourseBaselineReviewResult::Ok) return finish(result);
    result = captureJournal();
    if (result != CourseBaselineReviewResult::Ok) return finish(result);
    static constexpr const char* PROOF_PATHS[] = {COURSE_MARK_MIGRATION_PATHS.done, COURSE_MARK_MIGRATION_PATHS.intent,
                                                  COURSE_STATE_MIGRATION_PATHS.done,
                                                  COURSE_STATE_MIGRATION_PATHS.intent};
    static constexpr std::string_view PROOF_NAMES[] = {"mark-done", "mark-intent", "state-done", "state-intent"};
    for (unsigned at = 0; at < 4; ++at) {
      result = captureFile(CourseBaselineReviewDomain::Isolation, PROOF_NAMES[at], PROOF_PATHS[at], unbound);
      if (result != CourseBaselineReviewResult::Ok) return finish(result);
    }
    if (!verifyScope() || (referencePresent && !referenceMatches()) || !closeReaders() || !guard())
      return finish(CourseBaselineReviewResult::Corrupt);
    auto bytes = scratch.first(64 + count * COURSE_BASELINE_REVIEW_ENTRY_SIZE);
    std::fill_n(bytes.begin(), COURSE_BASELINE_REVIEW_HEADER_SIZE, 0);
    std::copy(course_review_detail::MAGIC.begin(), course_review_detail::MAGIC.end(), bytes.begin());
    bytes[4] = unbound ? 2 : 1;
    bytes[5] = journalPresent;
    bytes[6] = unbound;
    std::copy(selectedReader.begin(), selectedReader.end(), bytes.begin() + 8);
    std::copy(selectedGeneration.begin(), selectedGeneration.end(), bytes.begin() + 24);
    std::copy(selectedCourse.begin(), selectedCourse.end(), bytes.begin() + 40);
    course_review_detail::number(bytes, 56, count, 2);
    course_review_detail::number(bytes, bytes.size() - 4, binary_record::crc32(bytes.data(), bytes.size() - 4), 4);
    CourseBaselineReviewView view;
    if (!view.decode(bytes, true) || mbedtls_sha256_starts(&digestContext, 0) ||
        mbedtls_sha256_update(&digestContext, bytes.data(), bytes.size()) ||
        mbedtls_sha256_finish(&digestContext, reviewHash.data()))
      return finish(CourseBaselineReviewResult::Corrupt);
    return finish(CourseBaselineReviewResult::Ok);
  }
  std::span<const uint8_t> bytes() const {
    return loan() ? scratch.first(64 + count * COURSE_BASELINE_REVIEW_ENTRY_SIZE) : std::span<const uint8_t>{};
  }
  const Digest* hash() const { return loan() ? &reviewHash : nullptr; }
  bool closeReaders() {
    ready = false;
    const bool isolationClosed = isolation.closeReaders(), metadataClosed = metadata.closeReaders();
    const bool entryClosed = !entry.isOpen() || entry.close(),
               directoryClosed = !directory.isOpen() || directory.close();
    const bool readerClosed = !reader.isOpen() || reader.close();
    return isolationClosed && metadataClosed && entryClosed && directoryClosed && readerClosed;
  }

 private:
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalCourseStateIsolation isolation;
  HalFile directory, entry, reader;
  mbedtls_sha256_context digestContext;
  Identity selectedReader{}, selectedGeneration{}, selectedCourse{};
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> scope{};
  std::array<char, 256> name{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  CourseBaselineReviewFile file;
  Digest reviewHash{};
  std::array<char, COURSE_ARCHIVE_PATH_SIZE> referencePath{};
  ContentManifest referenceManifest;
  uint64_t referenceLength = 0;
  bool referencePresent = false, referenceSeen = false, referencePending = false;
  size_t count = 0;
  bool journalPresent = false, capturing = false;
  bool unbound = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  bool namespaceUnbound() {
    static constexpr const char* PATHS[] = {COURSE_BINDING_PATH,
                                            COURSE_BINDING_STAGE,
                                            COURSE_BINDING_BACKUP,
                                            COURSE_STATE_MIGRATION_PATHS.intent,
                                            COURSE_STATE_MIGRATION_PATHS.stage,
                                            COURSE_STATE_MIGRATION_PATHS.done,
                                            COURSE_STATE_MIGRATION_PATHS.doneStage,
                                            COURSE_MARK_MIGRATION_PATHS.intent,
                                            COURSE_MARK_MIGRATION_PATHS.stage,
                                            COURSE_MARK_MIGRATION_PATHS.done,
                                            COURSE_MARK_MIGRATION_PATHS.doneStage};
    uint64_t size = 0;
    for (const auto* candidate : UNBOUND_COURSE_INTENT_PATHS)
      if (!guard() || metadata.stat(candidate, size) != FileStatus::Missing || !guard()) return false;
    for (const auto* candidate : UNBOUND_COURSE_INTENT_STAGES)
      if (!guard() || metadata.stat(candidate, size) != FileStatus::Missing || !guard()) return false;
    for (const auto* candidate : PATHS)
      if (!guard() || metadata.stat(candidate, size) != FileStatus::Missing || !guard()) return false;
    return true;
  }
  bool verifyScope() { return unbound ? namespaceUnbound() : isolation.verify(selectedCourse); }
  bool loan() const {
    if (!guard()) ready = false;
    return ready;
  }
  std::span<uint8_t> io() const {
    return scratch.size() >= COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ? scratch.subspan(COURSE_BASELINE_REVIEW_MAX_SIZE)
                                                                   : std::span<uint8_t>{};
  }
  static uint32_t fold(uint32_t value) { return Storage.foldFilenameCodepoint(value); }
  static bool progress(void* context) { return static_cast<HalCourseBaselineReviewCapture*>(context)->guard(); }
  bool openDirectory(const char* path) {
    return guard() && (!entry.isOpen() || entry.close()) && (!directory.isOpen() || directory.close()) &&
           Storage.openFileForReadReusing("COMPANION", path, directory) && guard() && directory.isDirectory() &&
           entry.prepareDirectoryEntry();
  }
  bool entryName(std::string_view& output) {
    const auto length = entry.getName(name.data(), name.size());
    if (!guard() || !length || length >= name.size() || name[length] != 0 ||
        !hal_filename::valid(std::string_view(name.data(), length)))
      return false;
    char alias[13]{};
    if (!entry.getShortName(alias, sizeof(alias)) || !guard()) return false;
    const auto aliasLength = strnlen(alias, sizeof(alias));
    if (aliasLength == sizeof(alias) || !hal_filename::valid(std::string_view(alias, aliasLength))) return false;
    output = std::string_view(name.data(), length);
    return true;
  }
  bool referenceMatches() {
    uint64_t size = 0;
    if (!guard() || metadata.stat(referencePath.data(), size) != FileStatus::Present || size != referenceLength ||
        size > COURSE_BINDING_SIZE || !guard())
      return false;
    if (referencePending) {
      const auto expected = io().first(COURSE_BINDING_SIZE);
      if (encodeCourseBinding(referenceManifest, expected) != expected.size()) return false;
      const auto saved = io().subspan(COURSE_BINDING_SIZE, static_cast<size_t>(size));
      return (!size || metadata.read(referencePath.data(), 0, saved)) && guard() &&
             std::equal(saved.begin(), saved.end(), expected.begin());
    }
    if (size != COURSE_BINDING_SIZE || !metadata.read(referencePath.data(), 0, io().first(COURSE_BINDING_SIZE)) ||
        !guard())
      return false;
    ContentManifest observed;
    return decodeCourseBinding(io().first(COURSE_BINDING_SIZE), observed) && observed == referenceManifest && guard();
  }
  CourseBaselineReviewResult scanScope() {
    if (!openDirectory(scope.data())) return CourseBaselineReviewResult::IoError;
    unsigned steps = 0;
    for (;;) {
      if (!guard()) return CourseBaselineReviewResult::Busy;
      if (entry.isOpen() && !entry.close()) return CourseBaselineReviewResult::IoError;
      const auto next = directory.nextEntry(entry);
      if (next == HalDirectoryResult::Error) return CourseBaselineReviewResult::IoError;
      if (next == HalDirectoryResult::End) break;
      std::string_view filename;
      if (!entryName(filename) || entry.isDirectory()) return CourseBaselineReviewResult::Corrupt;
      if (referencePresent) {
        const std::string_view reference(referencePath.data());
        const auto wanted = reference.substr(reference.find_last_of('/') + 1);
        const auto match = hal_filename::compare(wanted, filename, fold);
        if (match == hal_filename::Comparison::Invalid) return CourseBaselineReviewResult::Corrupt;
        if (match == hal_filename::Comparison::Equal) {
          if (referenceSeen) return CourseBaselineReviewResult::Corrupt;
          referenceSeen = true;
          continue;
        }
      }
      if (filename.size() >= file.name.size()) return CourseBaselineReviewResult::Corrupt;
      file.name.fill(0);
      for (size_t at = 0; at < filename.size(); ++at) {
        const auto value = static_cast<unsigned char>(filename[at]);
        file.name[at] = value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
      }
      const std::string_view normalized(file.name.data(), filename.size());
      if (!course_review_detail::validName(normalized)) return CourseBaselineReviewResult::Corrupt;
      if (normalized.starts_with("pack-")) return CourseBaselineReviewResult::Corrupt;
      if (!course_review_detail::stableLearnerName(normalized)) return CourseBaselineReviewResult::Busy;
      if (unbound) {
        const auto length =
            snprintf(path.data(), path.size(), "/tinta/%.*s", static_cast<int>(filename.size()), filename.data());
        if (length <= 0 || static_cast<size_t>(length) >= path.size()) return CourseBaselineReviewResult::Corrupt;
      } else if (!courseStatePath(selectedCourse, filename, path)) {
        return CourseBaselineReviewResult::Corrupt;
      }
      const auto result = captureFile(CourseBaselineReviewDomain::Learner, normalized, path.data(), false);
      if (result != CourseBaselineReviewResult::Ok) return result;
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
    if (referencePresent && !referenceSeen) return CourseBaselineReviewResult::Corrupt;
    if (!count) return CourseBaselineReviewResult::Missing;
    return (!entry.isOpen() || entry.close()) && directory.close() && guard() ? CourseBaselineReviewResult::Ok
                                                                              : CourseBaselineReviewResult::IoError;
  }
  CourseBaselineReviewResult captureJournal() {
    journalPresent = false;
    if (!openDirectory(TRANSFER_DIRECTORY)) return CourseBaselineReviewResult::IoError;
    unsigned steps = 0;
    for (;;) {
      if (!guard()) return CourseBaselineReviewResult::Busy;
      if (entry.isOpen() && !entry.close()) return CourseBaselineReviewResult::IoError;
      const auto next = directory.nextEntry(entry);
      if (next == HalDirectoryResult::Error) return CourseBaselineReviewResult::IoError;
      if (next == HalDirectoryResult::End) break;
      std::string_view filename;
      if (!entryName(filename)) return CourseBaselineReviewResult::Corrupt;
      const auto match = hal_filename::compare(filename, "tinta-events", fold);
      if (match == hal_filename::Comparison::Invalid) return CourseBaselineReviewResult::Corrupt;
      if (match == hal_filename::Comparison::Equal) {
        if (journalPresent || !entry.isDirectory()) return CourseBaselineReviewResult::Corrupt;
        journalPresent = true;
      }
      char alias[13]{};
      if (!entry.getShortName(alias, sizeof(alias))) return CourseBaselineReviewResult::IoError;
      const auto aliasLength = strnlen(alias, sizeof(alias));
      const auto aliasMatch = aliasLength
                                  ? hal_filename::compare(std::string_view(alias, aliasLength), "tinta-events", fold)
                                  : hal_filename::Comparison::Different;
      if (aliasLength == sizeof(alias) || aliasMatch == hal_filename::Comparison::Invalid ||
          (aliasMatch == hal_filename::Comparison::Equal && match != hal_filename::Comparison::Equal))
        return CourseBaselineReviewResult::Corrupt;
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
    const bool entryClosed = !entry.isOpen() || entry.close();
    const bool directoryClosed = directory.close();
    if (!entryClosed || !directoryClosed) return CourseBaselineReviewResult::IoError;
    static constexpr std::string_view NAMES[] = {"events.bin", "header-a.bin", "header-b.bin"};
    static constexpr const char* PATHS[] = {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B};
    if (journalPresent) {
      if (!openDirectory(TINTA_JOURNAL_DIRECTORY)) return CourseBaselineReviewResult::IoError;
      for (;;) {
        if (!guard()) return CourseBaselineReviewResult::Busy;
        if (entry.isOpen() && !entry.close()) return CourseBaselineReviewResult::IoError;
        const auto next = directory.nextEntry(entry);
        if (next == HalDirectoryResult::Error) return CourseBaselineReviewResult::IoError;
        if (next == HalDirectoryResult::End) break;
        std::string_view filename;
        if (!entryName(filename) || entry.isDirectory()) return CourseBaselineReviewResult::Corrupt;
        bool known = false;
        for (const auto name : NAMES)
          if (hal_filename::compare(filename, name, fold) == hal_filename::Comparison::Equal) known = true;
        if (!known) return CourseBaselineReviewResult::Corrupt;
      }
      if (!closeReaders()) return CourseBaselineReviewResult::IoError;
    }
    for (unsigned at = 0; at < 3; ++at) {
      if (journalPresent) {
        const auto result = captureFile(CourseBaselineReviewDomain::Journal, NAMES[at], PATHS[at], true);
        if (result != CourseBaselineReviewResult::Ok) return result;
      } else {
        file = {};
        file.domain = CourseBaselineReviewDomain::Journal;
        std::copy(NAMES[at].begin(), NAMES[at].end(), file.name.begin());
        const auto result = insert();
        if (result != CourseBaselineReviewResult::Ok) return result;
      }
    }
    return CourseBaselineReviewResult::Ok;
  }
  CourseBaselineReviewResult captureFile(CourseBaselineReviewDomain domain, std::string_view filename,
                                         const char* source, bool optional) {
    // filename may borrow file.name; copy it before clearing the descriptor.
    std::array<char, 24> savedName{};
    if (!course_review_detail::validName(filename)) return CourseBaselineReviewResult::Corrupt;
    std::copy(filename.begin(), filename.end(), savedName.begin());
    file = {};
    file.domain = domain;
    file.name = savedName;
    if (!guard()) return CourseBaselineReviewResult::Busy;
    uint64_t size = 0;
    const auto status = metadata.stat(source, size);
    if (status == FileStatus::Error) return CourseBaselineReviewResult::IoError;
    if (status == FileStatus::Missing) return optional ? insert() : CourseBaselineReviewResult::Corrupt;
    if (size > UINT32_MAX || !Storage.openFileForReadReusing("COMPANION", source, reader) || !guard() ||
        reader.isDirectory())
      return CourseBaselineReviewResult::IoError;
    const bool hashed = hashInventoryFile(reader, io(), file.length, file.hash, progress, this);
    const bool synced = hashed && reader.sync(), closed = reader.close();
    if (!hashed || !synced || !closed || file.length != size || !guard()) return CourseBaselineReviewResult::IoError;
    file.present = true;
    return insert();
  }
  CourseBaselineReviewResult insert() {
    if (count >= COURSE_BASELINE_REVIEW_MAX_FILES) return CourseBaselineReviewResult::TooManyFiles;
    const std::string_view filename(file.name.data());
    size_t position = 0;
    for (; position < count; ++position) {
      const auto existing = scratch.subspan(60 + position * 68, 68);
      const std::string_view name(reinterpret_cast<const char*>(existing.data() + 4), existing[2]);
      if (existing[0] == static_cast<uint8_t>(file.domain) && name == filename)
        return CourseBaselineReviewResult::Corrupt;
      if (existing[0] > static_cast<uint8_t>(file.domain) ||
          (existing[0] == static_cast<uint8_t>(file.domain) && name > filename))
        break;
    }
    auto* destination = scratch.data() + 60 + position * 68;
    std::memmove(destination + 68, destination, (count - position) * 68);
    if (!encodeCourseBaselineReviewFile(file, {destination, 68})) return CourseBaselineReviewResult::Corrupt;
    ++count;
    return CourseBaselineReviewResult::Ok;
  }
  CourseBaselineReviewResult finish(CourseBaselineReviewResult result) {
    if (!closeReaders()) result = CourseBaselineReviewResult::IoError;
    if (!guard()) result = CourseBaselineReviewResult::Busy;
    capturing = false;
    ready = result == CourseBaselineReviewResult::Ok;
    if (!ready && result != CourseBaselineReviewResult::Missing)
      LOG_ERR("COMPANION", "Course baseline review refused: %u", static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
