#pragma once

#include "CompanionDictionaryExtractionJournal.h"
#include "CompanionDictionaryInstallationPlan.h"

namespace companion {
// Session-owned. Plans and codec state exceed the task-local budget. Scratch is
// borrowed; parent authorization and filesystem verification precede updates.
class DictionaryInstallationJournal final {
 public:
  DictionaryInstallationJournal(DictionaryExtractionJournalStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const DictionaryInstallationPlan* current() const { return ready ? &active : nullptr; }
  void invalidate() { ready = false; }
  DictionaryJournalResult recover(const Transfer& transfer) {
    ready = false;
    slot = -1;
    const auto state = transfer.current();
    const auto manifest = transfer.contentManifest();
    if (!state || !manifest || manifest->kind != ContentKind::Dictionary || transfer.destination().empty() ||
        state->durableOffset != state->length || scratch.size() < DICTIONARY_INSTALLATION_PLAN_SIZE)
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    bool present = false;
    for (const auto path : DICTIONARY_INSTALLATION_JOURNALS) {
      uint64_t length = 0;
      const auto status = storage.stat(path, length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (length != DICTIONARY_INSTALLATION_PLAN_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_INSTALLATION_PLAN_SIZE);
      if (!storage.read(path, 0, bytes)) return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed) continue;
      if (parsed->extraction.transaction != state->transaction ||
          parsed->extraction.generation != state->storageGeneration || parsed->archives.original != *manifest ||
          transfer.destination() != std::string_view(parsed->base.data()))
        return DictionaryJournalResult::Conflict;
      candidate = *parsed;
      return recover(candidate);
    }
    return present ? DictionaryJournalResult::Corrupt : DictionaryJournalResult::Missing;
  }
  DictionaryJournalResult recover(const DictionaryInstallationPlan& expected) {
    ready = false;
    slot = -1;
    if (scratch.size() < DICTIONARY_INSTALLATION_PLAN_SIZE || !validDictionaryInstallationPlan(expected))
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    bool present = false;
    for (int at = 0; at < 2; ++at) {
      uint64_t length = 0;
      const auto path = DICTIONARY_INSTALLATION_JOURNALS[at];
      const auto status = storage.stat(path, length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (length != DICTIONARY_INSTALLATION_PLAN_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_INSTALLATION_PLAN_SIZE);
      if (!storage.read(path, 0, bytes)) return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed) continue;
      if (!samePlan(*parsed, expected)) return DictionaryJournalResult::Conflict;
      if (slot >= 0 && (parsed->revision == active.revision  ? *parsed != active
                        : parsed->revision > active.revision ? !successor(active, *parsed)
                                                             : !successor(*parsed, active)))
        return DictionaryJournalResult::Corrupt;
      if (slot < 0 || parsed->revision > active.revision) {
        active = *parsed;
        slot = at;
      }
    }
    if (slot < 0) return present ? DictionaryJournalResult::Corrupt : DictionaryJournalResult::Missing;
    ready = true;
    return DictionaryJournalResult::Ok;
  }
  DictionaryJournalResult begin(const DictionaryInstallationPlan& initial) {
    ready = false;
    if (initial.revision != 1 || initial.phase != DictionaryInstallationPhase::Prepared || initial.published)
      return DictionaryJournalResult::Invalid;
    const auto result = recover(initial);
    if (result != DictionaryJournalResult::Missing) {
      ready = false;
      return result == DictionaryJournalResult::Ok ? DictionaryJournalResult::Conflict : result;
    }
    candidate = initial;
    return persist();
  }
  DictionaryJournalResult startPublishing() { return phase(DictionaryInstallationPhase::Publishing); }
  DictionaryJournalResult markBound() { return phase(DictionaryInstallationPhase::Bound); }
  DictionaryJournalResult markCommitted() { return phase(DictionaryInstallationPhase::Committed); }
  DictionaryJournalResult recordPublished(unsigned member) {
    if (!ready || active.phase != DictionaryInstallationPhase::Publishing || member >= 4)
      return DictionaryJournalResult::Invalid;
    if (active.published & (1u << member)) return DictionaryJournalResult::Ok;
    candidate = active;
    candidate.published |= 1u << member;
    return advance();
  }

 private:
  DictionaryExtractionJournalStorage& storage;
  std::span<uint8_t> scratch;
  DictionaryInstallationPlan active, candidate;
  DictionaryInstallationPlanCodec codec;
  int slot = -1;
  bool ready = false;
  static bool samePlan(const DictionaryInstallationPlan& a, const DictionaryInstallationPlan& b) {
    return a.extraction == b.extraction && a.archives == b.archives && a.base == b.base;
  }
  static bool successor(const DictionaryInstallationPlan& previous, const DictionaryInstallationPlan& next) {
    if (previous.revision == UINT64_MAX || next.revision != previous.revision + 1) return false;
    if (previous.phase == next.phase) {
      const unsigned added = next.published ^ previous.published;
      return next.phase == DictionaryInstallationPhase::Publishing && added && !(added & (added - 1)) &&
             (previous.published & next.published) == previous.published;
    }
    return static_cast<unsigned>(next.phase) == static_cast<unsigned>(previous.phase) + 1 &&
           next.published == previous.published;
  }
  DictionaryJournalResult phase(DictionaryInstallationPhase next) {
    if (!ready) return DictionaryJournalResult::Invalid;
    if (active.phase == next) return DictionaryJournalResult::Ok;
    candidate = active;
    candidate.phase = next;
    return advance();
  }
  DictionaryJournalResult advance() {
    if (active.revision == UINT64_MAX) return DictionaryJournalResult::Exhausted;
    candidate.revision = active.revision + 1;
    if (!validDictionaryInstallationPlan(candidate) || !successor(active, candidate))
      return DictionaryJournalResult::Invalid;
    return persist();
  }
  DictionaryJournalResult persist() {
    ready = false;
    auto bytes = scratch.first(DICTIONARY_INSTALLATION_PLAN_SIZE);
    if (!codec.encode(candidate, bytes)) return DictionaryJournalResult::Invalid;
    const int next = slot == 0 ? 1 : 0;
    const auto path = DICTIONARY_INSTALLATION_JOURNALS[next];
    if (!storage.write(path, 0, bytes, true)) return DictionaryJournalResult::IoError;
    uint64_t length = 0;
    if (storage.stat(path, length) != FileStatus::Present || length != bytes.size() || !storage.read(path, 0, bytes))
      return DictionaryJournalResult::IoError;
    const auto readback = codec.inspect(bytes);
    if (!readback || *readback != candidate) return DictionaryJournalResult::Corrupt;
    active = candidate;
    slot = next;
    ready = true;
    return DictionaryJournalResult::Ok;
  }
};
}  // namespace companion
