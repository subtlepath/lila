#pragma once

#include "CompanionCourseRemovalProof.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class CourseRemovalProofStorageResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
// Retain off stack. Encoding scratch is borrowed and disjoint from journal bytes.
// This store publishes proof metadata; pack/state verification belongs to its caller.
class HalCourseRemovalProofStorage final {
 public:
  using Permission = bool (*)(void*);
  explicit HalCourseRemovalProofStorage(std::span<uint8_t> scratch, Permission permitted = nullptr,
                                        void* context = nullptr)
      : scratch(scratch), permitted(permitted), context(context), lookup(progress, this) {}
  ~HalCourseRemovalProofStorage() { close(); }
  bool closeReaders() { return close(); }
  CourseRemovalProofStorageResult load(const Identity& generation, ContentRemovalRecord& output) {
    if (scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || generation == Identity{})
      return CourseRemovalProofStorageResult::Invalid;
    if (!allowed() || !prepare()) return error("load preparation");
    const auto result = read(COURSE_REMOVAL_PROOF_PATH);
    if (!allowed()) return CourseRemovalProofStorageResult::Conflict;
    if (result != CourseRemovalProofStorageResult::Ok) return result;
    if (decoded.request.generation != generation) return CourseRemovalProofStorageResult::Conflict;
    output = decoded;
    return CourseRemovalProofStorageResult::Ok;
  }
  CourseRemovalProofStorageResult persist(const ContentRemovalRecord& record, ContentRemovalJournal& journal) {
    if (!validCourseRemovalProof(record) || scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !journal.current() ||
        *journal.current() != record)
      return CourseRemovalProofStorageResult::Invalid;
    expected = record;
    owner = &journal;
    if (!prepare() || !guard()) return error("persist preparation");
    auto result = read(COURSE_REMOVAL_PROOF_PATH);
    if (!guard()) return CourseRemovalProofStorageResult::Conflict;
    if (result == CourseRemovalProofStorageResult::Ok)
      return decoded == expected ? result : CourseRemovalProofStorageResult::Conflict;
    if (result != CourseRemovalProofStorageResult::Missing) return result;
    result = read(COURSE_REMOVAL_PROOF_STAGE);
    if (!guard()) return CourseRemovalProofStorageResult::Conflict;
    if (result == CourseRemovalProofStorageResult::Ok && decoded != expected)
      return CourseRemovalProofStorageResult::Conflict;
    if (result != CourseRemovalProofStorageResult::Missing && result != CourseRemovalProofStorageResult::Corrupt &&
        result != CourseRemovalProofStorageResult::Ok)
      return result;
    if (encodeContentRemovalRecord(expected, scratch) != CONTENT_REMOVAL_RECORD_SIZE || !guard() ||
        !Storage.openFileForWriteReusing("COMPANION", COURSE_REMOVAL_PROOF_STAGE, file))
      return error("stage open/encode");
    const bool written = !file.isDirectory() &&
                         file.write(scratch.data(), CONTENT_REMOVAL_RECORD_SIZE) == CONTENT_REMOVAL_RECORD_SIZE &&
                         file.truncate(CONTENT_REMOVAL_RECORD_SIZE) && file.sync();
    const bool closed = close();
    if (!written || !closed || !guard()) return error("stage write/sync");
    result = read(COURSE_REMOVAL_PROOF_STAGE);
    if (result != CourseRemovalProofStorageResult::Ok || decoded != expected || !guard())
      return error("stage readback");
    if (lookup.inspect(COURSE_REMOVAL_PROOF_PATH) != CompanionFilePresence::Missing || !guard() ||
        !Storage.rename(COURSE_REMOVAL_PROOF_STAGE, COURSE_REMOVAL_PROOF_PATH))
      return error("publication rename");
    result = read(COURSE_REMOVAL_PROOF_PATH);
    if (result != CourseRemovalProofStorageResult::Ok || decoded != expected || !guard())
      return error("publication readback");
    return CourseRemovalProofStorageResult::Ok;
  }

 private:
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  ContentRemovalRecord expected, decoded;
  ContentRemovalJournal* owner = nullptr;
  HalCompanionFileLookup lookup;
  HalFile file;
  bool allowed() const { return !permitted || permitted(context); }
  static bool progress(void* context) { return static_cast<HalCourseRemovalProofStorage*>(context)->allowed(); }
  bool guard() const { return allowed() && owner && owner->current() && *owner->current() == expected; }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool prepare() {
    return close() && allowed() && Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) && allowed();
  }
  CourseRemovalProofStorageResult read(const char* path) {
    if (!close()) return error("read close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return CourseRemovalProofStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return error("read open");
    if (file.isDirectory() || file.fileSize64() > CONTENT_REMOVAL_RECORD_SIZE) {
      close();
      return error("receipt collision");
    }
    const bool exact = file.fileSize64() == CONTENT_REMOVAL_RECORD_SIZE;
    if (exact &&
        file.read(scratch.data(), CONTENT_REMOVAL_RECORD_SIZE) != static_cast<int>(CONTENT_REMOVAL_RECORD_SIZE)) {
      close();
      return error("receipt read");
    }
    const bool valid = exact && decodeContentRemovalRecord(scratch.first(CONTENT_REMOVAL_RECORD_SIZE), decoded);
    const bool synced = valid && file.sync();
    const bool closed = close();
    if (!closed || (valid && !synced)) return error("receipt sync/close");
    if (!valid) return CourseRemovalProofStorageResult::Corrupt;
    return validCourseRemovalProof(decoded) ? CourseRemovalProofStorageResult::Ok
                                            : CourseRemovalProofStorageResult::Conflict;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Course removal proof storage failed: %s", reason);
    return false;
  }
  static CourseRemovalProofStorageResult error(const char* reason) {
    failure(reason);
    return CourseRemovalProofStorageResult::IoError;
  }
};
}  // namespace companion
