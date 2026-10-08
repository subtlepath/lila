#pragma once

#include <mbedtls/sha256.h>

#include "CompanionContentRemoval.h"
#include "CompanionSingleFileRemovalPlan.h"
#include "HalInventoryFileHash.h"
#include "HalInventoryPathLookup.h"
#include "HalMultiPathRemovalPlanStorage.h"
#include "HalRemovalCohortAddress.h"

namespace companion {
class EpubRemovalReferences {
 public:
  virtual ~EpubRemovalReferences() = default;
  // Durably invalidate live references while retaining reading/bookmark history.
  virtual bool publish(const ContentRemovalRecord&, const char* originalPath) = 0;
  virtual bool verify(const ContentRemovalRecord&, const char* originalPath) = 0;
  virtual bool retire(const ContentRemovalRecord&, const char* originalPath) = 0;
  virtual bool verifyRetired(const ContentRemovalRecord&, const char* originalPath) = 0;
};
// Session-owned outside the stack. Scratch is borrowed and remains separate from
// retained plan/journal bytes. The caller authorizes ownership and excludes edits.
class HalEpubRemovalParticipant final : public ContentRemovalParticipant {
 public:
  HalEpubRemovalParticipant(ContentRemovalJournal& journal, EpubRemovalReferences& references,
                            std::span<uint8_t> scratch)
      : journal(journal), references(references), scratch(scratch) {
    mbedtls_sha256_init(&hashContext);
  }
  ~HalEpubRemovalParticipant() override {
    close();
    mbedtls_sha256_free(&hashContext);
  }
  bool bind(std::span<const uint8_t> bytes, const Digest& expectedHash) {
    if (configured || scratch.empty() || !decodeSingleFileRemovalPlan(bytes, decoded) ||
        decoded.request.manifest.kind != ContentKind::Epub)
      return failure("plan arguments");
    if (mbedtls_sha256_starts(&hashContext, 0) || mbedtls_sha256_update(&hashContext, bytes.data(), bytes.size()) ||
        mbedtls_sha256_finish(&hashContext, actual.data()) || actual != expectedHash)
      return failure("plan hash");
    seed.request = decoded.request;
    seed.planHash = expectedHash;
    if (!validContentRemovalRecord(seed)) return failure("plan ownership");
    std::copy(decoded.path.begin(), decoded.path.end(), original.begin());
    original[decoded.path.size()] = 0;
    decoded.path = {};
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-bytes-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 <= 112);
    std::copy_n(PREFIX, sizeof(PREFIX) - 1, backup.begin());
    size_t at = sizeof(PREFIX) - 1;
    for (const auto byte : expectedHash) {
      backup[at++] = HEX_DIGITS[byte >> 4];
      backup[at++] = HEX_DIGITS[byte & 15];
    }
    backup[at] = 0;
    configured = true;
    return true;
  }
  // Reuses this owner between serialized cohort paths; it does not undo IO.
  bool unbind() {
    configured = preflight = false;
    return close();
  }
  bool bindCohortPath(HalMultiPathRemovalPlanStorage& plans, const Digest& parent, uint64_t ordinal) {
    if (configured || scratch.empty() || !plans.current() || !plans.verifiedDigest() ||
        *plans.verifiedDigest() != parent || plans.current()->request.manifest.kind != ContentKind::Epub ||
        ordinal >= plans.current()->count)
      return failure("cohort plan arguments");
    seed.request = plans.current()->request;
    seed.planHash = parent;
    if (!validContentRemovalRecord(seed) ||
        (journal.current() && (journal.current()->request != seed.request || journal.current()->planHash != parent)) ||
        !plans.rewind())
      return failure("cohort ownership");
    for (uint64_t at = 0; at <= ordinal; ++at) {
      if (plans.next(original) != InventoryPathRecordResult::Entry) return failure("cohort path read");
    }
    if (!removalCohortAddress(hashContext, parent, ordinal, backup)) return failure("cohort address");
    configured = true;
    return true;
  }
  bool verifyPlan(const ContentRemovalRecord& record) override {
    if (!select(record, ContentRemovalPhase::Prepared, true) || !inspectPair()) return failure("verification");
    if (sourceStatus == FileStatus::Present && backupStatus == FileStatus::Missing) return verifyFile(original.data());
    return !preflight && backupStatus == FileStatus::Present && verifyFile(backup.data()) &&
           (sourceStatus == FileStatus::Missing || verifyFile(original.data()));
  }
  bool quarantine(const ContentRemovalRecord& record) override {
    if (!select(record, ContentRemovalPhase::Prepared) || !inspectPair()) return failure("verification");
    if (backupStatus == FileStatus::Present) {
      if (!verifyFile(backup.data())) return failure("verification");
      if (sourceStatus == FileStatus::Missing) return guard();
      if (!verifyFile(original.data()) || !guard() || !Storage.remove(original.data()))
        return failure("duplicate source removal");
    } else {
      if (sourceStatus != FileStatus::Present || !verifyFile(original.data()) || !guard() ||
          !Storage.rename(original.data(), backup.data()))
        return failure("quarantine rename");
    }
    return guard() && inspectPair() && sourceStatus == FileStatus::Missing && backupStatus == FileStatus::Present &&
           verifyFile(backup.data());
  }
  bool verifyQuarantined(const ContentRemovalRecord& record) override {
    if (!select(record, record.phase) || record.phase > ContentRemovalPhase::Quarantined || !inspectPair())
      return failure("verification");
    return sourceStatus == FileStatus::Missing && backupStatus == FileStatus::Present && verifyFile(backup.data());
  }
  bool publishRemoval(const ContentRemovalRecord& record) override {
    return record.phase == ContentRemovalPhase::Quarantined && verifyQuarantined(record) && guard() &&
           references.publish(record, original.data()) && guard();
  }
  bool verifyPublished(const ContentRemovalRecord& record) override {
    if (!select(record, record.phase) ||
        (record.phase != ContentRemovalPhase::Quarantined && record.phase != ContentRemovalPhase::Committed) ||
        !inspectPair())
      return failure("verification");
    if (sourceStatus != FileStatus::Missing || (backupStatus == FileStatus::Present && !verifyFile(backup.data())))
      return failure("verification");
    if (record.phase == ContentRemovalPhase::Quarantined && backupStatus != FileStatus::Present)
      return failure("verification");
    return guard() && references.verify(record, original.data()) && guard();
  }
  bool retireBackups(const ContentRemovalRecord& record) override {
    if (record.phase != ContentRemovalPhase::Committed || !verifyPublished(record)) return failure("verification");
    if (!guard() || !references.retire(record, original.data()) || !guard() ||
        !references.verifyRetired(record, original.data()) || !guard())
      return failure("reference retirement");
    if (backupStatus == FileStatus::Missing) return guard();
    if (!guard() || !Storage.remove(backup.data())) return failure("backup retirement");
    return guard() && inspectPair() && sourceStatus == FileStatus::Missing && backupStatus == FileStatus::Missing;
  }
  bool verifyRetired(const ContentRemovalRecord& record) override {
    return select(record, record.phase) && record.phase >= ContentRemovalPhase::Committed && inspectPair() &&
           sourceStatus == FileStatus::Missing && backupStatus == FileStatus::Missing && guard() &&
           references.verifyRetired(record, original.data()) && guard();
  }

 private:
  ContentRemovalJournal& journal;
  EpubRemovalReferences& references;
  std::span<uint8_t> scratch;
  HalInventoryPathLookup lookup;
  HalFile file;
  mbedtls_sha256_context hashContext;
  Digest actual{};
  ContentRemovalRecord seed, checkpoint;
  SingleFileRemovalPlan decoded;
  std::array<char, 512> original{};
  std::array<char, 112> backup{};
  FileStatus sourceStatus = FileStatus::Error, backupStatus = FileStatus::Error;
  bool configured = false, preflight = false;
  bool select(const ContentRemovalRecord& record, ContentRemovalPhase phase, bool allowPreflight = false) {
    if (!configured || !validContentRemovalRecord(record) || record.request != seed.request ||
        record.planHash != seed.planHash || record.phase != phase)
      return failure("context");
    checkpoint = record;
    preflight = allowPreflight && !journal.current();
    return guard();
  }
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  bool inspectPair() {
    if (!guard() || !close()) return failure("verification");
    uint64_t size = 0;
    sourceStatus = lookup.stat(original.data(), size);
    if (!guard() || sourceStatus == FileStatus::Error) return failure("verification");
    backupStatus = lookup.stat(backup.data(), size);
    return guard() && backupStatus != FileStatus::Error;
  }
  bool verifyFile(const char* path) {
    if (!guard() || !close() || !Storage.openFileForReadReusing("COMPANION", path, file)) return failure("hash open");
    uint64_t length = 0;
    const bool hashed = hashInventoryFile(
        file, scratch, length, actual,
        [](void* context) { return static_cast<HalEpubRemovalParticipant*>(context)->guard(); }, this);
    const bool synced = hashed && file.sync();
    const bool closed = close();
    return (hashed && synced && closed && guard() && length == seed.request.manifest.length &&
            actual == seed.request.manifest.contentHash) ||
           failure("file proof");
  }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "EPUB removal %s failed", operation);
    return false;
  }
};
}  // namespace companion
