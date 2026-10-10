#pragma once

#if LILA_TINTA
#include <mbedtls/sha256.h>

#include "CompanionLegacyTintaReplay.h"
#include "CompanionLegacyTintaStarPlan.h"
#include "CompanionUnboundCourseStarReservation.h"
#include "HalTintaReplayStore.h"
#include "HalUnboundCourseReviewConversion.h"
#include "HalUnboundCourseStarReader.h"

namespace companion {
struct UnboundCourseStarPlanReport {
  uint32_t events = 0;
  Digest hash{};
};
// Off-stack owner; parent excludes writers and lends exclusive complete replay,
// conversion, star-stream and workspace owners. No internal allocation. This
// proves a frozen star plan, not committed item agreement or publication rights.
class HalUnboundCourseStarPlanInspection final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseStarPlanInspection(HalUnboundCourseMigrationInspection& inspection, HalUnboundCourseStarReader& stars,
                                     HalUnboundCourseReviewConversion& conversion, LegacyTintaReplay& replay,
                                     HalTintaReplayStore& store, Permission permitted, void* context)
      : inspection(inspection),
        stars(stars),
        conversion(conversion),
        replay(replay),
        store(store),
        permitted(permitted),
        context(context),
        plan(nextItem, allowed, this) {
    mbedtls_sha256_init(&digest);
  }
  ~HalUnboundCourseStarPlanInspection() {
    releaseReaders();
    mbedtls_sha256_free(&digest);
  }
  HalUnboundCourseStarPlanInspection(const HalUnboundCourseStarPlanInspection&) = delete;
  HalUnboundCourseStarPlanInspection& operator=(const HalUnboundCourseStarPlanInspection&) = delete;
  bool inspect(const UnboundCourseReviewReservation& input) {
    if (operating) return false;
    ready = false;
    if (!validUnboundCourseReviewReservation(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)))
      return failure();
    selected = input;
    result = {};
    operating = true;
    cancelled = false;
    const bool valid = releaseReaders() && scoped() && stars.open(selected.intent) &&
                       plan.beginFromSource(selected.intent.request.original.manifest.logicalIdentity, nextMember) &&
                       hashPlan() && verifyMarks() && releaseReaders() && scoped() && input == selected && !cancelled;
    ready = valid;
    if (!valid) releaseReaders();
    operating = false;
    return ready || failure();
  }
  const UnboundCourseStarPlanReport* report(const UnboundCourseReviewReservation& input) const {
    if (operating || !ready || input != selected) return nullptr;
    operating = true;
    if (!scoped() || cancelled) ready = false;
    operating = false;
    return ready && input == selected ? &result : nullptr;
  }
  bool closeReaders() {
    ready = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseStarReader& stars;
  HalUnboundCourseReviewConversion& conversion;
  LegacyTintaReplay& replay;
  HalTintaReplayStore& store;
  Permission permitted;
  void* context;
  UnboundCourseReviewReservation selected;
  LegacyTintaStarPlan plan;
  mbedtls_sha256_context digest;
  TintaBody body;
  std::array<uint8_t, 23> bytes{};
  UnboundCourseStarPlanReport result;
  mutable bool operating = false, ready = false;
  bool cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  bool scoped() const {
    return guard() && inspection.report(selected.intent) && conversion.completed(selected) &&
           replay.at(selected, store, selected.records) && replay.complete(selected) &&
           store.matchesCourse(selected.intent.request.original.manifest.logicalIdentity) && guard();
  }
  static bool allowed(void* raw) { return static_cast<HalUnboundCourseStarPlanInspection*>(raw)->scoped(); }
  static bool nextItem(void* raw, bool previous, uint32_t uid, tinta::core::ItemState& item, bool& found) {
    auto& owner = *static_cast<HalUnboundCourseStarPlanInspection*>(raw);
    uint32_t key = 0;
    return owner.scoped() && owner.store.nextKey(HalTintaReplayStore::Kind::Item, previous, uid, key, found) &&
           (!found || owner.store.item(key, item)) && owner.scoped();
  }
  static LegacyTintaStarPlanResult nextMember(void* raw, uint32_t& uid) {
    auto& owner = *static_cast<HalUnboundCourseStarPlanInspection*>(raw);
    UnboundCourseStarEntry member;
    if (!owner.scoped()) return LegacyTintaStarPlanResult::Unavailable;
    const auto read = owner.stars.next(owner.selected.intent, member);
    if (!owner.scoped()) return LegacyTintaStarPlanResult::Unavailable;
    if (read == UnboundCourseStarReadResult::End) return LegacyTintaStarPlanResult::End;
    if (read != UnboundCourseStarReadResult::Record) return LegacyTintaStarPlanResult::IoError;
    uid = member.uid;
    return LegacyTintaStarPlanResult::Record;
  }
  [[gnu::noinline]] bool hashPlan() {
    if (!scoped() || mbedtls_sha256_starts(&digest, 0) != 0 || !scoped()) return false;
    for (;;) {
      if (!scoped()) return false;
      const auto read = plan.next(body);
      if (!scoped()) return false;
      if (read == LegacyTintaStarPlanResult::End) break;
      if (read != LegacyTintaStarPlanResult::Record || result.events == UINT32_MAX ||
          !unbound_course_detail::validStarReservationCounts(selected.records, result.events + 1) ||
          encodeTintaBody(body, bytes) != bytes.size() ||
          mbedtls_sha256_update(&digest, bytes.data(), bytes.size()) != 0 || !scoped())
        return false;
      if (++result.events % 32 == 0) vTaskDelay(1);
    }
    return plan.completed() && mbedtls_sha256_finish(&digest, result.hash.data()) == 0 && scoped();
  }
  [[gnu::noinline]] bool verifyMarks() {
    if (!scoped() || !stars.open(selected.intent)) return false;
    for (;;) {
      UnboundCourseStarEntry member;
      if (!scoped()) return false;
      const auto read = stars.next(selected.intent, member);
      if (!scoped()) return false;
      if (read == UnboundCourseStarReadResult::End) return true;
      if (read != UnboundCourseStarReadResult::Record) return false;
    }
  }
  bool releaseReaders() {
    plan.close();
    return stars.closeReaders();
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course star plan inspection refused");
    return false;
  }
};
}  // namespace companion
#endif
