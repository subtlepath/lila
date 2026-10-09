#pragma once

#include "CompanionJournalReplayOrder.h"
#include "CompanionPreferenceBody.h"

namespace companion {
// Retain off-stack. Caller freezes authority/index and owns disposable marks.
// Each key uses one backward pass; marked events are ancestors of later values.
class PortablePreferenceResolution final {
 public:
  TintaJournalResult run(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks) {
    clear();
    if (!journal.available()) return TintaJournalResult::Unavailable;
    const auto count = journal.count();
    auto result = validation.validate(journal, index);
    if (result != TintaJournalResult::Ok) return result;
    for (size_t slot = 0; slot < KEY_COUNT; ++slot) {
      const auto key = static_cast<uint8_t>(slot < 14 ? slot + 1 : slot + 18);
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
        const bool matching = event.kind == EventKind::Preference && bytes.size() >= 5 && bytes[2] == key;
        if (matching && !marked) {
          auto& selectedBody = encoded[slot];
          if (!selected) {
            PreferenceBodyView decoded;
            if (!decodePreferenceBody(bytes, decoded)) return TintaJournalResult::Corrupt;
            std::copy(bytes.begin(), bytes.end(), selectedBody.begin());
            lengths[slot] = bytes.size();
            selected = true;
          } else if (bytes.size() != lengths[slot] || !std::equal(bytes.begin(), bytes.end(), selectedBody.begin()))
            conflicts |= uint32_t{1} << slot;
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
      if (selected) views[size++] = std::span(encoded[slot]).first(lengths[slot]);
    }
    ready = true;
    return conflicts ? TintaJournalResult::Conflict : TintaJournalResult::Ok;
  }
  void clear() {
    ready = false;
    conflicts = 0;
    size = 0;
  }
  uint32_t conflictMask() const { return ready ? conflicts : 0; }
  // Empty on failure/conflict; callers must also check run's result.
  std::span<const std::span<const uint8_t>> bodies() const {
    return ready && !conflicts ? std::span(views).first(size) : std::span<const std::span<const uint8_t>>{};
  }

  bool missingReaderKeys(uint16_t& output) const {
    if (!ready || conflicts) return false;
    uint16_t missing = 0x3fff;
    for (const auto body : std::span(views).first(size)) {
      if (body[2] <= 14) missing &= ~(uint16_t{1} << (body[2] - 1));
    }
    output = missing;
    return true;
  }
  std::span<const uint8_t> readerBody(uint8_t key) const {
    if (!ready || key < 1 || key > 14 || (conflicts & (uint32_t{1} << (key - 1)))) return {};
    for (const auto body : std::span(views).first(size)) {
      if (body[2] == key) return body;
    }
    return {};
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
  static constexpr size_t KEY_COUNT = 23;
  std::array<std::array<uint8_t, 69>, KEY_COUNT> encoded{};
  std::array<size_t, KEY_COUNT> lengths{};
  std::array<std::span<const uint8_t>, KEY_COUNT> views{};
  uint32_t conflicts = 0;
  size_t size = 0;
  bool ready = false;
};
}  // namespace companion
