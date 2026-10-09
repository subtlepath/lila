#pragma once

#include "CompanionReaderPreferenceChangeCapture.h"
#include "CompanionReaderPreferencePhase.h"
#include "HalTintaWriterSession.h"

namespace companion {
// One checked off-stack save workspace; caller serializes settings/content/registry
// writers. Borrowed scratch remains valid until all proof and journal handles close.
class NativeReaderPreferenceSaveSession final {
 public:
  NativeReaderPreferenceSaveSession(const SdCardFontRegistry& registry, std::span<uint8_t> scratch)
      : content(registry, scratch), capture(writer.mutations()) {}
  TintaJournalResult persist(const ReaderPreferenceValues& baseline, const ReaderPreferenceValues& candidate,
                             IdentityStorage& identities, uint16_t contentChanges = 0) {
    if (!capture.initialize(baseline)) return failure("capture initialization");
    const auto prepared = capture.prepare(candidate, &encode, &content.contentMetadata(), contentChanges);
    if (prepared != TintaJournalResult::Ok || !capture.preparedChanges()) return prepared;
    const auto refreshKeys =
        static_cast<uint16_t>(contentChanges & ~ReaderPreferenceChangeCapture::changedKeys(baseline, candidate));
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(refreshKeys ? &contentFrontier : nullptr)) return failure("authority audit");
    if (refreshKeys) {
      const auto filtered = deduplicateContent(*audit, refreshKeys);
      if (filtered != TintaJournalResult::Ok || !capture.preparedChanges()) return filtered;
    }
    return commitPrepared(*audit, identities);
  }
  TintaJournalResult importMissing(const ReaderPreferenceValues& values, IdentityStorage& identities) {
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    Digest frontier{}, rechecked{};
    if (!audit || !audit->run(&frontier)) return failure("import authority audit");
    const auto count = audit->recordCount();
    const auto recordSize = audit->recordSize();
    uint16_t missing = 0;
    {
      // Resolution tables exceed the stack; release before dependency proof buffers.
      auto resolution = makeUniqueNoThrow<PortablePreferenceResolution>();
      if (!resolution) return failure("OOM: import resolution");
      const auto resolved = audit->resolvePortablePreferences(*resolution);
      if (resolved != TintaJournalResult::Ok) return resolved;
      if (!resolution->missingReaderKeys(missing)) return failure("import missing keys");
    }
    if (!missing) return TintaJournalResult::Ok;
    if (!capture.initialize(values)) return failure("import capture initialization");
    const auto prepared = capture.prepareSelected(values, &encode, &content.contentMetadata(), missing);
    if (prepared != TintaJournalResult::Ok) return prepared;
    if (!audit->run(&rechecked)) return failure("import authority recheck");
    if (rechecked != frontier || audit->recordCount() != count || audit->recordSize() != recordSize)
      return TintaJournalResult::Conflict;
    return commitPrepared(*audit, identities);
  }
  bool requiresRecovery() const { return recovery; }

 private:
  TintaJournalResult deduplicateContent(HalJournalCausalAuditSession& audit, uint16_t mask) {
    const auto count = audit.recordCount();
    const auto recordSize = audit.recordSize();
    {
      auto resolution = makeUniqueNoThrow<PortablePreferenceResolution>();
      if (!resolution) return failure("OOM: content refresh resolution");
      const auto resolved = audit.resolvePortablePreferences(*resolution);
      if (resolved != TintaJournalResult::Ok && resolved != TintaJournalResult::Conflict)
        return failure("content refresh authority resolution");
      for (uint8_t key = 1; key <= 14; ++key) {
        if (!(mask & (uint16_t{1} << (key - 1)))) continue;
        const auto proposed = capture.preparedBody(key);
        const auto current = resolution->readerBody(key);
        if (!current.empty() && current.size() == proposed.size() &&
            std::equal(current.begin(), current.end(), proposed.begin()) && !capture.omitPreparedKey(key))
          return failure("content refresh prepared key");
      }
    }
    if (!audit.run(&contentRechecked)) return failure("content refresh authority recheck");
    if (contentFrontier != contentRechecked || audit.recordCount() != count || audit.recordSize() != recordSize)
      return TintaJournalResult::Conflict;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult commitPrepared(HalJournalCausalAuditSession& audit, IdentityStorage& identities) {
    const auto started = writer.start(identities);
    if (started != TintaJournalResult::Ok) return failure("identity/writer start");
    auto* heads = audit.beginPreferenceKnowledge(writer.authority().count());
    if (!heads) return failure("knowledge snapshot");
    if (!capture.setInitialKnowledge(*heads)) return failure("capture knowledge binding");
    const auto result = capture.persistPrepared(0, 0, ClockQuality::Unknown);
    recovery = !writer.mutations().available();
    const bool headsClosed = audit.endPreferenceKnowledge();
    const bool writerClosed = writer.close();
    if (!headsClosed || !writerClosed) return failure("knowledge/writer close");
    return result;
  }
  static size_t encode(void* context, const ReaderPreferenceValues& values, uint8_t key, std::span<uint8_t> output) {
    return static_cast<NativeReaderPreferenceMetadata*>(context)->encodePreference(values, key, output);
  }
  TintaJournalResult failure(const char* stage) {
    recovery = true;
    LOG_ERR("COMPANION", "Reader preference save failed: %s", stage);
    return TintaJournalResult::IoError;
  }
  NativeReaderPreferenceContentPhase content;
  HalTintaWriterSession writer;
  ReaderPreferenceChangeCapture capture;
  Digest contentFrontier{}, contentRechecked{};
  bool recovery = false;
};
}  // namespace companion
