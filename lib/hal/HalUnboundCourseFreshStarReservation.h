#pragma once

#if LILA_TINTA
#include "HalUnboundCourseStarPlanInspection.h"
#include "HalUnboundCourseStarReservationStore.h"

namespace companion {
// Retain off stack. Native parent lends exclusive identity/store/plan owners,
// excludes identity/learner/journal writers, and never uses this epoch elsewhere.
// Sealing reserves identities; replay/publication authorization is separate.
class HalUnboundCourseFreshStarReservation final {
 public:
  using Permission = bool (*)(void*);
  using VerifyReviews = bool (*)(void*, const UnboundCourseReviewReservation&);
  HalUnboundCourseFreshStarReservation(IdentityStorage& identities, const Identity& reader, const Identity& generation,
                                       const Identity& owner, HalUnboundCourseStarReservationStore& store,
                                       HalUnboundCourseStarPlanInspection& plans, std::span<uint8_t> scratch,
                                       Permission permitted, VerifyReviews verifyReviews, void* context)
      : identities(identities),
        reader(reader),
        generation(generation),
        owner(owner),
        store(store),
        plans(plans),
        scratch(scratch),
        permitted(permitted),
        verifyReviews(verifyReviews),
        context(context),
        guarded(*this) {}
  ~HalUnboundCourseFreshStarReservation() { closeReaders(); }
  HalUnboundCourseFreshStarReservation(const HalUnboundCourseFreshStarReservation&) = delete;
  HalUnboundCourseFreshStarReservation& operator=(const HalUnboundCourseFreshStarReservation&) = delete;
  UnboundCourseIntentResult reserve(const UnboundCourseReviewReservation& input) {
    if (operating) return UnboundCourseIntentResult::Busy;
    ready = false;
    if (!verifyReviews || !validUnboundCourseReviewReservation(input) || reader == Identity{} ||
        generation == Identity{} || owner == Identity{} || input.intent.reader != reader ||
        input.intent.request.original.generation != generation || input.intent.request.original.owner != owner ||
        scratch.size() < SESSION_WORKSPACE_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return UnboundCourseIntentResult::Invalid;
    if (hasEpoch && selected.reviews != input) return UnboundCourseIntentResult::Conflict;
    selected.reviews = input;
    operating = true;
    cancelled = false;
    if (!releaseReaders() || !guard()) return finish(UnboundCourseIntentResult::Busy);
    const auto loaded = store.load(observed);
    if (loaded != UnboundCourseIntentResult::Missing && loaded != UnboundCourseIntentResult::Pending &&
        loaded != UnboundCourseIntentResult::Ok)
      return finish(loaded);
    if (loaded == UnboundCourseIntentResult::Ok && observed.reviews != selected.reviews)
      return finish(UnboundCourseIntentResult::Conflict);
    if (!hasEpoch && loaded != UnboundCourseIntentResult::Missing) return finish(UnboundCourseIntentResult::Pending);
    if (hasEpoch && loaded == UnboundCourseIntentResult::Ok && observed != selected)
      return finish(UnboundCourseIntentResult::Conflict);
    if (!hasEpoch) {
      if (!readPlan()) return finish(UnboundCourseIntentResult::VerificationFailed);
      if (!guard() || !current(false) || !guard()) return finish(UnboundCourseIntentResult::Busy);
      if (provisionIdentity(guarded, allocated) != IdentityResult::Ok)
        return finish(UnboundCourseIntentResult::IoError);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (allocated.device != reader || allocated.storageGeneration != generation ||
          allocated.eventEpoch <= selected.reviews.epoch)
        return finish(UnboundCourseIntentResult::Conflict);
      selected.epoch = allocated.eventEpoch;
      hasEpoch = true;
    }
    if (!guard() || !current(true)) return finish(UnboundCourseIntentResult::Busy);
    return finish(store.persist(selected, owner, verify, this));
  }
  const UnboundCourseStarReservation* reservation(const UnboundCourseReviewReservation& input) const {
    if (operating || !ready || !hasEpoch || selected.reviews != input) return nullptr;
    operating = true;
    if (!guard() || !current(true) || cancelled) ready = false;
    operating = false;
    return ready && selected.reviews == input ? &selected : nullptr;
  }
  bool closeReaders() {
    ready = hasEpoch = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  class GuardedIdentity final : public IdentityStorage {
   public:
    explicit GuardedIdentity(HalUnboundCourseFreshStarReservation& owner) : owner(owner) {}
    bool hardwareIdentity(Identity& output) override {
      return owner.guard() && owner.identities.hardwareIdentity(output) && owner.guard();
    }
    bool cardIdentity(Identity& output) override {
      return owner.guard() && owner.identities.cardIdentity(output) && owner.guard();
    }
    IdentityRead readBinding(std::span<uint8_t> output) override {
      if (!owner.guard()) return IdentityRead::Error;
      const auto result = owner.identities.readBinding(output);
      return owner.guard() ? result : IdentityRead::Error;
    }
    bool writeBinding(std::span<const uint8_t> input) override {
      return owner.guard() && owner.identities.writeBinding(input) && owner.guard();
    }
    IdentityRead readMarker(Identity& output) override {
      if (!owner.guard()) return IdentityRead::Error;
      const auto result = owner.identities.readMarker(output);
      return owner.guard() ? result : IdentityRead::Error;
    }
    bool createMarker(const Identity& input) override {
      return owner.guard() && owner.identities.createMarker(input) && owner.guard();
    }
    bool randomIdentity(Identity& output) override {
      return owner.guard() && owner.identities.randomIdentity(output) && owner.guard();
    }

   private:
    HalUnboundCourseFreshStarReservation& owner;
  };
  IdentityStorage& identities;
  Identity reader, generation, owner;
  HalUnboundCourseStarReservationStore& store;
  HalUnboundCourseStarPlanInspection& plans;
  std::span<uint8_t> scratch;
  Permission permitted;
  VerifyReviews verifyReviews;
  void* context;
  mutable GuardedIdentity guarded;
  UnboundCourseStarReservation selected, observed;
  IdentityState allocated;
  mutable bool operating = false, ready = false;
  bool hasEpoch = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  bool reviewProof() const { return guard() && verifyReviews && verifyReviews(context, selected.reviews) && guard(); }
  [[gnu::noinline]] bool current(bool requireOwned) const {
    IdentityState state;
    return reviewProof() && inspectIdentity(guarded, state) == IdentityInspectionResult::Ok && state.device == reader &&
           state.storageGeneration == generation && state.eventEpoch >= selected.reviews.epoch &&
           (!requireOwned || (hasEpoch && allocated.device == reader && allocated.storageGeneration == generation &&
                              allocated.eventEpoch == selected.epoch && state.eventEpoch >= selected.epoch));
  }
  [[gnu::noinline]] bool readPlan() {
    if (!reviewProof() || !plans.inspect(selected.reviews)) return false;
    const auto* report = plans.report(selected.reviews);
    if (!report || !guard()) return false;
    selected.planHash = report->hash;
    selected.events = report->events;
    return true;
  }
  static bool verify(void* raw, const UnboundCourseStarReservation& value) {
    auto& self = *static_cast<HalUnboundCourseFreshStarReservation*>(raw);
    if (!self.hasEpoch || value != self.selected || !self.guard() || !self.current(true) ||
        !self.plans.inspect(self.selected.reviews))
      return false;
    const auto* report = self.plans.report(self.selected.reviews);
    return report && report->hash == self.selected.planHash && report->events == self.selected.events && self.guard() &&
           self.current(true) && !self.cancelled;
  }
  bool releaseReaders() {
    ready = false;
    const bool planned = plans.closeReaders();
    const bool stored = store.closeReaders();
    return planned && stored;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!releaseReaders()) result = UnboundCourseIntentResult::IoError;
    if (!guard() || (result == UnboundCourseIntentResult::Ok && !current(true)) || cancelled)
      result = UnboundCourseIntentResult::Busy;
    ready = result == UnboundCourseIntentResult::Ok;
    operating = false;
    if (!ready) LOG_ERR("COMPANION", "Fresh unbound star reservation refused: %u", static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
#endif
