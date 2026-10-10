#pragma once

#if LILA_TINTA
#include "HalUnboundCourseReviewCountInspection.h"
#include "HalUnboundCourseReviewReservationStore.h"

namespace companion {
// Retain off stack. Native parent lends exclusive identity/store/count owners,
// excludes identity/learner/journal writers, and never uses this epoch elsewhere.
// Sealing reserves identities; replay/publication authorization is separate.
class HalUnboundCourseFreshReviewReservation final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseFreshReviewReservation(IdentityStorage& identities, const Identity& reader,
                                         const Identity& generation, const Identity& owner,
                                         HalUnboundCourseReviewReservationStore& store,
                                         HalUnboundCourseReviewCountInspection& counts, std::span<uint8_t> scratch,
                                         Permission permitted, void* context)
      : identities(identities),
        reader(reader),
        generation(generation),
        owner(owner),
        store(store),
        counts(counts),
        scratch(scratch),
        permitted(permitted),
        context(context),
        guarded(*this) {}
  ~HalUnboundCourseFreshReviewReservation() { closeReaders(); }
  HalUnboundCourseFreshReviewReservation(const HalUnboundCourseFreshReviewReservation&) = delete;
  HalUnboundCourseFreshReviewReservation& operator=(const HalUnboundCourseFreshReviewReservation&) = delete;
  UnboundCourseIntentResult reserve(const UnboundCourseMigrationIntent& input) {
    if (operating) return UnboundCourseIntentResult::Busy;
    ready = false;
    if (!validUnboundCourseMigrationIntent(input) || input.phase != UnboundCourseMigrationPhase::Prepared ||
        reader == Identity{} || generation == Identity{} || owner == Identity{} || input.reader != reader ||
        input.request.original.generation != generation || input.request.original.owner != owner ||
        scratch.size() < SESSION_WORKSPACE_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return UnboundCourseIntentResult::Invalid;
    if (hasEpoch && selected.intent != input) return UnboundCourseIntentResult::Conflict;
    selected.intent = input;
    operating = true;
    cancelled = false;
    if (!releaseReaders() || !guard()) return finish(UnboundCourseIntentResult::Busy);
    const auto loaded = store.load(observed);
    if (loaded != UnboundCourseIntentResult::Missing && loaded != UnboundCourseIntentResult::Pending &&
        loaded != UnboundCourseIntentResult::Ok)
      return finish(loaded);
    if (loaded == UnboundCourseIntentResult::Ok && observed.intent != selected.intent)
      return finish(UnboundCourseIntentResult::Conflict);
    if (!hasEpoch && loaded != UnboundCourseIntentResult::Missing) return finish(UnboundCourseIntentResult::Pending);
    if (hasEpoch && loaded == UnboundCourseIntentResult::Ok && observed != selected)
      return finish(UnboundCourseIntentResult::Conflict);
    if (!hasEpoch) {
      if (!readCounts()) return finish(UnboundCourseIntentResult::VerificationFailed);
      if (!guard() || !current(false) || !guard()) return finish(UnboundCourseIntentResult::Busy);
      if (provisionIdentity(guarded, allocated) != IdentityResult::Ok)
        return finish(UnboundCourseIntentResult::IoError);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (allocated.device != reader || allocated.storageGeneration != generation || !allocated.eventEpoch)
        return finish(UnboundCourseIntentResult::Conflict);
      selected.epoch = allocated.eventEpoch;
      hasEpoch = true;
    }
    if (!guard() || !current(true)) return finish(UnboundCourseIntentResult::Busy);
    return finish(store.persist(selected, owner, verify, this));
  }
  const UnboundCourseReviewReservation* reservation(const UnboundCourseMigrationIntent& input) const {
    if (operating || !ready || !hasEpoch || selected.intent != input) return nullptr;
    operating = true;
    if (!guard() || !current(true) || cancelled) ready = false;
    operating = false;
    return ready && selected.intent == input ? &selected : nullptr;
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    ready = hasEpoch = false;
    return releaseReaders();
  }

 private:
  class GuardedIdentity final : public IdentityStorage {
   public:
    explicit GuardedIdentity(HalUnboundCourseFreshReviewReservation& owner) : owner(owner) {}
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
    HalUnboundCourseFreshReviewReservation& owner;
  };
  IdentityStorage& identities;
  Identity reader, generation, owner;
  HalUnboundCourseReviewReservationStore& store;
  HalUnboundCourseReviewCountInspection& counts;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  mutable GuardedIdentity guarded;
  UnboundCourseReviewReservation selected, observed;
  IdentityState allocated;
  mutable bool operating = false, ready = false;
  bool hasEpoch = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  [[gnu::noinline]] bool current(bool requireOwned) const {
    IdentityState state;
    return inspectIdentity(guarded, state) == IdentityInspectionResult::Ok && state.device == reader &&
           state.storageGeneration == generation &&
           (!requireOwned || (hasEpoch && allocated.device == reader && allocated.storageGeneration == generation &&
                              allocated.eventEpoch == selected.epoch && state.eventEpoch >= selected.epoch));
  }
  [[gnu::noinline]] bool readCounts() {
    if (!guard() || !counts.inspect(selected.intent)) return false;
    const auto* report = counts.report(selected.intent);
    if (!report || !guard()) return false;
    selected.records = report->records;
    selected.events = report->events;
    return true;
  }
  static bool verify(void* raw, const UnboundCourseReviewReservation& value) {
    auto& self = *static_cast<HalUnboundCourseFreshReviewReservation*>(raw);
    if (!self.hasEpoch || value != self.selected || !self.guard() || !self.current(true) ||
        !self.counts.inspect(self.selected.intent))
      return false;
    const auto* report = self.counts.report(self.selected.intent);
    return report && report->records == self.selected.records && report->events == self.selected.events &&
           self.guard() && self.current(true) && !self.cancelled;
  }
  bool releaseReaders() {
    ready = false;
    const bool counted = counts.closeReaders();
    const bool stored = store.closeReaders();
    return counted && stored;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!releaseReaders()) result = UnboundCourseIntentResult::IoError;
    if (!guard() || (result == UnboundCourseIntentResult::Ok && !current(true)) || cancelled)
      result = UnboundCourseIntentResult::Busy;
    ready = result == UnboundCourseIntentResult::Ok;
    operating = false;
    if (!ready) LOG_ERR("COMPANION", "Fresh unbound review reservation refused: %u", static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
#endif
