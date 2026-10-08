#pragma once

#include <Logging.h>

#include "CompanionTintaWriter.h"
#include "HalTintaJournalStorage.h"

namespace companion {
// Retain off stack with checked makeUniqueNoThrow for the learner activity lifetime.
class HalTintaWriterSession {
 public:
  HalTintaWriterSession() : journal(storage, scratch), writer(journal, storage) {}
  ~HalTintaWriterSession() { close(); }
  HalTintaWriterSession(const HalTintaWriterSession&) = delete;
  HalTintaWriterSession& operator=(const HalTintaWriterSession&) = delete;

  // The caller finishes migration and derived-state recovery before starting mutation hooks.
  TintaJournalResult start(IdentityStorage& identities) {
    if (failed) return failure(TintaJournalResult::Unavailable);
    if (started) return failure(TintaJournalResult::Conflict);
    const auto result = writer.start(identities);
    if (result != TintaJournalResult::Ok) {
      failed = true;
      return failure(close() ? result : TintaJournalResult::IoError);
    }
    started = true;
    return result;
  }
  bool close() {
    writer.stop();
    started = false;
    const bool closed = storage.close();
    failed |= !closed;
    return closed;
  }
  TintaWriter& mutations() { return writer; }
  // Borrowed proof views; caller excludes writes and retains this session.
  TintaJournal& authority() { return journal; }
  TintaJournalStorage& proofStorage() { return storage; }

 private:
  static TintaJournalResult failure(TintaJournalResult result) {
    LOG_ERR("COMPANION", "Tinta writer session start failed: %u", static_cast<unsigned>(result));
    return result;
  }
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> scratch{};
  HalTintaJournalStorage storage;
  TintaJournal journal;
  TintaWriter writer;
  bool started = false;
  bool failed = false;
};
}  // namespace companion
