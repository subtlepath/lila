#pragma once

#include <optional>

#include "CompanionIdentity.h"
#include "CompanionUnboundCourseReviewEpochUse.h"
#include "HalCompanionHeapAdmission.h"
#include "HalTintaJournalReadOnlyStorage.h"

namespace companion {
struct UnboundCourseReviewJournalReport {
  std::array<UnboundCourseReviewEpochUse, 5> journals{};
  uint8_t present = 0;
  bool operator==(const UnboundCourseReviewJournalReport&) const = default;
};
// Parent admits this owner off stack with the shared workspace and excludes all
// identity/journal writers. Verify checks exact frozen legacy event provenance.
// Its event/body loan forbids reusing the journal workspace during the callback.
// Covers the five live/retained namespaces, not archived baseline copies or replay.
// Counts are per namespace: copies of one event are not distinct learner events.
class HalUnboundCourseReviewJournalInspection final {
 public:
  using Permission = bool (*)(void*);
  using Verify = bool (*)(void*, const SyncEvent&, std::span<const uint8_t>);
  static constexpr std::array<TintaJournalLocation, 5> LOCATIONS{
      TintaJournalLocation::Active, TintaJournalLocation::MigrationCandidate, TintaJournalLocation::Backup,
      TintaJournalLocation::MergeCandidate, TintaJournalLocation::MergeBackup};
  HalUnboundCourseReviewJournalInspection(IdentityStorage& identities, const Identity& reader,
                                          const Identity& generation, std::span<uint8_t> scratch, Permission permitted,
                                          void* context)
      : identities(identities),
        reader(reader),
        generation(generation),
        scratch(scratch),
        permitted(permitted),
        context(context),
        lookup(allowed, this) {}
  ~HalUnboundCourseReviewJournalInspection() { releaseReaders(); }
  HalUnboundCourseReviewJournalInspection(const HalUnboundCourseReviewJournalInspection&) = delete;
  HalUnboundCourseReviewJournalInspection& operator=(const HalUnboundCourseReviewJournalInspection&) = delete;
  bool inspect(const UnboundCourseReviewReservation& reservation, Verify verify, void* verificationContext) {
    if (operating) return false;
    ready = false;
    if (!verify || !validUnboundCourseReviewReservation(reservation) || reservation.intent.reader != reader ||
        reservation.intent.request.original.generation != generation || scratch.size() < 8192 ||
        course_baseline_detail::overlaps(this, sizeof(*this), &reservation, sizeof(reservation)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &reservation, sizeof(reservation)))
      return failure("arguments");
    selected = reservation;
    verifier = verify;
    verifierContext = verificationContext;
    operating = true;
    cancelled = false;
    clearReport();
    bool valid = releaseReaders() && guard();
    for (size_t at = 0; valid && at < LOCATIONS.size(); ++at) valid = inspectLocation(at);
    const bool closed = releaseReaders();
    ready = valid && closed && guard() && !cancelled;
    operating = false;
    return ready || failure("inspection");
  }
  const UnboundCourseReviewJournalReport* report(const UnboundCourseReviewReservation& reservation) const {
    if (operating || !ready || reservation != selected) return nullptr;
    operating = true;
    if (!guard() || cancelled) ready = false;
    operating = false;
    return ready && reservation == selected ? &result : nullptr;
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
  IdentityStorage& identities;
  Identity reader, generation;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  HalCompanionFileLookup lookup;
  std::optional<HalTintaJournalReadOnlyStorage> storage;
  std::optional<TintaJournal> journal;
  UnboundCourseReviewReservation selected;
  UnboundCourseReviewJournalReport result;
  Verify verifier = nullptr;
  void* verifierContext = nullptr;
  mutable bool operating = false, ready = false;
  bool cancelled = false;
  [[gnu::noinline]] void clearReport() { result = {}; }
  [[gnu::noinline]] bool current() const {
    IdentityState state;
    return inspectIdentity(identities, state) == IdentityInspectionResult::Ok && state.device == reader &&
           state.storageGeneration == generation && selected.epoch <= state.eventEpoch;
  }
  bool guard() const {
    return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap() && current() &&
           !cancelled;
  }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseReviewJournalInspection*>(context)->guard(); }
  static bool verifyMatched(void* context, const SyncEvent& event, std::span<const uint8_t> body) {
    auto& owner = *static_cast<HalUnboundCourseReviewJournalInspection*>(context);
    return owner.guard() && owner.verifier && owner.verifier(owner.verifierContext, event, body) && owner.guard();
  }
  [[gnu::noinline]] bool inspectLocation(size_t at) {
    if (!guard()) return false;
    const auto presence = lookup.inspect(tintaJournalPaths(LOCATIONS[at])->directory);
    if (!guard() || presence == CompanionFilePresence::Error) return false;
    if (presence == CompanionFilePresence::Missing) return true;
    storage.emplace(LOCATIONS[at], allowed, this);
    journal.emplace(*storage, scratch);
    const bool valid =
        journal->open() == TintaJournalResult::Ok && guard() &&
        inspectUnboundCourseReviewEpochUse(*journal, selected, result.journals[at], allowed, this, verifyMatched) &&
        !result.journals[at].foreignGeneration && !result.journals[at].outsideReviewRange;
    const bool closed = releaseReaders();
    if (!valid || !closed || !guard()) return false;
    result.present |= static_cast<uint8_t>(1u << at);
    return true;
  }
  bool releaseReaders() {
    journal.reset();
    const bool closed = !storage || storage->close();
    storage.reset();
    return closed;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Unbound course review journal %s failed", operation);
    return false;
  }
};
}  // namespace companion
