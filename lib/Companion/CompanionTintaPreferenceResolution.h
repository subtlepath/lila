#pragma once

#include "CompanionJournalReplayOrder.h"
#include "CompanionPreferenceBody.h"

namespace companion {
// Retain off-stack. Caller freezes authority/index and owns disposable marks.
// Each key uses one backward pass; marked events are ancestors of later values.
class TintaPreferenceResolution final {
 public:
  TintaJournalResult run(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks) {
    clear();
    if (!journal.available()) return TintaJournalResult::Unavailable;
    const auto count = journal.count();
    auto result = validation.validate(journal, index);
    if (result != TintaJournalResult::Ok) return result;
    for (uint8_t key = 32; key <= 40; ++key) {
      if (!marks.reset(count)) return TintaJournalResult::IoError;
      bool selected = false;
      for (uint32_t remaining = count; remaining; --remaining) {
        const auto record = remaining - 1;
        bool marked = false;
        if (!marks.visited(record, marked)) return TintaJournalResult::IoError;
        result = journal.read(record);
        if (result != TintaJournalResult::Ok) return result;
        event = journal.event();
        const auto bytes = journal.body();
        const bool matching = event.kind == EventKind::Preference && bytes.size() == 8 && bytes[2] == key;
        if (matching && !marked) {
          auto& selectedBody = encoded[key - 32];
          if (!selected) {
            std::copy_n(bytes.begin(), selectedBody.size(), selectedBody.begin());
            selected = true;
          } else if (!std::equal(bytes.begin(), bytes.end(), selectedBody.begin()))
            conflicts |= static_cast<uint16_t>(1U << (key - 32));
        }
        if (matching || marked) {
          for (unsigned parent = 0; parent < event.ancestorCount; ++parent) {
            result = markParent(index, marks, event.ancestors[parent], record);
            if (result != TintaJournalResult::Ok) return result;
          }
          if (event.identity.sequence > 1) {
            predecessor = event.identity;
            --predecessor.sequence;
            result = markParent(index, marks, predecessor, record);
            if (result != TintaJournalResult::Ok) return result;
          }
        }
        if (journal.count() != count) return TintaJournalResult::Conflict;
      }
      if (selected) views[size++] = encoded[key - 32];
    }
    ready = true;
    return conflicts ? TintaJournalResult::Conflict : TintaJournalResult::Ok;
  }
  void clear() {
    ready = false;
    conflicts = 0;
    size = 0;
  }
  uint16_t conflictMask() const { return ready ? conflicts : 0; }
  // Empty on failure/conflict; callers must also check run's result.
  std::span<const std::span<const uint8_t>> bodies() const {
    return ready && !conflicts ? std::span(views).first(size) : std::span<const std::span<const uint8_t>>{};
  }

 private:
  static TintaJournalResult markParent(JournalIdentityIndex& index, JournalReplayVisits& marks,
                                       const EventIdentity& identity, uint32_t child) {
    uint32_t record = 0;
    const auto lookup = index.find(identity, record);
    if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (lookup != JournalIdentityLookup::Found || record >= child) return TintaJournalResult::Corrupt;
    return marks.mark(record) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }
  JournalCausalValidation validation;
  SyncEvent event;
  EventIdentity predecessor;
  std::array<std::array<uint8_t, 8>, 9> encoded{};
  std::array<std::span<const uint8_t>, 9> views{};
  uint16_t conflicts = 0;
  size_t size = 0;
  bool ready = false;
};
}  // namespace companion
