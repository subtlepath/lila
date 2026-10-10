#pragma once

#include <span>
#include <string_view>

#include "CompanionTransferDeclaration.h"

namespace companion {
enum class TransferRecoveryMode { CompleteInstallation, DeferDictionaryInstallation, InspectJournal };

inline constexpr char TRANSFER_DIRECTORY[] = "/.crosspoint/companion";
inline constexpr char TRANSFER_STAGE[] = "/.crosspoint/companion/incoming";
inline constexpr char TRANSFER_BACKUP[] = "/.crosspoint/companion/backup";
inline constexpr char TRANSFER_JOURNALS[2][40] = {"/.crosspoint/companion/transfer-a",
                                                  "/.crosspoint/companion/transfer-b"};
inline constexpr size_t TRANSFER_LEGACY_JOURNAL_SIZE = 244;
inline constexpr size_t TRANSFER_JOURNAL_SIZE = 308;
inline constexpr size_t TRANSFER_TARGET_SIZE = 128;

enum class FileStatus : uint8_t { Present, Missing, Error };

// All successful mutations must be synced before returning. The firmware
// implementation uses HalStorage; the host fake injects interrupted operations.
class TransferStorage {
 public:
  virtual ~TransferStorage() = default;
  virtual bool prepare() = 0;
  virtual FileStatus stat(const char* path, uint64_t& size) = 0;
  virtual bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) = 0;
  virtual bool resize(const char* path, uint64_t size) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool validateContent(const char*, const char*, const ContentManifest& manifest, std::span<uint8_t>) {
    return manifest.kind == ContentKind::Epub;
  }
  virtual bool installContentMetadata(const char*, const ContentManifest& manifest, std::span<uint8_t>) {
    return manifest.kind == ContentKind::Epub;
  }
  // The state is borrowed for this call from the durable parent transfer.
  virtual bool validateContent(const char* destination, const char* candidate, const ContentManifest& manifest,
                               const TransferState&, std::span<uint8_t> scratch) {
    return validateContent(destination, candidate, manifest, scratch);
  }
  virtual bool installContentMetadata(const char* destination, const ContentManifest& manifest, const TransferState&,
                                      std::span<uint8_t> scratch) {
    return installContentMetadata(destination, manifest, scratch);
  }
  virtual bool finalizeContentMetadata(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) {
    return true;
  }
  // Dictionary destinations identify member sets, not the uploaded ZIP. The
  // serialized installer owns publication and retains the original archive.
  virtual bool installDictionaryMembers(const char*, const ContentManifest&, const TransferState&, std::span<uint8_t>) {
    return false;
  }
  virtual bool verifyDictionaryArchive(const char* destination, const ContentManifest& manifest, const TransferState&,
                                       std::span<uint8_t> scratch) {
    uint64_t size = 0;
    const auto stage = stat(TRANSFER_STAGE, size);
    if (stage == FileStatus::Error) return false;
    return verify(stage == FileStatus::Present ? TRANSFER_STAGE : destination, manifest.length, manifest.contentHash,
                  scratch);
  }
  virtual bool verifyCourseSwitchSource(const ContentManifest& previous, const Identity&, std::span<uint8_t> scratch) {
    return verify("/tinta/course.pack", previous.length, previous.contentHash, scratch);
  }
  virtual bool verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t> workspace) = 0;
};

enum class TransferResult : uint8_t {
  Ok,
  NoTransaction,
  Invalid,
  Unauthorized,
  WrongStorage,
  Busy,
  IoError,
  Corrupt,
  Offset,
  HashMismatch
};

// Session-owned object. Use only while reader activities are closed and one
// serialized controller owns storage. Scratch must have at least 308 bytes
// and remain disjoint from radio queues and command payloads.
class Transfer {
 public:
  Transfer(TransferStorage& storage, std::span<uint8_t> workspace) : storage(storage), workspace(workspace) {}
  TransferResult recover(const Identity& storageGeneration,
                         TransferRecoveryMode mode = TransferRecoveryMode::CompleteInstallation);
  // InspectJournal validates journal context only. It performs no installation
  // or cleanup and leaves mutations disabled until normal recovery succeeds.
  TransferResult begin(const TransferState& state, std::string_view destination);
  TransferResult begin(const TransferDeclaration& declaration, std::string_view destination);
  const ContentManifest* contentManifest() const { return loaded && hasManifest ? &manifest : nullptr; }
  TransferResult append(const Identity& transaction, const Identity& owner, uint64_t offset,
                        std::span<const uint8_t> bytes);
  TransferResult commit(const Identity& transaction, const Identity& owner);
  TransferResult abort(const Identity& transaction, const Identity& owner);
  const TransferState* current() const { return loaded ? &state : nullptr; }
  // Borrowed until the next mutation/recovery; also visible to installation
  // callbacks during validated recovery. Unavailable after ambiguous I/O.
  std::string_view destination() const {
    return loaded && (recovered || inspected || recoveringInstallation) ? std::string_view(target) : std::string_view{};
  }

 private:
  TransferResult beginImpl(const TransferState& initial, std::string_view destination, const ContentManifest* content);
  TransferResult recoverImpl(const Identity& storageGeneration, TransferRecoveryMode mode);
  TransferResult authorize(const Identity& transaction, const Identity& owner) const;
  bool persist(const TransferState& next);
  TransferResult install();
  bool removeIfPresent(const char* path);
  bool finalizeMetadata() {
    return !hasManifest || storage.finalizeContentMetadata(target, manifest, state, workspace);
  }
  bool dictionary() const { return hasManifest && manifest.kind == ContentKind::Dictionary; }
  TransferStorage& storage;
  std::span<uint8_t> workspace;
  TransferState state{};
  ContentManifest manifest{};
  bool hasManifest = false;
  Identity generation{};
  char target[TRANSFER_TARGET_SIZE]{};
  uint64_t journalSequence = 0;
  int activeSlot = -1;
  bool hadOriginal = false;
  bool loaded = false;
  bool recovered = false;
  bool inspected = false;
  bool recoveringInstallation = false;
};

}  // namespace companion
