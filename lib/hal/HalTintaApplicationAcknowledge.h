#pragma once

#include "HalTintaApplicationReceipts.h"

namespace companion {
// Checked off-stack owner. All borrowed stores outlive it; caller excludes journal
// and native mutation writers. Recovery must establish history before live binding.
class HalTintaApplicationAcknowledge final {
 public:
  HalTintaApplicationAcknowledge(TintaJournal& journal, TintaJournalStorage& storage,
                                 tinta::core::ProgressStore& progress, HalTintaApplicationReceipts& receipts,
                                 const Identity& course, const Identity& generation, const Digest& resource)
      : journal(journal),
        storage(storage),
        progress(progress),
        receipts(receipts),
        course(course),
        generation(generation),
        resource(resource) {}
  static bool callback(void* context, const EventIdentity& event, const tinta::core::JournalEntry& entry,
                       const tinta::core::ItemState& before, const tinta::core::ItemState& after,
                       uint32_t milliseconds) {
    return context && static_cast<HalTintaApplicationAcknowledge*>(context)->acknowledge(event, entry, before, after,
                                                                                         milliseconds);
  }
  // Recovery precedes live binding; a native undo requires one exact durable receipt.
  bool recoverUndo(EventIdentity& output) {
    if (progress.failed()) return failure("native progress unavailable");
    if (!progress.canUndo()) {
      output = {};
      return true;
    }
    if (!progress.loadUndoReview(receipt.entry, receipt.before, receipt.after) || !journal.available())
      return failure("native undo proof");
    const uint32_t count = journal.count();
    EventIdentity found{};
    bool matched = false;
    for (uint32_t at = 0; at < count; ++at) {
      if (journal.read(at) != TintaJournalResult::Ok) return failure("undo recovery journal");
      if (journal.event().kind != EventKind::Review || journal.event().storageGeneration != generation ||
          journal.event().resource != resource)
        continue;
      if (!capture(at, 0)) return failure("undo recovery review");
      if (bodies[0].course != course || bodies[0].uid != receipt.entry.uid) continue;
      const auto loaded = receipts.load(events[0].identity, course, generation, resource, target);
      if (loaded == TintaApplicationReceiptResult::Missing) continue;
      if (loaded != TintaApplicationReceiptResult::Ok ||
          !verifyTintaReviewApplicationReceipt(target, events[0], bodies[0], digests[0], configurations[0]))
        return failure("undo recovery receipt");
      uint8_t expected[tinta::core::JournalEntry::kSize], actual[tinta::core::JournalEntry::kSize];
      receipt.entry.encode(expected);
      target.entry.encode(actual);
      if (std::memcmp(expected, actual, sizeof(expected)) || !(target.before == receipt.before) ||
          !(target.after == receipt.after))
        continue;
      if (matched) return failure("ambiguous native undo");
      found = target.event;
      matched = true;
    }
    if (!matched || journal.count() != count ||
        !progress.verifyCommittedMutation(receipt.entry, receipt.before, receipt.after))
      return failure("undo recovery authority");
    output = found;
    return true;
  }
  bool acknowledge(const EventIdentity& event, const tinta::core::JournalEntry& entry,
                   const tinta::core::ItemState& before, const tinta::core::ItemState& after, uint32_t milliseconds) {
    receipt.event = event;
    receipt.course = course;
    receipt.generation = generation;
    receipt.resource = resource;
    receipt.entry = entry;
    receipt.before = before;
    receipt.after = after;
    receipt.responseMilliseconds = milliseconds;
    if (!validTintaApplicationReceipt(receipt) || !progress.verifyCommittedMutation(entry, before, after) ||
        !journal.available() || !journal.count())
      return failure("native mutation unavailable");
    const uint32_t count = journal.count();
    if (!capture(count - 1, 1) || events[1].identity != event) return failure("frontier mismatch");
    bool verified = false;
    if (entry.isReview()) {
      verified = verifyTintaReviewApplicationReceipt(receipt, events[1], bodies[1], digests[1], configurations[1]);
    } else if (entry.controlCode() == tinta::core::JournalEntry::kSetFlags) {
      const auto changed = before.flags ^ after.flags;
      const bool pair = (changed & tinta::core::item_flag::kSuspended) && (changed & tinta::core::item_flag::kStarred);
      if (pair && (count < 2 || !capture(count - 2, 0))) return failure("flag batch missing");
      const size_t first = pair ? 0 : 1;
      verified =
          verifyTintaFlagsApplicationReceipt(receipt, std::span(events).subspan(first),
                                             std::span(bodies).subspan(first), std::span(digests).subspan(first));
    } else if (entry.controlCode() == tinta::core::JournalEntry::kUndo) {
      if (bodies[1].kind != EventKind::UndoReview || receipts.load(bodies[1].undoTarget, course, generation, resource,
                                                                   target) != TintaApplicationReceiptResult::Ok)
        return failure("undo target receipt");
      bool found = false;
      for (uint32_t at = count - 1; at != 0; --at) {
        if (journal.read(at - 1) != TintaJournalResult::Ok) return failure("undo target lookup");
        if (journal.event().identity != target.event) continue;
        if (!capture(at - 1, 0)) return failure("undo target read");
        found = true;
        break;
      }
      verified = found && verifyTintaUndoApplicationReceipt(receipt, events[1], bodies[1], digests[1], target,
                                                            events[0], bodies[0], digests[0], configurations[0]);
    }
    if (!verified || journal.count() != count || !progress.verifyCommittedMutation(entry, before, after))
      return failure("application proof");
    return receipts.persist(receipt) == TintaApplicationReceiptResult::Ok || failure("receipt publication");
  }

 private:
  bool capture(uint32_t index, size_t slot) {
    if (journal.read(index) != TintaJournalResult::Ok || !decodeTintaBody(journal.body(), bodies[slot]) ||
        !storage.digest(journal.body(), digests[slot]))
      return false;
    events[slot] = journal.event();
    configurations[slot] = {};
    if (bodies[slot].kind == EventKind::Review) {
      std::array<uint8_t, 6> bytes{};
      if (encodeTintaConfiguration(bodies[slot].configuration, bytes) != bytes.size() ||
          !storage.digest(bytes, configurations[slot]))
        return false;
    }
    return true;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta application acknowledgement failed: %s", reason);
    return false;
  }
  TintaJournal& journal;
  TintaJournalStorage& storage;
  tinta::core::ProgressStore& progress;
  HalTintaApplicationReceipts& receipts;
  Identity course, generation;
  Digest resource;
  TintaApplicationReceipt receipt, target;
  std::array<SyncEvent, 2> events;
  std::array<TintaBody, 2> bodies;
  std::array<Digest, 2> digests{}, configurations{};
};
}  // namespace companion
