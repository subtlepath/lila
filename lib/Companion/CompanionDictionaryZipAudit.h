#pragma once

#include "CompanionDictionaryExtractionJournal.h"
#include "CompanionDictionaryInstallationPlan.h"
#include "CompanionTransferDeclaration.h"

namespace companion {
inline constexpr size_t DICTIONARY_ZIP_AUDIT_SIZE = 8 + TRANSFER_DECLARATION_SIZE + 128 + 4;
struct DictionaryZipAuditProof {
  TransferDeclaration declaration;
  std::array<char, 128> base{};
  bool operator==(const DictionaryZipAuditProof&) const = default;
};
inline bool validDictionaryZipAuditProof(const DictionaryZipAuditProof& proof) {
  const auto& state = proof.declaration.state;
  const auto end = std::find(proof.base.begin(), proof.base.end(), 0);
  return end != proof.base.end() && std::all_of(end, proof.base.end(), [](char value) { return value == 0; }) &&
         validDictionaryInstallationBase(std::string_view(proof.base.data(), end - proof.base.begin())) &&
         validTransferDeclaration(proof.declaration) && validDictionaryBindingManifest(proof.declaration.manifest) &&
         inventory_detail::nonzero(state.owner) && inventory_detail::nonzero(state.transaction) &&
         inventory_detail::nonzero(state.storageGeneration);
}
// Retain outside the task stack; the decoded proof exceeds 256 bytes.
class DictionaryZipAuditCodec final {
 public:
  static size_t encode(const DictionaryZipAuditProof& proof, std::span<uint8_t> bytes) {
    if (bytes.size() < DICTIONARY_ZIP_AUDIT_SIZE || !validDictionaryZipAuditProof(proof)) return 0;
    bytes = bytes.first(DICTIONARY_ZIP_AUDIT_SIZE);
    std::fill(bytes.begin(), bytes.end(), 0);
    std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
    if (encodeTransferDeclaration(proof.declaration, bytes.subspan(8, TRANSFER_DECLARATION_SIZE)) !=
        TRANSFER_DECLARATION_SIZE)
      return 0;
    std::copy(proof.base.begin(), proof.base.end(), bytes.begin() + 8 + TRANSFER_DECLARATION_SIZE);
    inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(bytes.first(bytes.size() - 4)), 4);
    return bytes.size();
  }
  const DictionaryZipAuditProof* inspect(std::span<const uint8_t> bytes) {
    if (bytes.size() != DICTIONARY_ZIP_AUDIT_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
        bytes[5] || bytes[6] || bytes[7] ||
        inventory_detail::read(bytes, bytes.size() - 4, 4) != inventoryIndexCrc(bytes.first(bytes.size() - 4)) ||
        !decodeTransferDeclaration(bytes.subspan(8, TRANSFER_DECLARATION_SIZE), parsed.declaration))
      return nullptr;
    std::copy_n(bytes.begin() + 8 + TRANSFER_DECLARATION_SIZE, parsed.base.size(), parsed.base.begin());
    return validDictionaryZipAuditProof(parsed) ? &parsed : nullptr;
  }

 private:
  static constexpr std::array<uint8_t, 5> PREFIX = {'D', 'Z', 'A', 'P', 1};
  DictionaryZipAuditProof parsed;
};
// Persist both proofs before opening any ZIP index. One surviving valid proof
// permits repair; foreign valid records and wholly corrupt evidence are retained.
class DictionaryZipAuditJournal final {
 public:
  DictionaryZipAuditJournal(DictionaryExtractionJournalStorage& storage, const TransferState& state,
                            const ContentManifest& manifest, const Identity& generation, std::string_view base,
                            std::span<uint8_t> scratch)
      : storage(storage), state(state), manifest(manifest), generation(generation), base(base), scratch(scratch) {}
  const DictionaryZipAuditProof* current() const { return ready && authorized() ? &expected : nullptr; }
  DictionaryJournalResult recover() {
    ready = false;
    present.fill(false);
    valid.fill(false);
    if (!seed() || scratch.size() < DICTIONARY_ZIP_AUDIT_SIZE) return DictionaryJournalResult::Invalid;
    if (!storage.prepare() || !authorized()) return DictionaryJournalResult::IoError;
    for (unsigned at = 0; at < 2; ++at) {
      uint64_t length = 0;
      const auto status = storage.stat(DICTIONARY_ZIP_AUDIT_JOURNALS[at], length);
      if (status == FileStatus::Error || !authorized()) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present[at] = true;
      if (length != DICTIONARY_ZIP_AUDIT_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_ZIP_AUDIT_SIZE);
      if (!storage.read(DICTIONARY_ZIP_AUDIT_JOURNALS[at], 0, bytes) || !authorized())
        return DictionaryJournalResult::IoError;
      const auto proof = codec.inspect(bytes);
      if (!proof) continue;
      if (*proof != expected) return DictionaryJournalResult::Conflict;
      valid[at] = true;
    }
    ready = valid[0] || valid[1];
    return ready                      ? DictionaryJournalResult::Ok
           : present[0] || present[1] ? DictionaryJournalResult::Corrupt
                                      : DictionaryJournalResult::Missing;
  }
  DictionaryJournalResult publish() {
    const auto result = recover();
    if ((result != DictionaryJournalResult::Ok && result != DictionaryJournalResult::Missing) ||
        state.phase == TransferPhase::Aborted)
      return result == DictionaryJournalResult::Ok || result == DictionaryJournalResult::Missing
                 ? DictionaryJournalResult::Invalid
                 : result;
    ready = false;
    for (unsigned at = 0; at < 2; ++at) {
      if (valid[at]) continue;
      auto bytes = scratch.first(DICTIONARY_ZIP_AUDIT_SIZE);
      if (!authorized() || state.phase == TransferPhase::Aborted ||
          DictionaryZipAuditCodec::encode(expected, bytes) != bytes.size() ||
          !storage.write(DICTIONARY_ZIP_AUDIT_JOURNALS[at], 0, bytes, true) || !authorized())
        return DictionaryJournalResult::IoError;
      if (!storage.read(DICTIONARY_ZIP_AUDIT_JOURNALS[at], 0, bytes) || !authorized())
        return DictionaryJournalResult::IoError;
      const auto proof = codec.inspect(bytes);
      if (!proof || *proof != expected) return DictionaryJournalResult::Corrupt;
      valid[at] = true;
    }
    ready = authorized() && state.phase != TransferPhase::Aborted;
    return ready ? DictionaryJournalResult::Ok : DictionaryJournalResult::Invalid;
  }

 private:
  DictionaryExtractionJournalStorage& storage;
  const TransferState& state;
  const ContentManifest& manifest;
  const Identity& generation;
  std::string_view base;
  std::span<uint8_t> scratch;
  DictionaryZipAuditProof expected;
  DictionaryZipAuditCodec codec;
  std::array<bool, 2> present{}, valid{};
  bool ready = false, captured = false;
  bool seed() {
    if (captured) return authorized();
    if (!validDictionaryInstallationBase(base)) return false;
    expected.declaration = {manifest, state};
    expected.declaration.state.phase = TransferPhase::Receiving;
    expected.declaration.state.durableOffset = 0;
    expected.base.fill(0);
    std::copy(base.begin(), base.end(), expected.base.begin());
    captured = authorized();
    return captured;
  }
  bool authorized() const {
    const auto& parent = expected.declaration.state;
    return validDictionaryZipAuditProof(expected) && generation == parent.storageGeneration &&
           manifest == expected.declaration.manifest && state.transaction == parent.transaction &&
           state.owner == parent.owner && state.storageGeneration == parent.storageGeneration &&
           state.contentHash == parent.contentHash && state.length == parent.length &&
           state.durableOffset == state.length &&
           (state.phase == TransferPhase::Receiving || state.phase == TransferPhase::Verified ||
            state.phase == TransferPhase::Aborted) &&
           base == std::string_view(expected.base.data());
  }
};
}  // namespace companion
