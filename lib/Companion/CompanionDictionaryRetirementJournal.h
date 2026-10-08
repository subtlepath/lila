#pragma once

#include "CompanionDictionaryRetirementProof.h"

namespace companion {
// Session-owned active/codec plans exceed the task-local budget. Wire scratch
// is borrowed. Two verified copies must precede initial member-journal cleanup.
class DictionaryRetirementJournal final {
 public:
  DictionaryRetirementJournal(DictionaryExtractionJournalStorage& storage, std::span<uint8_t> scratch,
                              const Transfer& transfer)
      : storage(storage), scratch(scratch), transfer(transfer) {}
  const DictionaryInstallationPlan* current() const {
    return ready && dictionaryRetirementProofMatchesTransfer(active, transfer) ? &active : nullptr;
  }
  bool redundant() const { return current() && valid[0] && valid[1]; }
  void invalidate() { ready = false; }
  DictionaryJournalResult begin(const DictionaryInstallationParent& parent) {
    ready = false;
    if (!dictionaryRetirementProofFromParent(parent, active)) return DictionaryJournalResult::Conflict;
    return publish(active);
  }
  // Discover the committed plan after member journals have been retired.
  DictionaryJournalResult recover() {
    ready = false;
    present.fill(false);
    valid.fill(false);
    if (scratch.size() < DICTIONARY_RETIREMENT_PROOF_SIZE || !transfer.current() ||
        transfer.current()->phase != TransferPhase::Committed || !transfer.contentManifest() ||
        transfer.contentManifest()->kind != ContentKind::Dictionary || transfer.destination().empty())
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    bool found = false;
    for (unsigned at = 0; at < 2; ++at) {
      uint64_t length = 0;
      const auto status = storage.stat(DICTIONARY_RETIREMENT_JOURNALS[at], length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      found = true;
      if (length != DICTIONARY_RETIREMENT_PROOF_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_RETIREMENT_PROOF_SIZE);
      if (!storage.read(DICTIONARY_RETIREMENT_JOURNALS[at], 0, bytes)) return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed) continue;
      if (!dictionaryRetirementProofMatchesTransfer(*parsed, transfer)) return DictionaryJournalResult::Conflict;
      active = *parsed;
      return recover(active);
    }
    return found ? DictionaryJournalResult::Corrupt : DictionaryJournalResult::Missing;
  }
  DictionaryJournalResult recover(const DictionaryInstallationPlan& expected) {
    ready = false;
    present.fill(false);
    valid.fill(false);
    if (scratch.size() < DICTIONARY_RETIREMENT_PROOF_SIZE ||
        !dictionaryRetirementProofMatchesTransfer(expected, transfer))
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    for (unsigned at = 0; at < 2; ++at) {
      uint64_t length = 0;
      const auto status = storage.stat(DICTIONARY_RETIREMENT_JOURNALS[at], length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present[at] = true;
      if (length != DICTIONARY_RETIREMENT_PROOF_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_RETIREMENT_PROOF_SIZE);
      if (!storage.read(DICTIONARY_RETIREMENT_JOURNALS[at], 0, bytes)) return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed) continue;
      if (*parsed != expected || !dictionaryRetirementProofMatchesTransfer(*parsed, transfer))
        return DictionaryJournalResult::Conflict;
      active = *parsed;
      valid[at] = true;
    }
    ready = valid[0] || valid[1];
    return ready                      ? DictionaryJournalResult::Ok
           : present[0] || present[1] ? DictionaryJournalResult::Corrupt
                                      : DictionaryJournalResult::Missing;
  }
  DictionaryJournalResult publish(const DictionaryInstallationPlan& expected) {
    const auto recovered = recover(expected);
    if (recovered != DictionaryJournalResult::Ok && recovered != DictionaryJournalResult::Missing) return recovered;
    ready = false;
    for (unsigned at = 0; at < 2; ++at)
      if (present[at] && !valid[at]) return DictionaryJournalResult::Corrupt;
    return writeUnverified(expected);
  }
  // Verification must check installed content and hold exclusive ownership
  // through repair. A surviving valid proof is required before any overwrite.
  DictionaryJournalResult repair(const DictionaryInstallationPlan& expected,
                                 bool (*verify)(void*, const DictionaryInstallationPlan&), void* context) {
    const auto recovered = recover(expected);
    if (recovered != DictionaryJournalResult::Ok) return recovered;
    if (!verify || !verify(context, expected) || !current()) {
      ready = false;
      return DictionaryJournalResult::Conflict;
    }
    ready = false;
    return writeUnverified(expected);
  }

 private:
  DictionaryJournalResult writeUnverified(const DictionaryInstallationPlan& expected) {
    active = expected;
    for (unsigned at = 0; at < 2; ++at) {
      if (valid[at]) continue;
      auto bytes = scratch.first(DICTIONARY_RETIREMENT_PROOF_SIZE);
      if (!dictionaryRetirementProofMatchesTransfer(active, transfer) || !codec.encode(active, bytes))
        return DictionaryJournalResult::Conflict;
      const auto path = DICTIONARY_RETIREMENT_JOURNALS[at];
      if (!storage.write(path, 0, bytes, true)) return DictionaryJournalResult::IoError;
      uint64_t length = 0;
      if (storage.stat(path, length) != FileStatus::Present || length != bytes.size() || !storage.read(path, 0, bytes))
        return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed || *parsed != active) return DictionaryJournalResult::Corrupt;
      valid[at] = true;
    }
    ready = true;
    return current() ? DictionaryJournalResult::Ok : DictionaryJournalResult::Conflict;
  }

  DictionaryExtractionJournalStorage& storage;
  std::span<uint8_t> scratch;
  const Transfer& transfer;
  DictionaryInstallationPlan active;
  DictionaryRetirementProofCodec codec;
  std::array<bool, 2> present{}, valid{};
  bool ready = false;
};
}  // namespace companion
