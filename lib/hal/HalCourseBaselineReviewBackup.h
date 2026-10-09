#pragma once

#include "HalCourseBaselineReviewCapture.h"
#include "HalCourseBaselineReviewStore.h"

namespace companion {
// Admit off stack. Native readiness and exclusion of all state/namespace writers
// are caller obligations. Scratch remains borrowed until the next operation.
class HalCourseBaselineReviewBackup final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineReviewBackup(std::span<uint8_t> scratch, Permission permitted, void* context)
      : scratch(scratch),
        permitted(permitted),
        context(context),
        metadata(permitted, context),
        store(io(), permitted, context),
        capture(scratch, permitted, context) {}
  ~HalCourseBaselineReviewBackup() { closeReaders(); }
  HalCourseBaselineReviewBackup(const HalCourseBaselineReviewBackup&) = delete;
  HalCourseBaselineReviewBackup& operator=(const HalCourseBaselineReviewBackup&) = delete;
  bool preserve(const Digest& reviewHash, const Identity& reader, const Identity& generation, const Identity& course) {
    if (operating) return failure("reentry");
    ready = false;
    if (scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)))
      return failure("workspace");
    operating = true;
    selectedHash = reviewHash;
    selectedReader = reader;
    selectedGeneration = generation;
    selectedCourse = course;
    if (!closeReaders() || !guard() ||
        store.open(selectedHash, selectedReader, selectedGeneration, selectedCourse,
                   scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE)) != CourseBaselineReviewStoreResult::Ok ||
        !currentReview())
      return finish(false);
    CourseBaselineReviewView view;
    if (!view.decode(capture.bytes())) return finish(false);
    for (size_t index = 0; index < view.count(); ++index) {
      const auto entry = view.entry(index);
      paths(index);
      if (!guard()) return finish(false);
      uint64_t size = 0;
      const auto destination = metadata.stat(backup.data(), size);
      const auto pending = metadata.stat(stage.data(), size);
      if (destination == FileStatus::Error || pending == FileStatus::Error) return finish(false);
      if (!entry[1]) {
        if (destination != FileStatus::Missing || pending != FileStatus::Missing) return finish(false);
        continue;
      }
      const auto length = course_review_detail::number(entry, 28, 8);
      std::copy_n(entry.begin() + 36, expectedFileHash.size(), expectedFileHash.begin());
      if (!sourcePath(entry)) return finish(false);
      if (destination == FileStatus::Present) {
        if (pending != FileStatus::Missing || !verified(backup.data(), length)) return finish(false);
        continue;
      }
      if (!copy(length, pending == FileStatus::Present)) return finish(false);
    }
    // No complete loan until both native recapture and every copied file agree.
    return finish(currentReview());
  }
  // Historical evidence only: no live-state validation or current-state loan.
  bool verifyStored(const Digest& reviewHash, const Identity& reader, const Identity& generation,
                    const Identity& course) {
    if (operating) return failure("reentry");
    ready = false;
    if (scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)))
      return failure("workspace");
    operating = true;
    selectedHash = reviewHash;
    selectedReader = reader;
    selectedGeneration = generation;
    selectedCourse = course;
    if (!closeReaders() || !guard() ||
        store.open(selectedHash, selectedReader, selectedGeneration, selectedCourse,
                   scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE)) != CourseBaselineReviewStoreResult::Ok)
      return finish(false);
    const auto count = course_review_detail::number(scratch, 56, 2);
    CourseBaselineReviewView view;
    if (count > COURSE_BASELINE_REVIEW_MAX_FILES ||
        !view.decode(scratch.first(64 + count * COURSE_BASELINE_REVIEW_ENTRY_SIZE)))
      return finish(false);
    for (size_t index = 0; index < view.count(); ++index) {
      const auto record = view.entry(index);
      paths(index);
      uint64_t size = 0;
      const auto destination = metadata.stat(backup.data(), size);
      if (!guard() || metadata.stat(stage.data(), size) != FileStatus::Missing) return finish(false);
      if (!record[1]) {
        if (destination != FileStatus::Missing) return finish(false);
        continue;
      }
      std::copy_n(record.begin() + 36, expectedFileHash.size(), expectedFileHash.begin());
      if (destination != FileStatus::Present || !verified(backup.data(), course_review_detail::number(record, 28, 8)))
        return finish(false);
    }
    return finish(true, false);
  }
  bool complete() const {
    if (!guard()) ready = false;
    return ready;
  }
  bool closeReaders() {
    ready = false;
    const bool sourceClosed = !source.isOpen() || source.close();
    const bool fileClosed = !file.isOpen() || file.close();
    const bool entryClosed = !entry.isOpen() || entry.close();
    const bool directoryClosed = !directory.isOpen() || directory.close();
    const bool metadataClosed = metadata.closeReaders(), storeClosed = store.closeReaders(),
               captureClosed = capture.closeReaders();
    return sourceClosed && fileClosed && entryClosed && directoryClosed && metadataClosed && storeClosed &&
           captureClosed;
  }

 private:
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalCourseBaselineReviewStore store;
  HalCourseBaselineReviewCapture capture;
  HalFile source, file, directory, entry;
  std::array<char, 112> backup{}, original{}, parent{};
  std::array<char, 116> stage{};
  std::array<char, INVENTORY_PATH_LIMIT + 1> name{};
  Digest selectedHash{}, expectedFileHash{};
  Identity selectedReader{}, selectedGeneration{}, selectedCourse{};
  bool operating = false;
  mutable bool ready = false;
  std::span<uint8_t> io() const {
    return scratch.size() > COURSE_BASELINE_REVIEW_MAX_SIZE ? scratch.subspan(COURSE_BASELINE_REVIEW_MAX_SIZE)
                                                            : std::span<uint8_t>{};
  }
  bool guard() const { return permitted && permitted(context); }
  bool currentReview() {
    return guard() &&
           capture.capture(selectedReader, selectedGeneration, selectedCourse) == CourseBaselineReviewResult::Ok &&
           capture.hash() && *capture.hash() == selectedHash;
  }
  void paths(size_t index) {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-review-state-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 64 + 4 <= 112);
    std::copy(PREFIX.begin(), PREFIX.end(), backup.begin());
    size_t at = PREFIX.size();
    for (const auto byte : selectedHash) {
      backup[at++] = HEX_DIGITS[byte >> 4];
      backup[at++] = HEX_DIGITS[byte & 15];
    }
    backup[at++] = '-';
    backup[at++] = HEX_DIGITS[(index >> 4) & 15];
    backup[at++] = HEX_DIGITS[index & 15];
    backup[at] = 0;
    std::copy_n(backup.begin(), at, stage.begin());
    static constexpr char SUFFIX[] = ".tmp";
    std::copy_n(SUFFIX, sizeof(SUFFIX), stage.begin() + at);
  }
  bool sourcePath(std::span<const uint8_t> record) {
    if (record[0] == static_cast<uint8_t>(CourseBaselineReviewDomain::Learner)) {
      std::copy_n(record.begin() + 4, record[2], name.begin());
      name[record[2]] = 0;
      if (!courseStatePath(selectedCourse, name.data(), original)) return false;
    } else {
      static constexpr const char* JOURNAL[] = {TINTA_JOURNAL_EVENTS, TINTA_JOURNAL_HEADER_A, TINTA_JOURNAL_HEADER_B};
      static constexpr std::string_view JOURNAL_NAMES[] = {"events.bin", "header-a.bin", "header-b.bin"};
      static constexpr const char* PROOFS[] = {COURSE_MARK_MIGRATION_PATHS.done, COURSE_MARK_MIGRATION_PATHS.intent,
                                               COURSE_STATE_MIGRATION_PATHS.done, COURSE_STATE_MIGRATION_PATHS.intent};
      static constexpr std::string_view PROOF_NAMES[] = {"mark-done", "mark-intent", "state-done", "state-intent"};
      const std::string_view wanted(reinterpret_cast<const char*>(record.data() + 4), record[2]);
      const char* path = nullptr;
      if (record[0] == static_cast<uint8_t>(CourseBaselineReviewDomain::Journal)) {
        for (size_t i = 0; i < 3; ++i)
          if (wanted == JOURNAL_NAMES[i]) path = JOURNAL[i];
      } else {
        for (size_t i = 0; i < 4; ++i)
          if (wanted == PROOF_NAMES[i]) path = PROOFS[i];
      }
      if (!path || strnlen(path, original.size()) >= original.size()) return false;
      std::copy_n(path, strlen(path) + 1, original.begin());
    }
    uint64_t length = 0;
    if (metadata.stat(original.data(), length) != FileStatus::Present) return false;
    // Preserve actual spelling for HALs whose path lookup is case-sensitive.
    const std::string_view path(original.data());
    const auto separator = path.find_last_of('/');
    const auto wanted = path.substr(separator + 1);
    std::copy_n(original.begin(), separator, parent.begin());
    parent[separator] = 0;
    if (!guard() || !Storage.openFileForReadReusing("COMPANION", parent.data(), directory) ||
        !directory.isDirectory() || !entry.prepareDirectoryEntry())
      return false;
    bool found = false;
    unsigned steps = 0;
    for (;;) {
      if (!guard() || (entry.isOpen() && !entry.close())) return false;
      const auto result = directory.nextEntry(entry);
      if (result == HalDirectoryResult::Error) return false;
      if (result == HalDirectoryResult::End) break;
      const auto count = entry.getName(name.data(), name.size());
      if (!count || count >= name.size() || name[count] != 0) return false;
      const auto lower = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
      if (count == wanted.size() && std::equal(wanted.begin(), wanted.end(), name.begin(),
                                               [lower](char a, char b) { return lower(a) == lower(b); })) {
        if (found || entry.isDirectory() || separator + 1 + count >= original.size()) return false;
        found = true;
        // Updating after the scan avoids changing the borrowed wanted name.
        std::copy_n(name.begin(), count + 1, parent.begin() + separator + 1);
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
    const bool entryClosed = !entry.isOpen() || entry.close(), directoryClosed = directory.close();
    if (!found || !entryClosed || !directoryClosed || !guard()) return false;
    parent[separator] = '/';
    original = parent;
    return true;
  }
  bool matches(HalFile& handle, uint64_t length) {
    Digest actual{};
    uint64_t actualLength = 0;
    return guard() && handle.isOpen() && !handle.isDirectory() && handle.fileSize64() == length &&
           hashInventoryFile(handle, io(), actualLength, actual, permitted, context) && actualLength == length &&
           actual == expectedFileHash && handle.sync() && guard();
  }
  bool verified(const char* path, uint64_t length) {
    if (!guard() || !Storage.openFileForReadReusing("COMPANION", path, file)) return false;
    const bool valid = matches(file, length), closed = file.close();
    return valid && closed && guard();
  }
  bool copy(uint64_t length, bool pending) {
    if (!guard() || !Storage.openFileForReadReusing("COMPANION", original.data(), source) || !matches(source, length) ||
        !source.seek64(0))
      return false;
    uint64_t offset = 0;
    if (pending) {
      if (!Storage.openFileForReadReusing("COMPANION", stage.data(), file) || file.isDirectory()) return false;
      offset = file.fileSize64();
      if (offset > length) return false;
      const auto half = io().size() / 2;
      for (uint64_t at = 0; at < offset;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(half, offset - at));
        if (!guard() || file.fileSize64() != offset || file.read(io().data(), count) != static_cast<int>(count) ||
            source.read(io().data() + half, count) != static_cast<int>(count) ||
            memcmp(io().data(), io().data() + half, count) != 0)
          return false;
        at += count;
        vTaskDelay(1);
      }
      const bool synced = file.sync(), closed = file.close();
      if (!synced || !closed || !guard()) return false;
    }
    if (offset != length || !pending) {
      if (pending)
        file = Storage.open(stage.data(), O_WRONLY);
      else if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file))
        return false;
      if (!file || file.isDirectory() || file.fileSize64() != offset || !file.seek64(offset)) return false;
      for (uint64_t at = offset; at < length;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(io().size(), length - at));
        if (!guard() || source.read(io().data(), count) != static_cast<int>(count) ||
            file.write(io().data(), count) != count)
          return false;
        at += count;
        vTaskDelay(1);
      }
      const bool synced = file.truncate(length) && file.sync(), closed = file.close();
      if (!synced || !closed || !guard()) return false;
    }
    const bool sourceMatches = matches(source, length), sourceClosed = source.close();
    if (!sourceMatches || !sourceClosed || !verified(stage.data(), length)) return false;
    uint64_t size = 0;
    return metadata.stat(backup.data(), size) == FileStatus::Missing && guard() &&
           Storage.rename(stage.data(), backup.data()) && verified(backup.data(), length);
  }
  bool finish(bool result, bool offerLoan = true) {
    const bool closed = closeReaders();
    operating = false;
    const bool complete = result && closed && guard();
    ready = offerLoan && complete;
    return complete || failure("preservation");
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Course review backup %s refused", operation);
    return false;
  }
};
}  // namespace companion
