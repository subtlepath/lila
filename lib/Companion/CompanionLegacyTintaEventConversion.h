#pragma once

#include "CompanionLegacyTintaEventCursor.h"
#include "CompanionLegacyTintaMutation.h"
#include "CompanionUnboundCourseReviewReservation.h"

namespace companion {
// Retain off stack; events/bodies are reused without allocating. Parent supplies
// exclusively owned reserved identities, frozen configuration and replayed prior
// item state. End-of-stream hashing and replay agreement remain publication gates.
// This isolated conversion asserts no shared ancestry or trustworthy UTC clock.
class LegacyTintaEventConversion final {
 public:
  using Hash = bool (*)(void*, std::span<const uint8_t>, Digest&);
  using Permission = bool (*)(void*);
  LegacyTintaEventConversion(Hash hash, Permission permitted, void* context)
      : hash(hash), permitted(permitted), context(context) {}
  bool begin(const UnboundCourseReviewReservation& input, const TintaSchedulerConfiguration& scheduler) {
    if (operating) return false;
    ready = packetReady = false;
    if (!hash || !permitted || !validUnboundCourseReviewReservation(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &scheduler, sizeof(scheduler)) ||
        !encodeTintaConfiguration(scheduler, configurationBytes))
      return false;
    selected = input;
    configuration = scheduler;
    operating = true;
    cancelled = false;
    ready = guard() && cursor.begin(selected.first(), selected.records) && guard();
    operating = false;
    return ready;
  }
  bool next(uint32_t index, const LegacyTintaEntry& entry, const tinta::core::ItemState& before) {
    if (operating) return false;
    packetReady = false;
    if (!ready || course_baseline_detail::overlaps(this, sizeof(*this), &entry, sizeof(entry)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &before, sizeof(before)))
      return false;
    sourceEntry = entry;
    priorState = before;
    operating = true;
    proposed = cursor;
    bool valid =
        guard() && proposed.assign(index, sourceEntry, identities) &&
        mapLegacyTintaMutation(sourceEntry, priorState, selected.intent.request.original.manifest.logicalIdentity,
                               configuration, identities.undoTarget, mutation) &&
        mutation.count == identities.count && proposed.events() <= selected.events;
    for (uint8_t at = 0; valid && at < identities.count; ++at) valid = encode(at);
    valid = valid && guard();
    if (valid) {
      cursor = proposed;
      packetReady = true;
    }
    operating = false;
    return valid;
  }
  const SyncEvent* event(uint8_t at) const {
    return !operating && ready && packetReady && at < identities.count ? &events[at] : nullptr;
  }
  std::span<const uint8_t> body(uint8_t at) const {
    return event(at) ? std::span(bytes[at]).first(lengths[at]) : std::span<const uint8_t>{};
  }
  bool matches(uint8_t at, const SyncEvent& candidate, std::span<const uint8_t> encoded) const {
    const auto* expected = event(at);
    const auto expectedBody = body(at);
    return expected && candidate == *expected && encoded.size() == expectedBody.size() &&
           std::equal(encoded.begin(), encoded.end(), expectedBody.begin());
  }
  bool complete() const { return !operating && ready && cursor.complete() && cursor.events() == selected.events; }
  uint32_t records() const { return !operating && ready ? cursor.records() : 0; }
  uint64_t count() const { return !operating && ready ? cursor.events() : 0; }
  void close() {
    if (operating) cancelled = true;
    ready = packetReady = false;
  }

 private:
  Hash hash;
  Permission permitted;
  void* context;
  UnboundCourseReviewReservation selected;
  TintaSchedulerConfiguration configuration;
  std::array<uint8_t, 6> configurationBytes{};
  LegacyTintaEventCursor cursor, proposed;
  LegacyTintaEventIdentities identities;
  TintaProgressMutation mutation;
  LegacyTintaEntry sourceEntry;
  tinta::core::ItemState priorState;
  std::array<SyncEvent, 2> events{};
  std::array<std::array<uint8_t, MAX_TINTA_BODY_SIZE>, 2> bytes{};
  std::array<size_t, 2> lengths{};
  bool operating = false, ready = false, packetReady = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled; }
  [[gnu::noinline]] bool encode(uint8_t at) {
    auto& event = events[at];
    event.identity = identities.events[at];
    event.storageGeneration = selected.intent.request.original.generation;
    event.resource = selected.intent.request.original.manifest.contentHash;
    event.kind = mutation.bodies[at].kind;
    event.studyDay = sourceEntry.studyDay;
    event.timestamp = 0;
    event.clockQuality = ClockQuality::Unknown;
    event.schedulerVersion = 0;
    event.schedulerConfiguration = {};
    event.ancestorCount = event.kind == EventKind::UndoReview ? 1 : 0;
    for (auto& ancestor : event.ancestors) ancestor = {};
    if (event.ancestorCount) event.ancestors[0] = identities.undoTarget;
    lengths[at] = encodeTintaBody(mutation.bodies[at], bytes[at]);
    if (!lengths[at] || !guard() || !hash(context, std::span(bytes[at]).first(lengths[at]), event.bodyHash) || !guard())
      return false;
    if (event.kind == EventKind::Review) {
      event.schedulerVersion = 1;
      if (!hash(context, configurationBytes, event.schedulerConfiguration) || !guard()) return false;
    }
    return validateTintaEnvelope(event, mutation.bodies[at], event.bodyHash, event.schedulerConfiguration);
  }
};
}  // namespace companion
