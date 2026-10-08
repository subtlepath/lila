#pragma once

#include "CompanionDictionaryExtractionJournal.h"

namespace companion {
// References must come from the recovered durable transfer under the serialized
// controller. Authorization is rechecked after every parent state change.
class DictionaryExtractionParent final {
 public:
  DictionaryExtractionParent(DictionaryExtractionJournal& journal, const TransferState& parent,
                             const ContentManifest& manifest, const Identity& generation)
      : journal(journal), parent(parent), manifest(manifest), generation(generation) {}
  DictionaryJournalResult begin(const DictionaryExtractionReceipt& initial) {
    if (!matches(initial) || parent.phase == TransferPhase::Committed) return reject();
    return journal.begin(initial);
  }
  DictionaryJournalResult recover(const DictionaryExtractionReceipt& expected) {
    if (!matches(expected)) return reject();
    const auto result = journal.recover(expected);
    if (result == DictionaryJournalResult::Ok && !current()) return reject();
    return result;
  }
  DictionaryJournalResult recover() {
    const auto result = journal.recover(parent.transaction, generation, parent.contentHash);
    if (result == DictionaryJournalResult::Ok && !current()) return reject();
    return result;
  }
  DictionaryJournalResult recordSealed(unsigned member, const Digest& hash) {
    if (!current() || parent.phase == TransferPhase::Committed) return reject();
    return journal.recordSealed(member, hash);
  }
  const DictionaryExtractionReceipt* current() const {
    const auto receipt = journal.current();
    if (!receipt || !matches(*receipt) ||
        (parent.phase == TransferPhase::Committed && receipt->sealed != (receipt->synonyms ? 15 : 7)))
      return nullptr;
    return receipt;
  }
  bool matchesArchive(const ContentManifest& archive) const { return current() && archive == manifest; }
  bool isPhase(TransferPhase phase) const { return current() && parent.phase == phase; }

 private:
  DictionaryExtractionJournal& journal;
  const TransferState& parent;
  const ContentManifest& manifest;
  const Identity& generation;
  bool matches(const DictionaryExtractionReceipt& receipt) const {
    return validDictionaryExtractionReceipt(receipt) && inventory_detail::nonzero(parent.owner) &&
           parent.transaction == receipt.transaction && parent.storageGeneration == generation &&
           receipt.generation == generation && parent.contentHash == receipt.archiveHash &&
           parent.durableOffset == parent.length && manifest.kind == ContentKind::Dictionary &&
           manifest.formatVersion == 1 && manifest.length >= 22 && manifest.length <= UINT32_MAX &&
           matchesTransferManifest(manifest, parent) &&
           (parent.phase == TransferPhase::Receiving || parent.phase == TransferPhase::Verified ||
            parent.phase == TransferPhase::Installing || parent.phase == TransferPhase::Committed);
  }
  DictionaryJournalResult reject() {
    journal.invalidate();
    return DictionaryJournalResult::Conflict;
  }
};
}  // namespace companion
