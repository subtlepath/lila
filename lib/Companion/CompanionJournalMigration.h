#pragma once

#include "CompanionJournalCausalValidation.h"
#include "CompanionJournalTintaUndoValidation.h"

namespace companion {
// Caller excludes writers, owns separate storage, and publishes only after success.
// Keep this workspace outside the task stack; bodies and envelopes are reused.
class JournalMigration {
 public:
  TintaJournalResult copy(TintaJournal& source, TintaJournal& destination, JournalIdentityIndex& identities) {
    if (&source == &destination || !source.available() || !destination.available())
      return TintaJournalResult::Unavailable;
    const auto count = source.count();
    if (destination.recordSize() != TintaJournal::EXTENDED_RECORD_SIZE || destination.count() > count)
      return TintaJournalResult::Conflict;
    auto result = causal.validate(source, identities);
    if (result != TintaJournalResult::Ok) return result;
    result = undos.validate(source, identities);
    if (result != TintaJournalResult::Ok) return result;
    const auto prefix = destination.count();
    for (uint32_t at = 0; at < count; ++at) {
      if (source.count() != count || destination.count() != (at < prefix ? prefix : at))
        return TintaJournalResult::Conflict;
      result = source.read(at);
      if (result != TintaJournalResult::Ok) return result;
      event = source.event();
      const auto body = source.body();
      if (body.size() > bytes.size()) return TintaJournalResult::Corrupt;
      length = body.size();
      std::copy(body.begin(), body.end(), bytes.begin());
      if (at < prefix) {
        result = destination.read(at);
        if (result != TintaJournalResult::Ok) return result;
        if (!TintaJournal::sameEvent(event, destination.event()) || destination.body().size() != length ||
            !std::equal(destination.body().begin(), destination.body().end(), bytes.begin()))
          return TintaJournalResult::Conflict;
      } else {
        // Source closure and the exact copied prefix establish every dependency.
        result = destination.appendFresh(event, std::span(bytes).first(length), at);
        if (result != TintaJournalResult::Ok) return result;
      }
    }
    return source.count() == count && destination.count() == count ? TintaJournalResult::Ok
                                                                   : TintaJournalResult::Conflict;
  }

 private:
  JournalCausalValidation causal;
  JournalTintaUndoValidation undos;
  SyncEvent event;
  std::array<uint8_t, TintaJournal::MAX_BODY_SIZE> bytes{};
  size_t length = 0;
};
}  // namespace companion
