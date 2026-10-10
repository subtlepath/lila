#pragma once

#if LILA_TINTA
#include "CompanionLegacyTintaStarConversion.h"
#include "HalUnboundCourseStarPlanInspection.h"

namespace companion {
// Parent lends exclusive sealed-reservation/plan/star/replay owners and excludes
// writers. Packets are provisional until End. No journal or learner writes,
// internal allocation or inferred shared legacy ancestry; retain off stack.
class HalUnboundCourseStarConversion final {
 public:
  using Permission = bool (*)(void*);
  using VerifyReservation = bool (*)(void*, const UnboundCourseStarReservation&);
  HalUnboundCourseStarConversion(HalUnboundCourseStarPlanInspection& plans, HalUnboundCourseStarReader& stars,
                                 HalUnboundCourseReviewConversion& reviews, LegacyTintaReplay& replay,
                                 HalTintaReplayStore& store, Permission permitted, VerifyReservation verifyReservation,
                                 void* context)
      : plans(plans),
        stars(stars),
        reviews(reviews),
        replay(replay),
        store(store),
        permitted(permitted),
        verifyReservation(verifyReservation),
        context(context),
        plan(nextItem, allowed, this),
        conversion(hash, allowed, this) {
    mbedtls_sha256_init(&digest);
  }
  ~HalUnboundCourseStarConversion() {
    releaseReaders();
    mbedtls_sha256_free(&digest);
  }
  HalUnboundCourseStarConversion(const HalUnboundCourseStarConversion&) = delete;
  HalUnboundCourseStarConversion& operator=(const HalUnboundCourseStarConversion&) = delete;
  bool open(const UnboundCourseStarReservation& input) {
    if (operating) return false;
    ready = complete = packetReady = false;
    if (!verifyReservation || !validUnboundCourseStarReservation(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)))
      return failure();
    selected = input;
    operating = true;
    cancelled = false;
    count = 0;
    const bool valid =
        releaseReaders() && owned() && plans.inspect(selected.reviews) && scoped() &&
        stars.open(selected.reviews.intent) &&
        plan.beginFromSource(selected.reviews.intent.request.original.manifest.logicalIdentity, nextMember) &&
        conversion.begin(selected) && mbedtls_sha256_starts(&digest, 0) == 0 && scoped() && input == selected;
    ready = valid;
    if (!valid) releaseReaders();
    operating = false;
    return ready || failure();
  }
  LegacyTintaStarPlanResult next(const UnboundCourseStarReservation& input) {
    if (operating || !ready || input != selected) return LegacyTintaStarPlanResult::Unavailable;
    operating = true;
    packetReady = false;
    if (!scoped()) return finish(LegacyTintaStarPlanResult::Unavailable, input);
    if (complete) return finish(LegacyTintaStarPlanResult::End, input);
    const auto read = plan.next(bodyValue);
    if (!scoped()) return finish(LegacyTintaStarPlanResult::Unavailable, input);
    if (read == LegacyTintaStarPlanResult::Record) {
      if (count >= selected.events || !conversion.next(count, bodyValue))
        return finish(LegacyTintaStarPlanResult::IoError, input);
      const auto bytes = conversion.body();
      if (bytes.size() != 23 || mbedtls_sha256_update(&digest, bytes.data(), bytes.size()) != 0 || !scoped())
        return finish(LegacyTintaStarPlanResult::IoError, input);
      if (++count % 32 == 0) vTaskDelay(1);
      packetReady = true;
      return finish(LegacyTintaStarPlanResult::Record, input);
    }
    if (read != LegacyTintaStarPlanResult::End) return finish(read, input);
    const bool valid = count == selected.events && plan.completed() &&
                       mbedtls_sha256_finish(&digest, actualHash.data()) == 0 && conversion.complete(actualHash) &&
                       scoped() && verifyMarks() && releaseReaders();
    complete = valid;
    return finish(valid ? LegacyTintaStarPlanResult::End : LegacyTintaStarPlanResult::IoError, input);
  }
  const SyncEvent* event(const UnboundCourseStarReservation& input) const {
    return loan(input) && packetReady ? conversion.event() : nullptr;
  }
  std::span<const uint8_t> body(const UnboundCourseStarReservation& input) const {
    return event(input) ? conversion.body() : std::span<const uint8_t>{};
  }
  bool matches(const UnboundCourseStarReservation& input, const SyncEvent& candidate,
               std::span<const uint8_t> bytes) const {
    return loan(input) && packetReady && conversion.matches(candidate, bytes);
  }
  bool completed(const UnboundCourseStarReservation& input) const { return loan(input) && complete; }
  bool closeReaders() {
    ready = complete = packetReady = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  HalUnboundCourseStarPlanInspection& plans;
  HalUnboundCourseStarReader& stars;
  HalUnboundCourseReviewConversion& reviews;
  LegacyTintaReplay& replay;
  HalTintaReplayStore& store;
  Permission permitted;
  VerifyReservation verifyReservation;
  void* context;
  UnboundCourseStarReservation selected;
  LegacyTintaStarPlan plan;
  LegacyTintaStarConversion conversion;
  mbedtls_sha256_context digest;
  TintaBody bodyValue;
  Digest actualHash{};
  uint32_t count = 0;
  mutable bool operating = false, ready = false, packetReady = false;
  bool complete = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  bool owned() const { return guard() && verifyReservation && verifyReservation(context, selected) && guard(); }
  bool scoped() const {
    if (!owned() || !reviews.completed(selected.reviews) ||
        !replay.at(selected.reviews, store, selected.reviews.records) || !replay.complete(selected.reviews) ||
        !store.matchesCourse(selected.reviews.intent.request.original.manifest.logicalIdentity))
      return false;
    const auto* report = plans.report(selected.reviews);
    return report && report->events == selected.events && report->hash == selected.planHash && owned();
  }
  static bool allowed(void* raw) { return static_cast<HalUnboundCourseStarConversion*>(raw)->scoped(); }
  static bool nextItem(void* raw, bool previous, uint32_t uid, tinta::core::ItemState& item, bool& found) {
    auto& owner = *static_cast<HalUnboundCourseStarConversion*>(raw);
    uint32_t key = 0;
    return owner.scoped() && owner.store.nextKey(HalTintaReplayStore::Kind::Item, previous, uid, key, found) &&
           (!found || owner.store.item(key, item)) && owner.scoped();
  }
  static LegacyTintaStarPlanResult nextMember(void* raw, uint32_t& uid) {
    auto& owner = *static_cast<HalUnboundCourseStarConversion*>(raw);
    UnboundCourseStarEntry member;
    if (!owner.scoped()) return LegacyTintaStarPlanResult::Unavailable;
    const auto result = owner.stars.next(owner.selected.reviews.intent, member);
    if (!owner.scoped()) return LegacyTintaStarPlanResult::Unavailable;
    if (result == UnboundCourseStarReadResult::End) return LegacyTintaStarPlanResult::End;
    if (result != UnboundCourseStarReadResult::Record) return LegacyTintaStarPlanResult::IoError;
    uid = member.uid;
    return LegacyTintaStarPlanResult::Record;
  }
  static bool hash(void* raw, std::span<const uint8_t> bytes, Digest& output) {
    auto& owner = *static_cast<HalUnboundCourseStarConversion*>(raw);
    Digest value{};
    if (!owner.scoped() || mbedtls_sha256(bytes.data(), bytes.size(), value.data(), 0) != 0 || !owner.scoped())
      return false;
    output = value;
    return true;
  }
  [[gnu::noinline]] bool verifyMarks() {
    if (!scoped() || !stars.open(selected.reviews.intent)) return false;
    for (;;) {
      UnboundCourseStarEntry member;
      if (!scoped()) return false;
      const auto read = stars.next(selected.reviews.intent, member);
      if (!scoped()) return false;
      if (read == UnboundCourseStarReadResult::End) return true;
      if (read != UnboundCourseStarReadResult::Record) return false;
    }
  }
  bool releaseReaders() {
    plan.close();
    conversion.close();
    return stars.closeReaders();
  }
  bool loan(const UnboundCourseStarReservation& input) const {
    if (operating || !ready || input != selected) return false;
    operating = true;
    if (!scoped() || cancelled) ready = packetReady = false;
    operating = false;
    return ready && input == selected;
  }
  LegacyTintaStarPlanResult finish(LegacyTintaStarPlanResult result, const UnboundCourseStarReservation& input) {
    if (!scoped() || cancelled || input != selected) result = LegacyTintaStarPlanResult::Unavailable;
    if (result != LegacyTintaStarPlanResult::Record && result != LegacyTintaStarPlanResult::End) {
      ready = packetReady = complete = false;
      releaseReaders();
      failure();
    }
    operating = false;
    return result;
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course star conversion refused");
    return false;
  }
};
}  // namespace companion
#endif
