#pragma once

#include "CompanionTintaProgressMutation.h"
#include "CompanionTintaWriter.h"

namespace companion {
// Session-owned mapper state; recovery must precede binding to ProgressStore.
class TintaProgressJournal {
 public:
  using ConfigurationProvider = bool (*)(void*, TintaSchedulerConfiguration&, ClockQuality&);
  using TimestampProvider = bool (*)(void*, const tinta::core::JournalEntry&, ClockQuality, uint64_t&);
  TintaProgressJournal(TintaWriter& writer, const Identity& course, const Digest& resource,
                       const EventIdentity& recoveredUndo = {})
      : writer(writer), course(course), resource(resource), undoTarget(recoveredUndo) {}

  TintaJournalResult persist(const tinta::core::JournalEntry& entry, const tinta::core::ItemState& before,
                             const tinta::core::ItemState& after, uint32_t milliseconds,
                             const TintaSchedulerConfiguration& configuration, ClockQuality quality,
                             uint64_t timestamp = UINT64_MAX) {
    if (!writer.available()) return TintaJournalResult::Unavailable;
    if (quality == ClockQuality::Trusted && timestamp == UINT64_MAX) return TintaJournalResult::Invalid;
    if (timestamp == UINT64_MAX) timestamp = entry.time;
    if (!mapTintaProgressMutation(entry, before, after, milliseconds, course, configuration, undoTarget, mutation))
      return TintaJournalResult::Invalid;
    auto result = TintaJournalResult::Ok;
    if (mutation.count == 2)
      result = writer.recordFlags(mutation.bodies, resource, entry.day, timestamp, quality);
    else if (mutation.count == 1)
      result = writer.record(mutation.bodies[0], resource, entry.day, timestamp, quality);
    if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) return result;
    applicationPending = mutation.count != 0;
    if (applicationPending) application = writer.committedIdentity();
    undoTarget = entry.isReview() ? writer.committedIdentity() : EventIdentity{};
    return result;
  }
  // The owner reports persistence errors and outlives every callback invocation.
  tinta::core::ProgressStore::MutationJournal binding(const TintaSchedulerConfiguration& configuration,
                                                      ClockQuality quality, void* errorContext,
                                                      void (*reportError)(void*, TintaJournalResult)) {
    this->configuration = configuration;
    this->quality = quality;
    this->errorContext = errorContext;
    this->reportError = reportError;
    configurationContext = nullptr;
    configurationProvider = nullptr;
    timestampProvider = nullptr;
    dynamicConfiguration = false;
    return {this, &persistCallback};
  }
  tinta::core::ProgressStore::MutationJournal binding(void* configurationContext,
                                                      ConfigurationProvider configurationProvider, void* errorContext,
                                                      void (*reportError)(void*, TintaJournalResult),
                                                      TimestampProvider timestampProvider = nullptr) {
    this->configurationContext = configurationContext;
    this->configurationProvider = configurationProvider;
    this->errorContext = errorContext;
    this->reportError = reportError;
    this->timestampProvider = timestampProvider;
    dynamicConfiguration = true;
    return {this, &persistCallback};
  }
  const EventIdentity& undoIdentity() const { return undoTarget; }
  bool pendingApplication() const { return applicationPending; }
  const EventIdentity& applicationIdentity() const { return application; }
  void didApply() { applicationPending = false; }

 private:
  static bool persistCallback(void* context, const tinta::core::JournalEntry& entry,
                              const tinta::core::ItemState& before, const tinta::core::ItemState& after,
                              uint32_t milliseconds) {
    auto& owner = *static_cast<TintaProgressJournal*>(context);
    if (!owner.reportError) return false;
    if (owner.dynamicConfiguration &&
        (!owner.configurationProvider ||
         !owner.configurationProvider(owner.configurationContext, owner.configuration, owner.quality))) {
      owner.reportError(owner.errorContext, TintaJournalResult::Invalid);
      return false;
    }
    uint64_t timestamp = UINT64_MAX;
    if (owner.timestampProvider &&
        !owner.timestampProvider(owner.configurationContext, entry, owner.quality, timestamp)) {
      owner.reportError(owner.errorContext, TintaJournalResult::Invalid);
      return false;
    }
    const auto result =
        owner.persist(entry, before, after, milliseconds, owner.configuration, owner.quality, timestamp);
    if (result == TintaJournalResult::Ok || result == TintaJournalResult::Duplicate) return true;
    owner.reportError(owner.errorContext, result);
    return false;
  }
  TintaSchedulerConfiguration configuration{};
  ClockQuality quality = ClockQuality::Unknown;
  void* errorContext = nullptr;
  void (*reportError)(void*, TintaJournalResult) = nullptr;
  void* configurationContext = nullptr;
  ConfigurationProvider configurationProvider = nullptr;
  TimestampProvider timestampProvider = nullptr;
  bool dynamicConfiguration = false;
  TintaWriter& writer;
  Identity course;
  Digest resource;
  EventIdentity undoTarget;
  EventIdentity application;
  bool applicationPending = false;
  TintaProgressMutation mutation;
};
}  // namespace companion
