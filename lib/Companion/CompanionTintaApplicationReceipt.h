#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaBody.h"
#include "core/srs/ProgressStore.h"
#include "core/srs/Review.h"

namespace companion {
inline constexpr size_t TINTA_APPLICATION_RECEIPT_SIZE = 152;
struct TintaApplicationReceipt {
  EventIdentity event;
  Identity course{}, generation{};
  Digest resource{};
  tinta::core::JournalEntry entry;
  tinta::core::ItemState before, after;
  uint32_t responseMilliseconds = 0;
};
inline bool validTintaApplicationReceipt(const TintaApplicationReceipt& value) {
  if (!tinta_body_detail::validIdentity(value.event) || !tinta_body_detail::nonzero(value.course) ||
      !tinta_body_detail::nonzero(value.generation) || !tinta_body_detail::nonzero(value.resource) ||
      !value.entry.uid || value.entry.uid == UINT32_MAX || value.before.uid != value.entry.uid ||
      value.after.uid != value.entry.uid)
    return false;
  std::array<uint8_t, 16> encoded{};
  tinta::core::ItemState checked;
  value.before.encode(encoded.data());
  if (!tinta::core::ItemState::decode(encoded.data(), checked) || !(checked == value.before)) return false;
  value.after.encode(encoded.data());
  if (!tinta::core::ItemState::decode(encoded.data(), checked) || !(checked == value.after)) return false;
  if (value.entry.isReview()) {
    const auto expected =
        tinta::core::JournalEntry::review(value.entry.uid, value.entry.grade(), value.entry.format(),
                                          value.responseMilliseconds, value.entry.day, value.entry.time);
    return value.entry.format() <= 9 && value.entry.arg == expected.arg && !value.after.isNew() &&
           value.after.lastDay == value.entry.day;
  }
  if (value.responseMilliseconds != 0) return false;
  if (value.entry.controlCode() == tinta::core::JournalEntry::kUndo) return value.entry.arg == 0;
  if (value.entry.controlCode() != tinta::core::JournalEntry::kSetFlags || value.entry.arg != value.after.flags)
    return false;
  const auto changed = value.before.flags ^ value.after.flags;
  return changed != 0 && !(changed & ~(tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred));
}
// Digests are computed from the received authority body/configuration by the caller.
// This proves a review transition, not the current native file contents or ancestry.
inline bool verifyTintaReviewApplicationReceipt(const TintaApplicationReceipt& receipt, const SyncEvent& event,
                                                const TintaBody& body, const Digest& bodyDigest,
                                                const Digest& configurationDigest) {
  if (!validTintaApplicationReceipt(receipt) || !receipt.entry.isReview() || body.kind != EventKind::Review ||
      !validateTintaEnvelope(event, body, bodyDigest, configurationDigest) || receipt.event != event.identity ||
      receipt.course != body.course || receipt.generation != event.storageGeneration ||
      receipt.resource != event.resource || receipt.entry.uid != body.uid || receipt.entry.day != event.studyDay ||
      static_cast<uint8_t>(receipt.entry.grade()) != body.grade || receipt.entry.format() != body.format ||
      receipt.responseMilliseconds != body.responseMilliseconds)
    return false;
  tinta::core::Fsrs scheduler(static_cast<float>(body.configuration.retentionBasisPoints) / 10000.0f,
                              body.configuration.maximumInterval);
  auto expected = receipt.before;
  tinta::core::applyReview(scheduler, expected, receipt.entry.grade(), receipt.entry.day);
  return expected == receipt.after;
}
// The authority records and their independently computed digests are caller-owned.
inline bool verifyTintaFlagsApplicationReceipt(const TintaApplicationReceipt& receipt,
                                               std::span<const SyncEvent> events, std::span<const TintaBody> bodies,
                                               std::span<const Digest> bodyDigests) {
  if (!validTintaApplicationReceipt(receipt) || receipt.entry.isReview() ||
      receipt.entry.controlCode() != tinta::core::JournalEntry::kSetFlags || events.empty() || events.size() > 2 ||
      bodies.size() != events.size() || bodyDigests.size() != events.size() || receipt.event != events.back().identity)
    return false;
  auto expected = receipt.before;
  expected.flags = receipt.after.flags;
  if (!(expected == receipt.after)) return false;
  const auto changed = receipt.before.flags ^ receipt.after.flags;
  const size_t count =
      ((changed & tinta::core::item_flag::kSuspended) != 0) + ((changed & tinta::core::item_flag::kStarred) != 0);
  if (events.size() != count) return false;
  size_t at = 0;
  const Digest noConfiguration{};
  for (const auto flag : {tinta::core::item_flag::kSuspended, tinta::core::item_flag::kStarred}) {
    if (!(changed & flag)) continue;
    const auto& event = events[at];
    const auto& body = bodies[at];
    const auto kind = flag == tinta::core::item_flag::kSuspended ? EventKind::Suspension : EventKind::Star;
    if (!validateTintaEnvelope(event, body, bodyDigests[at], noConfiguration) || body.kind != kind ||
        body.course != receipt.course || body.uid != receipt.entry.uid ||
        body.enabled != ((receipt.after.flags & flag) != 0) || event.storageGeneration != receipt.generation ||
        event.resource != receipt.resource || event.studyDay != receipt.entry.day)
      return false;
    ++at;
  }
  if (events.size() == 2) {
    const auto& first = events[0];
    const auto& last = events[1];
    if (first.identity.origin != last.identity.origin || first.identity.epoch != last.identity.epoch ||
        first.identity.sequence == UINT64_MAX || last.identity.sequence != first.identity.sequence + 1 ||
        last.ancestorCount != 1 || last.ancestors[0] != first.identity || last.timestamp != first.timestamp ||
        last.clockQuality != first.clockQuality)
      return false;
  }
  return true;
}
// The caller also proves target availability and intervening history in the
// frozen journal. Matching transitions alone do not establish undo eligibility.
inline bool verifyTintaUndoApplicationReceipt(const TintaApplicationReceipt& receipt, const SyncEvent& event,
                                              const TintaBody& body, const Digest& bodyDigest,
                                              const TintaApplicationReceipt& target, const SyncEvent& targetEvent,
                                              const TintaBody& targetBody, const Digest& targetBodyDigest,
                                              const Digest& targetConfigurationDigest) {
  const Digest noConfiguration{};
  return validTintaApplicationReceipt(receipt) && !receipt.entry.isReview() &&
         receipt.entry.controlCode() == tinta::core::JournalEntry::kUndo && body.kind == EventKind::UndoReview &&
         validateTintaEnvelope(event, body, bodyDigest, noConfiguration) &&
         verifyTintaReviewApplicationReceipt(target, targetEvent, targetBody, targetBodyDigest,
                                             targetConfigurationDigest) &&
         receipt.event == event.identity && receipt.course == body.course &&
         receipt.generation == event.storageGeneration && receipt.resource == event.resource &&
         receipt.entry.uid == body.uid && receipt.entry.day == event.studyDay && body.undoTarget == target.event &&
         receipt.course == target.course && receipt.generation == target.generation &&
         receipt.resource == target.resource && receipt.entry.uid == target.entry.uid &&
         receipt.before == target.after && receipt.after == target.before;
}
inline bool tintaApplicationReceiptOverlap(std::span<const uint8_t> bytes, const TintaApplicationReceipt& value) {
  const auto first = reinterpret_cast<uintptr_t>(bytes.data()), second = reinterpret_cast<uintptr_t>(&value);
  return first >= second ? first - second < sizeof(value) : second - first < bytes.size();
}
inline size_t encodeTintaApplicationReceipt(const TintaApplicationReceipt& value, std::span<uint8_t> output) {
  if (output.size() < TINTA_APPLICATION_RECEIPT_SIZE ||
      tintaApplicationReceiptOverlap(output.first(TINTA_APPLICATION_RECEIPT_SIZE), value) ||
      !validTintaApplicationReceipt(value))
    return 0;
  std::copy_n("TAP\1", 4, output.begin());
  std::copy(value.event.origin.begin(), value.event.origin.end(), output.begin() + 4);
  tinta_body_detail::write(output, 20, value.event.epoch, 8);
  tinta_body_detail::write(output, 28, value.event.sequence, 8);
  std::copy(value.course.begin(), value.course.end(), output.begin() + 36);
  std::copy(value.generation.begin(), value.generation.end(), output.begin() + 52);
  std::copy(value.resource.begin(), value.resource.end(), output.begin() + 68);
  value.entry.encode(output.data() + 100);
  value.before.encode(output.data() + 112);
  value.after.encode(output.data() + 128);
  binary_record::putU32(output.data() + 144, value.responseMilliseconds);
  binary_record::putU32(output.data() + 148, binary_record::crc32(output.data(), 148));
  return TINTA_APPLICATION_RECEIPT_SIZE;
}
inline bool decodeTintaApplicationReceipt(std::span<const uint8_t> input, TintaApplicationReceipt& output) {
  if (input.size() != TINTA_APPLICATION_RECEIPT_SIZE || tintaApplicationReceiptOverlap(input, output) ||
      !std::equal(input.begin(), input.begin() + 4, "TAP\1") ||
      binary_record::getU32(input.data() + 148) != binary_record::crc32(input.data(), 148))
    return false;
  TintaApplicationReceipt value;
  std::copy_n(input.begin() + 4, 16, value.event.origin.begin());
  value.event.epoch = tinta_body_detail::read(input, 20, 8);
  value.event.sequence = tinta_body_detail::read(input, 28, 8);
  std::copy_n(input.begin() + 36, 16, value.course.begin());
  std::copy_n(input.begin() + 52, 16, value.generation.begin());
  std::copy_n(input.begin() + 68, 32, value.resource.begin());
  value.entry = tinta::core::JournalEntry::decode(input.data() + 100);
  if (!tinta::core::ItemState::decode(input.data() + 112, value.before) ||
      !tinta::core::ItemState::decode(input.data() + 128, value.after))
    return false;
  value.responseMilliseconds = binary_record::getU32(input.data() + 144);
  if (!validTintaApplicationReceipt(value)) return false;
  output = value;
  return true;
}
}  // namespace companion
