#include "CompanionTransfer.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "CompanionDictionaryJournalPaths.h"
#include "CompanionFrame.h"

namespace companion {
namespace {
uint64_t readNumber(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t{bytes[i]} << (8 * i);
  return value;
}
void writeNumber(uint64_t value, std::span<uint8_t> bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t crc(std::span<const uint8_t> bytes) {
  uint32_t value = 0xFFFFFFFF;
  for (uint8_t byte : bytes) {
    value ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
  }
  return ~value;
}
bool validTarget(std::string_view path) {
  if (path.size() < 2 || path.size() >= TRANSFER_TARGET_SIZE || path.front() != '/' || path.back() == '/') return false;
  size_t start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i < path.size() && path[i] != '/') {
      if (static_cast<unsigned char>(path[i]) < 32 || path[i] == '\\' || path[i] == ':' || path[i] == 127) return false;
      continue;
    }
    const auto component = path.substr(start, i - start);
    if (component.empty() || component.front() == '.') return false;
    start = i + 1;
  }
  return true;
}
bool nonzero(const Identity& identity) {
  return std::any_of(identity.begin(), identity.end(), [](uint8_t byte) { return byte != 0; });
}
// Keep the manifest decode buffer out of the recovery scan stack frame.
[[gnu::noinline]] bool validJournalManifest(std::span<const uint8_t> bytes, const TransferState& state) {
  ContentManifest manifest;
  return bytes[240] == 1 && decodeRecord(bytes.subspan(241, CONTENT_MANIFEST_SIZE), manifest) &&
         matchesTransferManifest(manifest, state);
}
}  // namespace

bool Transfer::removeIfPresent(const char* path) {
  uint64_t size = 0;
  const auto status = storage.stat(path, size);
  return status == FileStatus::Missing || (status == FileStatus::Present && storage.remove(path));
}

bool Transfer::persist(const TransferState& next) {
  if (journalSequence == std::numeric_limits<uint64_t>::max()) return false;
  auto bytes = workspace.first(hasManifest ? TRANSFER_JOURNAL_SIZE : TRANSFER_LEGACY_JOURNAL_SIZE);
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'L';
  bytes[1] = 'C';
  bytes[2] = 'T';
  bytes[3] = hasManifest ? 2 : 1;
  writeNumber(journalSequence + 1, bytes.subspan(4, 8));
  if (encodeRecord(next, bytes.subspan(12, TRANSFER_STATE_SIZE)) != TRANSFER_STATE_SIZE) return false;
  bytes[111] = hadOriginal ? 1 : 0;
  std::memcpy(bytes.data() + 112, target, TRANSFER_TARGET_SIZE);
  if (hasManifest) {
    bytes[240] = 1;
    if (encodeRecord(manifest, bytes.subspan(241, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE) return false;
  }
  writeNumber(crc(bytes.first(bytes.size() - 4)), bytes.last(4));
  const int slot = activeSlot == 0 ? 1 : 0;
  if (!storage.write(TRANSFER_JOURNALS[slot], 0, bytes, true)) {
    recovered = false;
    return false;
  }
  state = next;
  ++journalSequence;
  activeSlot = slot;
  loaded = true;
  return true;
}

TransferResult Transfer::recover(const Identity& storageGeneration, TransferRecoveryMode mode) {
  const auto result = recoverImpl(storageGeneration, mode);
  recovered = result == TransferResult::Ok;
  return result;
}

TransferResult Transfer::recoverImpl(const Identity& storageGeneration, TransferRecoveryMode mode) {
  loaded = false;
  hasManifest = false;
  recovered = false;
  activeSlot = -1;
  journalSequence = 0;
  if (mode != TransferRecoveryMode::CompleteInstallation && mode != TransferRecoveryMode::DeferDictionaryInstallation)
    return TransferResult::Invalid;
  if (workspace.size() < TRANSFER_JOURNAL_SIZE || !nonzero(storageGeneration)) return TransferResult::Invalid;
  if (!storage.prepare()) return TransferResult::IoError;
  generation = storageGeneration;
  bool foundInvalid = false;
  for (int slot = 0; slot < 2; ++slot) {
    uint64_t size = 0;
    const auto status = storage.stat(TRANSFER_JOURNALS[slot], size);
    if (status == FileStatus::Error) return TransferResult::IoError;
    if (status == FileStatus::Missing) continue;
    if (size != TRANSFER_JOURNAL_SIZE && size != TRANSFER_LEGACY_JOURNAL_SIZE) {
      foundInvalid = true;
      continue;
    }
    auto bytes = workspace.first(static_cast<size_t>(size));
    if (!storage.read(TRANSFER_JOURNALS[slot], 0, bytes)) return TransferResult::IoError;
    TransferState parsed;
    const bool declared = size == TRANSFER_JOURNAL_SIZE;
    const auto end = std::find(bytes.begin() + 112, bytes.begin() + 240, 0);
    const size_t length = static_cast<size_t>(end - (bytes.begin() + 112));
    if (bytes[0] != 'L' || bytes[1] != 'C' || bytes[2] != 'T' || bytes[3] != (declared ? 2 : 1) || bytes[111] > 1 ||
        readNumber(bytes.last(4)) != crc(bytes.first(bytes.size() - 4)) ||
        !decodeRecord(bytes.subspan(12, TRANSFER_STATE_SIZE), parsed) ||
        !validTarget({reinterpret_cast<const char*>(bytes.data() + 112), length}) || !nonzero(parsed.transaction) ||
        !nonzero(parsed.owner) || (declared && !validJournalManifest(bytes, parsed))) {
      foundInvalid = true;
      continue;
    }
    const uint64_t sequence = readNumber(bytes.subspan(4, 8));
    if (sequence == 0) {
      foundInvalid = true;
      continue;
    }
    if (!loaded || sequence > journalSequence) {
      state = parsed;
      hasManifest = declared;
      if (declared) decodeRecord(bytes.subspan(241, CONTENT_MANIFEST_SIZE), manifest);
      std::memcpy(target, bytes.data() + 112, TRANSFER_TARGET_SIZE);
      hadOriginal = bytes[111] != 0;
      journalSequence = sequence;
      activeSlot = slot;
      loaded = true;
    } else if (sequence == journalSequence)
      return TransferResult::Corrupt;
  }
  if (!loaded) {
    uint64_t size = 0;
    const auto stage = storage.stat(TRANSFER_STAGE, size);
    const auto backup = storage.stat(TRANSFER_BACKUP, size);
    if (stage == FileStatus::Error || backup == FileStatus::Error) return TransferResult::IoError;
    if (foundInvalid || stage != FileStatus::Missing || backup != FileStatus::Missing) return TransferResult::Corrupt;
    return TransferResult::Ok;
  }
  if (state.storageGeneration != generation) return TransferResult::WrongStorage;
  struct RecoveryScope {
    bool& active;
    explicit RecoveryScope(bool& active) : active(active) { active = true; }
    ~RecoveryScope() { active = false; }
  } recoveryScope(recoveringInstallation);
  if (state.phase == TransferPhase::Installing) {
    if (mode == TransferRecoveryMode::DeferDictionaryInstallation && hasManifest &&
        manifest.kind == ContentKind::Dictionary) {
      uint64_t size = 0;
      if (storage.stat(TRANSFER_STAGE, size) == FileStatus::Error) return TransferResult::IoError;
      return storage.verifyDictionaryArchive(target, manifest, state, workspace) ? TransferResult::Ok
                                                                                 : TransferResult::HashMismatch;
    }
    return install();
  }
  if (state.phase == TransferPhase::Committed) {
    if (!(dictionary() ? storage.verifyDictionaryArchive(target, manifest, state, workspace)
                       : storage.verify(target, state.length, state.contentHash, workspace)))
      return TransferResult::HashMismatch;
    if (mode == TransferRecoveryMode::DeferDictionaryInstallation && hasManifest &&
        manifest.kind == ContentKind::Dictionary)
      return TransferResult::Ok;
    if (hasManifest && !storage.installContentMetadata(target, manifest, state, workspace))
      return TransferResult::IoError;
    return removeIfPresent(TRANSFER_BACKUP) && finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
  }
  uint64_t backupSize = 0;
  const auto backup = storage.stat(TRANSFER_BACKUP, backupSize);
  if (backup == FileStatus::Error) return TransferResult::IoError;
  if (backup != FileStatus::Missing) return TransferResult::Corrupt;
  if (state.phase == TransferPhase::Aborted)
    return removeIfPresent(TRANSFER_STAGE) && finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
  uint64_t size = 0;
  const auto stage = storage.stat(TRANSFER_STAGE, size);
  if (stage == FileStatus::Error) return TransferResult::IoError;
  if (stage == FileStatus::Missing && state.durableOffset == 0 && state.phase == TransferPhase::Receiving) {
    return storage.write(TRANSFER_STAGE, 0, {}, true) ? TransferResult::Ok : TransferResult::IoError;
  }
  if (stage != FileStatus::Present || size < state.durableOffset) return TransferResult::Corrupt;
  if (size != state.durableOffset && !storage.resize(TRANSFER_STAGE, state.durableOffset))
    return TransferResult::IoError;
  return TransferResult::Ok;
}

TransferResult Transfer::begin(const TransferState& initial, std::string_view destination) {
  return beginImpl(initial, destination, nullptr);
}

TransferResult Transfer::begin(const TransferDeclaration& declaration, std::string_view destination) {
  if (!validTransferDeclaration(declaration)) return TransferResult::Invalid;
  return beginImpl(declaration.state, destination, &declaration.manifest);
}

TransferResult Transfer::beginImpl(const TransferState& initial, std::string_view destination,
                                   const ContentManifest* content) {
  if (!recovered || !validTarget(destination) || initial.storageGeneration != generation ||
      initial.phase != TransferPhase::Receiving || initial.durableOffset != 0 || !nonzero(initial.transaction) ||
      !nonzero(initial.owner))
    return TransferResult::Invalid;
  if (loaded && state.transaction == initial.transaction) {
    return state.owner == initial.owner && state.contentHash == initial.contentHash && state.length == initial.length &&
                   destination == target && hasManifest == (content != nullptr) && (!content || manifest == *content)
               ? TransferResult::Ok
               : TransferResult::Invalid;
  }
  if (loaded && state.phase != TransferPhase::Committed && state.phase != TransferPhase::Aborted)
    return TransferResult::Busy;
  uint64_t size = 0;
  for (const auto paths : {DICTIONARY_EXTRACTION_JOURNALS, DICTIONARY_INSTALLATION_JOURNALS,
                           DICTIONARY_RETIREMENT_JOURNALS, DICTIONARY_ZIP_AUDIT_JOURNALS}) {
    for (unsigned slot = 0; slot < 2; ++slot) {
      const auto pending = storage.stat(paths[slot], size);
      if (pending == FileStatus::Error) return TransferResult::IoError;
      if (pending == FileStatus::Present) return TransferResult::Busy;
    }
  }
  const auto backup = storage.stat(TRANSFER_BACKUP, size);
  if (backup != FileStatus::Missing) return TransferResult::IoError;
  // Persist intent before creating the stage. Recovery accepts a missing empty stage.
  recovered = false;
  std::fill(std::begin(target), std::end(target), 0);
  std::memcpy(target, destination.data(), destination.size());
  const auto existing = storage.stat(target, size);
  if (existing == FileStatus::Error) return TransferResult::IoError;
  hadOriginal = existing == FileStatus::Present;
  hasManifest = content != nullptr;
  manifest = content ? *content : ContentManifest{};
  if (!persist(initial)) {
    recovered = false;
    return TransferResult::IoError;
  }
  if (!storage.write(TRANSFER_STAGE, 0, {}, true)) return TransferResult::IoError;
  recovered = true;
  return TransferResult::Ok;
}

TransferResult Transfer::authorize(const Identity& transaction, const Identity& owner) const {
  if (!recovered || !loaded) return TransferResult::NoTransaction;
  if (state.owner != owner) return TransferResult::Unauthorized;
  if (state.transaction != transaction) return TransferResult::Invalid;
  return TransferResult::Ok;
}

TransferResult Transfer::append(const Identity& transaction, const Identity& owner, uint64_t offset,
                                std::span<const uint8_t> bytes) {
  const auto authorization = authorize(transaction, owner);
  if (authorization != TransferResult::Ok) return authorization;
  if (state.phase != TransferPhase::Receiving || bytes.empty() || bytes.size() > SESSION_WORKSPACE_SIZE)
    return TransferResult::Invalid;
  const auto inputAddress = reinterpret_cast<uintptr_t>(bytes.data());
  const auto scratchAddress = reinterpret_cast<uintptr_t>(workspace.data());
  const bool overlaps = inputAddress <= scratchAddress ? scratchAddress - inputAddress < bytes.size()
                                                       : inputAddress - scratchAddress < workspace.size();
  if (overlaps) return TransferResult::Invalid;
  if (offset < state.durableOffset) {
    if (bytes.size() > state.durableOffset - offset) return TransferResult::Offset;
    // A lost acknowledgement may resend a durable range. Compare it without
    // rewriting either the staged file or its checkpoint.
    size_t compared = 0;
    while (compared < bytes.size()) {
      const size_t count = std::min(workspace.size(), bytes.size() - compared);
      auto part = workspace.first(count);
      if (!storage.read(TRANSFER_STAGE, offset + compared, part)) return TransferResult::IoError;
      if (!std::equal(part.begin(), part.end(), bytes.begin() + compared)) return TransferResult::Offset;
      compared += count;
    }
    return TransferResult::Ok;
  }
  if (offset != state.durableOffset || bytes.size() > state.length - state.durableOffset) return TransferResult::Offset;
  if (!storage.write(TRANSFER_STAGE, offset, bytes, false)) return TransferResult::IoError;
  TransferState next = state;
  next.durableOffset += bytes.size();
  if (!persist(next)) return TransferResult::IoError;
  return TransferResult::Ok;
}

TransferResult Transfer::install() {
  if (dictionary()) {
    if (!storage.verifyDictionaryArchive(target, manifest, state, workspace)) return TransferResult::HashMismatch;
    if (!storage.installDictionaryMembers(target, manifest, state, workspace) ||
        !storage.installContentMetadata(target, manifest, state, workspace))
      return TransferResult::IoError;
    TransferState next = state;
    next.phase = TransferPhase::Committed;
    if (!persist(next)) return TransferResult::IoError;
    return finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
  }
  uint64_t size = 0;
  const auto stage = storage.stat(TRANSFER_STAGE, size);
  const auto backup = storage.stat(TRANSFER_BACKUP, size);
  const auto destination = storage.stat(target, size);
  if (stage == FileStatus::Error || backup == FileStatus::Error || destination == FileStatus::Error)
    return TransferResult::IoError;
  if (stage == FileStatus::Present) {
    if (!storage.verify(TRANSFER_STAGE, state.length, state.contentHash, workspace))
      return TransferResult::HashMismatch;
    if (hadOriginal && backup == FileStatus::Missing) {
      if (destination != FileStatus::Present) return TransferResult::Corrupt;
      if (!storage.rename(target, TRANSFER_BACKUP)) return TransferResult::IoError;
    } else if (destination != FileStatus::Missing)
      return TransferResult::Corrupt;
    if (!storage.rename(TRANSFER_STAGE, target)) return TransferResult::IoError;
  } else if (destination != FileStatus::Present)
    return TransferResult::Corrupt;
  if (!storage.verify(target, state.length, state.contentHash, workspace)) return TransferResult::HashMismatch;
  if (hasManifest && !storage.installContentMetadata(target, manifest, state, workspace))
    return TransferResult::IoError;
  TransferState next = state;
  next.phase = TransferPhase::Committed;
  if (!persist(next)) return TransferResult::IoError;
  return removeIfPresent(TRANSFER_BACKUP) && finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
}

TransferResult Transfer::commit(const Identity& transaction, const Identity& owner) {
  const auto authorization = authorize(transaction, owner);
  if (authorization != TransferResult::Ok) return authorization;
  if (state.phase == TransferPhase::Committed) {
    if (hasManifest && !storage.installContentMetadata(target, manifest, state, workspace))
      return TransferResult::IoError;
    return removeIfPresent(TRANSFER_BACKUP) && finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
  }
  if (state.phase == TransferPhase::Aborted) return TransferResult::Invalid;
  if (state.phase == TransferPhase::Installing) return install();
  if (state.durableOffset != state.length) return TransferResult::Offset;
  if (!storage.verify(TRANSFER_STAGE, state.length, state.contentHash, workspace)) return TransferResult::HashMismatch;
  if (hasManifest && !storage.validateContent(target, TRANSFER_STAGE, manifest, state, workspace))
    return TransferResult::Invalid;
  TransferState next = state;
  next.phase = TransferPhase::Installing;
  if (!persist(next)) return TransferResult::IoError;
  return install();
}

TransferResult Transfer::abort(const Identity& transaction, const Identity& owner) {
  const auto authorization = authorize(transaction, owner);
  if (authorization != TransferResult::Ok) return authorization;
  if (state.phase == TransferPhase::Installing || state.phase == TransferPhase::Committed)
    return TransferResult::Invalid;
  TransferState next = state;
  next.phase = TransferPhase::Aborted;
  if (state.phase != TransferPhase::Aborted && !persist(next)) return TransferResult::IoError;
  return removeIfPresent(TRANSFER_STAGE) && finalizeMetadata() ? TransferResult::Ok : TransferResult::IoError;
}

}  // namespace companion
