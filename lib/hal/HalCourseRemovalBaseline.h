#pragma once

#include "CompanionCourseRemovalProof.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryPathLookup.h"
#include "HalRemovalCohortAddress.h"

namespace companion {
// Retain off stack after heap admission. The caller verifies the sealed plan,
// excludes namespace writers and lends disjoint hashing scratch for this owner.
class HalCourseRemovalBaseline final {
 public:
  HalCourseRemovalBaseline(ContentRemovalJournal& journal, std::span<uint8_t> scratch,
                           InventoryHashProgress permitted = nullptr, void* context = nullptr)
      : journal(journal), scratch(scratch), permitted(permitted), context(context), lookup(progress, this) {
    mbedtls_sha256_init(&hash);
  }
  ~HalCourseRemovalBaseline() {
    closeReaders();
    mbedtls_sha256_free(&hash);
  }
  HalCourseRemovalBaseline(const HalCourseRemovalBaseline&) = delete;
  HalCourseRemovalBaseline& operator=(const HalCourseRemovalBaseline&) = delete;
  const char* path() const { return verified && guard() ? cached.data() : nullptr; }
  bool closeReaders() { return !file.isOpen() || file.close() || fail("close"); }
  bool retain(const ContentRemovalRecord& record, const ContentRemovalRecord& proof) {
    verified = false;
    if (record.phase != ContentRemovalPhase::Committed || !select(record, proof)) return fail("retention owner");
    uint64_t size = 0;
    const auto cache = lookup.stat(cached.data(), size);
    if (!guard() || cache == FileStatus::Error) return fail("cache stat");
    const auto saved = lookup.stat(backup.data(), size);
    if (!guard() || saved == FileStatus::Error) return fail("backup stat");
    if (cache == FileStatus::Present) {
      if (!verifyFile(cached.data()) || (saved == FileStatus::Present && !verifyFile(backup.data())))
        return fail("retained proof");
    } else {
      if (saved != FileStatus::Present || !verifyFile(backup.data()) || !guard() ||
          lookup.stat(cached.data(), size) != FileStatus::Missing || !guard() ||
          !Storage.rename(backup.data(), cached.data()) || !guard() || !verifyFile(cached.data()))
        return fail("baseline rename");
    }
    verified = guard();
    return verified;
  }
  bool verify(const ContentRemovalRecord& record, const ContentRemovalRecord& proof) {
    verified = false;
    if (record.phase < ContentRemovalPhase::Committed || !select(record, proof) || !verifyFile(cached.data()))
      return fail("baseline verification");
    verified = guard();
    return verified;
  }
  bool verifyCompleted(const ContentRemovalRecord& proof, const ContentRemovalRecord& completed,
                       const ContentManifest& binding, const Identity& generation) {
    verified = false;
    if (!completedCourseRemovalProof(proof, completed, binding, generation) ||
        journal.recover(generation) != ContentRemovalJournalResult::Missing || !select(completed, proof, true))
      return fail("completed owner");
    uint64_t size = 0;
    if (lookup.stat(ACTIVE_COURSE_PATH, size) != FileStatus::Missing || !guard() ||
        lookup.stat(backup.data(), size) != FileStatus::Missing || !guard() || !verifyFile(cached.data()))
      return fail("completed baseline");
    verified = guard();
    return verified;
  }

 private:
  ContentRemovalJournal& journal;
  std::span<uint8_t> scratch;
  InventoryHashProgress permitted;
  void* context;
  HalInventoryPathLookup lookup;
  HalFile file;
  mbedtls_sha256_context hash;
  ContentRemovalRecord checkpoint;
  Digest actual{};
  std::array<char, COURSE_REMOVAL_CACHE_PATH_CAPACITY> cached{};
  std::array<char, 112> backup{};
  bool verified = false, completedMode = false;
  bool guard() const {
    return (completedMode ? !journal.current() : journal.current() && *journal.current() == checkpoint) &&
           (!permitted || permitted(context));
  }
  bool select(const ContentRemovalRecord& record, const ContentRemovalRecord& proof, bool complete = false) {
    verified = false;
    if (scratch.empty() || !matchesCourseRemovalProof(proof, record) || !closeReaders()) return false;
    checkpoint = record;
    completedMode = complete;
    return guard() && courseRemovalCachePath(proof, cached) && removalCohortAddress(hash, record.planHash, 0, backup) &&
           guard();
  }
  bool verifyFile(const char* path) {
    uint64_t storedLength = 0;
    if (!guard() || !closeReaders() || lookup.stat(path, storedLength) != FileStatus::Present || !guard() ||
        storedLength != checkpoint.request.manifest.length || !Storage.openFileForReadReusing("COMPANION", path, file))
      return fail("file open");
    uint64_t length = 0;
    const bool hashed = hashInventoryFile(file, scratch, length, actual, progress, this);
    const bool synced = hashed && file.sync();
    const bool closed = closeReaders();
    return (hashed && synced && closed && guard() && length == checkpoint.request.manifest.length &&
            actual == checkpoint.request.manifest.contentHash) ||
           fail("file SHA");
  }
  static bool progress(void* context) { return static_cast<HalCourseRemovalBaseline*>(context)->guard(); }
  static bool fail(const char* reason) {
    LOG_ERR("COMPANION", "Course removal baseline %s failed", reason);
    return false;
  }
};
}  // namespace companion
