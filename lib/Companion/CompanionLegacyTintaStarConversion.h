#pragma once

#include "CompanionUnboundCourseStarReservation.h"

namespace companion {
// Off-stack conversion owner; no allocation. Native parent proves exclusive
// reserved identity ownership and computes the digest of the full emitted plan.
// Packets are provisional until that digest, frozen source and replay agree.
class LegacyTintaStarConversion final {
 public:
  using Hash = bool (*)(void*, std::span<const uint8_t>, Digest&);
  using Permission = bool (*)(void*);
  LegacyTintaStarConversion(Hash hash, Permission permitted, void* context)
      : hash(hash), permitted(permitted), context(context) {}
  bool begin(const UnboundCourseStarReservation& input) {
    if (operating) return false;
    ready = packetReady = false;
    if (!hash || !permitted || !validUnboundCourseStarReservation(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)))
      return false;
    selected = input;
    count = 0;
    operating = true;
    cancelled = false;
    ready = guard();
    operating = false;
    return ready;
  }
  bool next(uint32_t index, const TintaBody& input) {
    if (operating) return false;
    packetReady = false;
    if (!ready || index != count || count >= selected.events || input.kind != EventKind::Star ||
        input.course != selected.reviews.intent.request.original.manifest.logicalIdentity ||
        !tinta_body_detail::valid(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)))
      return false;
    bodyValue = input;
    operating = true;
    eventValue.identity = {selected.reviews.intent.reader, selected.epoch, uint64_t(count) + 1};
    eventValue.storageGeneration = selected.reviews.intent.request.original.generation;
    eventValue.resource = selected.reviews.intent.request.original.manifest.contentHash;
    eventValue.kind = EventKind::Star;
    eventValue.studyDay = 0;
    eventValue.timestamp = 0;
    eventValue.clockQuality = ClockQuality::Unknown;
    eventValue.schedulerVersion = 0;
    eventValue.schedulerConfiguration = {};
    for (auto& ancestor : eventValue.ancestors) ancestor = {};
    eventValue.ancestorCount = count || selected.reviews.events ? 1 : 0;
    if (count)
      eventValue.ancestors[0] = {selected.reviews.intent.reader, selected.epoch, count};
    else if (selected.reviews.events)
      eventValue.ancestors[0] = selected.reviewTail();
    const auto length = encodeTintaBody(bodyValue, bytes);
    const bool valid =
        length == bytes.size() && guard() && hash(context, bytes, eventValue.bodyHash) && guard() &&
        validateTintaEnvelope(eventValue, bodyValue, eventValue.bodyHash, eventValue.schedulerConfiguration);
    if (valid) {
      ++count;
      packetReady = true;
    }
    operating = false;
    return valid;
  }
  const SyncEvent* event() const { return !operating && ready && packetReady ? &eventValue : nullptr; }
  std::span<const uint8_t> body() const { return event() ? std::span(bytes) : std::span<const uint8_t>{}; }
  bool matches(const SyncEvent& candidate, std::span<const uint8_t> encoded) const {
    return event() && candidate == eventValue && encoded.size() == bytes.size() &&
           std::equal(encoded.begin(), encoded.end(), bytes.begin());
  }
  // Digest must be computed over every emitted canonical body in exact order.
  bool complete(const Digest& verifiedPlanHash) const {
    return !operating && ready && count == selected.events && verifiedPlanHash == selected.planHash;
  }
  void close() {
    if (operating) cancelled = true;
    ready = packetReady = false;
  }

 private:
  Hash hash;
  Permission permitted;
  void* context;
  UnboundCourseStarReservation selected;
  SyncEvent eventValue;
  TintaBody bodyValue;
  std::array<uint8_t, 23> bytes{};
  uint32_t count = 0;
  bool operating = false, ready = false, packetReady = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled; }
};
}  // namespace companion
