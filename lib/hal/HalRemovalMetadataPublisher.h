#pragma once

#include "CompanionContentRemovalJournal.h"
#include "CompanionRemovalMetadataSnapshot.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryPathLookup.h"
#include "HalRemovalMetadataPaths.h"

namespace companion {
// The provider must verify an immutable durable snapshot declaration and the
// current removal journal before each call. It excludes all metadata writers.
using RemovalMetadataAuthorization = bool (*)(void*, const RemovalMetadataSnapshot&);
// Session-owned outside the task stack. Borrowed scratch is separate from the
// provider's declaration; retained HAL handles are allocated once and reused.
class HalRemovalMetadataPublisher final {
 public:
  HalRemovalMetadataPublisher(std::span<uint8_t> scratch, RemovalMetadataAuthorization authorize, void* context)
      : scratch(scratch), authorize(authorize), context(context) {}
  ~HalRemovalMetadataPublisher() { close(); }
  bool bind(const RemovalMetadataSnapshot& declaration) {
    if (bound || !authorize || scratch.empty() || !validRemovalMetadataSnapshot(declaration))
      return fail("declaration");
    expected = declaration;
    target = expected.file == RemovalMetadataFile::State ? "/.crosspoint/state.json" : "/.crosspoint/recent.json";
    if (!removalMetadataPaths(expected.planHash, expected.file, candidate, backup)) return fail("paths");
    bound = true;
    return guard() || fail("authorization");
  }
  const char* candidatePath() const { return bound ? candidate.data() : nullptr; }
  const char* backupPath() const { return bound ? backup.data() : nullptr; }
  bool publish() {
    if (!inspect()) return fail("lookup");
    if (unchangedRemovalMetadataSnapshot(expected)) return verifyPublished();
    if (!expected.previousLength) {
      if (b != FileStatus::Missing) return fail("unexpected backup");
      if (a == FileStatus::Present && c == FileStatus::Missing) return verifyPublished();
      if (a != FileStatus::Missing || c != FileStatus::Present || !verify(candidate.data(), false))
        return fail("initial publication state");
    } else if (a == FileStatus::Present && c == FileStatus::Present && b == FileStatus::Missing) {
      if (!verify(target, true) || !verify(candidate.data(), false) || !guard() ||
          !Storage.rename(target, backup.data()))
        return fail("backup rename");
    } else if (a == FileStatus::Missing && c == FileStatus::Present && b == FileStatus::Present) {
      if (!verify(backup.data(), true) || !verify(candidate.data(), false)) return fail("recovery hashes");
    } else if (a == FileStatus::Present && c == FileStatus::Missing && b == FileStatus::Present) {
      return verifyPublished();
    } else {
      return fail("publication state");
    }
    if (!guard() || !Storage.rename(candidate.data(), target)) return fail("candidate rename");
    return verifyPublished();
  }
  // Backup remains until the enclosing removal transaction commits. This
  // primitive never deletes either generation or its durable authorization.
  bool verifyPublished() {
    const bool unchanged = unchangedRemovalMetadataSnapshot(expected);
    if (!inspect() || a != FileStatus::Present || c != FileStatus::Missing ||
        b != (expected.previousLength && !unchanged ? FileStatus::Present : FileStatus::Missing) ||
        !verify(target, false) || (expected.previousLength && !unchanged && !verify(backup.data(), true)))
      return fail("published verification");
    return guard() || fail("published ownership");
  }

  bool verifyRetiring(const ContentRemovalRecord& record, ContentRemovalJournal& journal) {
    if (!retirementContext(record, journal, false) || !inspect() || a != FileStatus::Present ||
        c != FileStatus::Missing || !verify(target, false) ||
        (b == FileStatus::Present && (unchangedRemovalMetadataSnapshot(expected) || !verify(backup.data(), true))))
      return fail("retirement verification");
    return guard() || fail("retirement ownership");
  }
  bool retire(const ContentRemovalRecord& record, ContentRemovalJournal& journal) {
    if (!verifyRetiring(record, journal)) return false;
    if (b == FileStatus::Present && (!guard() || !Storage.remove(backup.data()) || !guard()))
      return fail("backup retirement");
    return verifyRetired(record, journal);
  }
  bool verifyRetired(const ContentRemovalRecord& record, ContentRemovalJournal& journal) {
    if (!retirementContext(record, journal, true) || !inspect() || a != FileStatus::Present ||
        b != FileStatus::Missing || c != FileStatus::Missing || !verify(target, false))
      return fail("retired verification");
    return guard() || fail("retired ownership");
  }

 private:
  RemovalMetadataSnapshot expected;
  std::span<uint8_t> scratch;
  RemovalMetadataAuthorization authorize;
  void* context;
  const char* target = nullptr;
  std::array<char, 112> candidate{}, backup{};
  HalInventoryPathLookup lookup;
  HalFile file;
  Digest actual{};
  FileStatus a = FileStatus::Error, b = FileStatus::Error, c = FileStatus::Error;
  bool bound = false;
  ContentRemovalJournal* retirementJournal = nullptr;
  ContentRemovalRecord retirementCheckpoint;
  bool retirementContext(const ContentRemovalRecord& record, ContentRemovalJournal& journal, bool retiredAllowed) {
    if (!validContentRemovalRecord(record) || record.request != expected.request ||
        record.planHash != expected.planHash ||
        (record.phase != ContentRemovalPhase::Committed &&
         (!retiredAllowed || record.phase != ContentRemovalPhase::Retired)) ||
        !journal.current() || *journal.current() != record)
      return fail("retirement context");
    retirementJournal = &journal;
    retirementCheckpoint = record;
    return true;
  }
  bool guard() const {
    return bound && authorize(context, expected) &&
           (!retirementJournal ||
            (retirementJournal->current() && *retirementJournal->current() == retirementCheckpoint));
  }
  bool inspect() {
    if (!guard() || !close()) return false;
    uint64_t length = 0;
    a = lookup.stat(target, length);
    if (!guard() || a == FileStatus::Error) return false;
    b = lookup.stat(backup.data(), length);
    if (!guard() || b == FileStatus::Error) return false;
    c = lookup.stat(candidate.data(), length);
    return guard() && c != FileStatus::Error;
  }
  bool verify(const char* path, bool previous) {
    if (!guard() || !close() || !Storage.openFileForReadReusing("COMPANION", path, file)) return fail("hash open");
    const uint64_t requiredLength = previous ? expected.previousLength : expected.nextLength;
    if (file.isDirectory() || file.fileSize64() != requiredLength) {
      close();
      return fail("hash extent");
    }
    uint64_t length = 0;
    const bool hashed = hashInventoryFile(
        file, scratch, length, actual,
        [](void* context) { return static_cast<HalRemovalMetadataPublisher*>(context)->guard(); }, this);
    const bool synced = hashed && file.sync();
    const bool closed = close();
    if (!hashed || !synced || !closed || !guard() || length != requiredLength ||
        actual != (previous ? expected.previousHash : expected.nextHash))
      return fail("hash verification");
    return true;
  }
  bool close() { return !file.isOpen() || file.close() || fail("close"); }
  static bool fail(const char* reason) {
    LOG_ERR("COMPANION", "Removal metadata publication failed: %s", reason);
    return false;
  }
};
}  // namespace companion
