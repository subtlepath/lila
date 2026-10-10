#pragma once

#if LILA_TINTA
#include <mbedtls/sha256.h>

#include "CompanionLegacyTintaEventConversion.h"
#include "HalUnboundCourseReviewReader.h"

namespace companion {
// Retain off stack with exclusive source/workspace owners. Prior supplies the
// replayed item state before this record without reusing the stream workspace.
// Packets are provisional until End; snapshot agreement and epoch ownership are
// separate publication gates. This owner writes no journal or learner state.
class HalUnboundCourseReviewConversion final {
 public:
  using Permission = bool (*)(void*);
  using Prior = bool (*)(void*, const UnboundCourseReviewEntry&, tinta::core::ItemState&);
  HalUnboundCourseReviewConversion(HalUnboundCourseMigrationInspection& inspection,
                                   HalUnboundCourseReviewReader& stream, std::span<uint8_t> scratch,
                                   Permission permitted, void* context)
      : inspection(inspection),
        stream(stream),
        scratch(scratch),
        permitted(permitted),
        context(context),
        conversion(hash, allowed, this) {}
  ~HalUnboundCourseReviewConversion() { releaseReaders(); }
  HalUnboundCourseReviewConversion(const HalUnboundCourseReviewConversion&) = delete;
  HalUnboundCourseReviewConversion& operator=(const HalUnboundCourseReviewConversion&) = delete;
  bool open(const UnboundCourseReviewReservation& input, Prior prior, void* replayContext) {
    if (operating) return false;
    ready = complete = packetReady = false;
    if (!prior || !validUnboundCourseReviewReservation(input) || scratch.size() < SESSION_WORKSPACE_SIZE ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)))
      return failure();
    selected = input;
    this->prior = prior;
    this->replayContext = replayContext;
    operating = true;
    cancelled = false;
    records = events = 0;
    bool valid = releaseReaders() && guard();
    const auto* report = valid ? inspection.report(selected.intent) : nullptr;
    valid = report && report->learner.reviews.journal.records == selected.records;
    if (valid) {
      const auto& profile = report->learner.profile;
      valid = !profile.present || profile.status == tinta::core::Profile::LoadResult::Loaded ||
              profile.status == tinta::core::Profile::LoadResult::Upgraded;
      TintaSchedulerConfiguration configuration;
      configuration.retentionBasisPoints = static_cast<uint16_t>(profile.profile.retentionPermille * 10u);
      configuration.maximumInterval = profile.profile.maxInterval;
      valid = valid && conversion.begin(selected, configuration) && stream.open(selected.intent);
    }
    ready = valid && guard() && inspection.report(selected.intent) && !cancelled;
    if (!ready) releaseReaders();
    operating = false;
    return ready || failure();
  }
  LegacyTintaReadResult next(const UnboundCourseReviewReservation& input) {
    if (operating || !ready || input != selected) return LegacyTintaReadResult::Unavailable;
    operating = true;
    packetReady = false;
    if (!guard() || !inspection.report(selected.intent)) return finish(LegacyTintaReadResult::Unavailable, input);
    if (complete) return finish(LegacyTintaReadResult::End, input);
    const auto read = stream.next(selected.intent, entry);
    if (read == LegacyTintaReadResult::Record) {
      before = {};
      if (entry.index != records || !guard() || !prior(replayContext, entry, before) || !guard() ||
          !inspection.report(selected.intent) || !conversion.next(entry.index, entry.entry, before))
        return finish(LegacyTintaReadResult::IoError, input);
      records = conversion.records();
      events = static_cast<uint32_t>(conversion.count());
      packetReady = true;
      return finish(LegacyTintaReadResult::Record, input);
    }
    if (read != LegacyTintaReadResult::End) return finish(read, input);
    const bool valid = records == selected.records && events == selected.events && conversion.complete();
    const bool closed = releaseReaders();
    complete = valid && closed;
    return finish(complete ? LegacyTintaReadResult::End : LegacyTintaReadResult::IoError, input);
  }
  const SyncEvent* event(const UnboundCourseReviewReservation& input, uint8_t at) const {
    return loan(input) && packetReady ? conversion.event(at) : nullptr;
  }
  std::span<const uint8_t> body(const UnboundCourseReviewReservation& input, uint8_t at) const {
    return event(input, at) ? conversion.body(at) : std::span<const uint8_t>{};
  }
  bool completed(const UnboundCourseReviewReservation& input) const { return loan(input) && complete; }
  bool closeReaders() {
    ready = complete = packetReady = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseReviewReader& stream;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  LegacyTintaEventConversion conversion;
  UnboundCourseReviewReservation selected;
  UnboundCourseReviewEntry entry;
  tinta::core::ItemState before;
  Prior prior = nullptr;
  void* replayContext = nullptr;
  uint32_t records = 0, events = 0;
  mutable bool operating = false, ready = false, packetReady = false;
  bool complete = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseReviewConversion*>(context)->guard(); }
  static bool hash(void* context, std::span<const uint8_t> bytes, Digest& output) {
    auto& owner = *static_cast<HalUnboundCourseReviewConversion*>(context);
    Digest computed{};
    if (!owner.guard() || mbedtls_sha256(bytes.data(), bytes.size(), computed.data(), 0) != 0 || !owner.guard())
      return false;
    output = computed;
    return true;
  }
  bool releaseReaders() {
    conversion.close();
    return stream.closeReaders();
  }
  bool loan(const UnboundCourseReviewReservation& input) const {
    if (operating || !ready || input != selected) return false;
    operating = true;
    if (!guard() || !inspection.report(selected.intent) || cancelled) ready = packetReady = false;
    operating = false;
    return ready && input == selected;
  }
  LegacyTintaReadResult finish(LegacyTintaReadResult result, const UnboundCourseReviewReservation& input) {
    if (!guard() || !inspection.report(selected.intent) || cancelled || input != selected)
      result = LegacyTintaReadResult::Unavailable;
    if (result != LegacyTintaReadResult::Record && result != LegacyTintaReadResult::End) {
      ready = packetReady = complete = false;
      releaseReaders();
      failure();
    }
    operating = false;
    return result;
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course review conversion refused");
    return false;
  }
};
}  // namespace companion
#endif
