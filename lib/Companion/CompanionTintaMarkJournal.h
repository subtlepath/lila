#pragma once

#include "CompanionTintaWriter.h"
#include "core/library/MarkLog.h"

namespace companion {
// The owner resolves legacy keys, restores derived state, and logs callback failures.
class TintaMarkJournal {
 public:
  struct Callbacks {
    void* context = nullptr;
    bool (*resolve)(void*, uint32_t, uint32_t&) = nullptr;
    bool (*clock)(void*, uint32_t&, uint64_t&, ClockQuality&) = nullptr;
    bool (*recover)(void*) = nullptr;
    void (*reportError)(void*, TintaJournalResult) = nullptr;
  };
  TintaMarkJournal(TintaWriter& writer, const Identity& course, const Digest& resource, EventKind kind,
                   Callbacks callbacks)
      : writer(writer), course(course), resource(resource), kind(kind), callbacks(callbacks) {}
  tinta::core::library::MarkLog::MutationJournal binding() { return {this, &persistCallback, &recoverCallback}; }

 private:
  bool error(TintaJournalResult result) {
    if (callbacks.reportError) callbacks.reportError(callbacks.context, result);
    return false;
  }
  static bool recoverCallback(void* context) {
    auto& owner = *static_cast<TintaMarkJournal*>(context);
    if (!owner.callbacks.reportError || !owner.callbacks.recover) return owner.error(TintaJournalResult::Invalid);
    if (!owner.callbacks.recover(owner.callbacks.context)) return owner.error(TintaJournalResult::IoError);
    return true;
  }
  static bool persistCallback(void* context, uint32_t key, bool enabled) {
    auto& owner = *static_cast<TintaMarkJournal*>(context);
    if (!owner.callbacks.reportError || !owner.callbacks.resolve || !owner.callbacks.clock ||
        (owner.kind != EventKind::Star && owner.kind != EventKind::ReadingComplete))
      return owner.error(TintaJournalResult::Invalid);
    if (!owner.writer.available()) return owner.error(TintaJournalResult::Unavailable);
    TintaBody body;
    body.course = owner.course;
    body.kind = owner.kind;
    body.enabled = enabled;
    if (!key || !owner.callbacks.resolve(owner.callbacks.context, key, body.uid) || !tinta_body_detail::valid(body))
      return owner.error(TintaJournalResult::Invalid);
    uint32_t day = 0;
    uint64_t timestamp = 0;
    ClockQuality quality = ClockQuality::Unknown;
    if (!owner.callbacks.clock(owner.callbacks.context, day, timestamp, quality))
      return owner.error(TintaJournalResult::Invalid);
    const auto result = owner.writer.record(body, owner.resource, day, timestamp, quality);
    if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) return owner.error(result);
    return true;
  }
  TintaWriter& writer;
  Identity course;
  Digest resource;
  EventKind kind;
  Callbacks callbacks;
};
}  // namespace companion
