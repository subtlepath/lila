#pragma once

#include "CompanionJournalIdentityIndex.h"

namespace companion {
class JournalReplayVisits {
 public:
  virtual ~JournalReplayVisits() = default;
  virtual bool reset(uint32_t count) = 0;
  virtual bool visited(uint32_t record, bool& value) = 0;
  virtual bool mark(uint32_t record) = 0;
};

// Caller freezes the journal and index; visits are disposable, never authoritative.
class JournalReplayOrder {
 public:
  TintaJournalResult begin(TintaJournal& source, JournalIdentityIndex& identities, JournalReplayVisits& visits) {
    ready = false;
    const auto result = validation.validate(source, identities);
    if (result != TintaJournalResult::Ok) return result;
    count = source.count();
    if (!visits.reset(count)) return TintaJournalResult::IoError;
    journal = &source;
    index = &identities;
    marks = &visits;
    emitted = 0;
    ready = true;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult next(uint32_t& record, bool& complete) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (journal->count() != count) return fail(TintaJournalResult::Conflict);
    if (emitted == count) {
      complete = true;
      return TintaJournalResult::Ok;
    }
    bool found = false;
    uint32_t selected = 0;
    for (uint32_t at = 0; at < count; ++at) {
      bool done = false;
      if (!marks->visited(at, done)) return fail(TintaJournalResult::IoError);
      if (done) continue;
      const auto read = journal->read(at);
      if (read != TintaJournalResult::Ok) return fail(read);
      candidate = journal->event();
      bool eligible = true;
      for (unsigned parent = 0; parent < candidate.ancestorCount; ++parent) {
        const auto result = dependency(candidate.ancestors[parent], eligible);
        if (result != TintaJournalResult::Ok) return fail(result);
      }
      if (candidate.identity.sequence > 1) {
        auto previous = candidate.identity;
        --previous.sequence;
        const auto result = dependency(previous, eligible);
        if (result != TintaJournalResult::Ok) return fail(result);
      }
      if (!eligible) continue;
      const uint64_t time = candidate.clockQuality == ClockQuality::Trusted ? candidate.timestamp : 0;
      if (!found || candidate.studyDay < selectedDay ||
          (candidate.studyDay == selectedDay &&
           (time < selectedTime ||
            (time == selectedTime && journalIdentityBefore(candidate.identity, selectedIdentity))))) {
        selected = at;
        selectedDay = candidate.studyDay;
        selectedTime = time;
        selectedIdentity = candidate.identity;
        found = true;
      }
    }
    if (journal->count() != count) return fail(TintaJournalResult::Conflict);
    if (!found) return fail(TintaJournalResult::Corrupt);
    if (!marks->mark(selected)) return fail(TintaJournalResult::IoError);
    ++emitted;
    record = selected;
    complete = false;
    return TintaJournalResult::Ok;
  }

 private:
  TintaJournalResult dependency(const EventIdentity& identity, bool& eligible) {
    uint32_t record = 0;
    const auto lookup = index->find(identity, record);
    if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (lookup != JournalIdentityLookup::Found || record >= count) return TintaJournalResult::Corrupt;
    bool done = false;
    if (!marks->visited(record, done)) return TintaJournalResult::IoError;
    eligible = eligible && done;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult fail(TintaJournalResult result) {
    ready = false;
    return result;
  }
  JournalCausalValidation validation;
  SyncEvent candidate;
  EventIdentity selectedIdentity{};
  uint32_t selectedDay = 0, count = 0, emitted = 0;
  uint64_t selectedTime = 0;
  TintaJournal* journal = nullptr;
  JournalIdentityIndex* index = nullptr;
  JournalReplayVisits* marks = nullptr;
  bool ready = false;
};
}  // namespace companion
