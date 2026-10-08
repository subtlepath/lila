#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {

struct TintaSchedulerConfiguration {
  uint16_t retentionBasisPoints = 9000;
  uint16_t maximumInterval = 365;
  bool operator==(const TintaSchedulerConfiguration&) const = default;
};

struct TintaBody {
  Identity course{};
  uint32_t uid = 0;
  EventKind kind = EventKind::Review;
  uint8_t grade = 0;
  uint8_t format = 0;
  uint32_t responseMilliseconds = 0;
  TintaSchedulerConfiguration configuration{};
  EventIdentity undoTarget{};
  bool enabled = false;
  bool operator==(const TintaBody&) const = default;
};

inline constexpr size_t MAX_TINTA_BODY_SIZE = 54;

namespace tinta_body_detail {
inline uint64_t read(std::span<const uint8_t> input, size_t offset, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(input[offset + i]) << (8 * i);
  return value;
}
inline void write(std::span<uint8_t> output, size_t offset, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) output[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
inline bool nonzero(const auto& bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value != 0; });
}
inline bool validIdentity(const EventIdentity& identity) {
  return nonzero(identity.origin) && identity.epoch != 0 && identity.sequence != 0;
}
inline size_t size(EventKind kind) {
  switch (kind) {
    case EventKind::Review:
      return 34;
    case EventKind::UndoReview:
      return 54;
    case EventKind::Suspension:
    case EventKind::Star:
    case EventKind::LessonComplete:
    case EventKind::ReadingComplete:
      return 23;
    default:
      return 0;
  }
}
inline bool valid(const TintaBody& body) {
  if (!size(body.kind) || !nonzero(body.course) || body.uid == 0 || body.uid == UINT32_MAX) return false;
  if (body.kind == EventKind::Review)
    return body.grade >= 1 && body.grade <= 4 && body.format <= 9 && body.configuration.retentionBasisPoints > 0 &&
           body.configuration.retentionBasisPoints < 10000 && body.configuration.maximumInterval > 0;
  return body.kind != EventKind::UndoReview || validIdentity(body.undoTarget);
}
}  // namespace tinta_body_detail

inline size_t encodeTintaConfiguration(const TintaSchedulerConfiguration& configuration, std::span<uint8_t> output) {
  if (output.size() < 6 || configuration.retentionBasisPoints == 0 || configuration.retentionBasisPoints >= 10000 ||
      configuration.maximumInterval == 0)
    return 0;
  output[0] = 1;
  output[1] = 1;
  tinta_body_detail::write(output, 2, configuration.retentionBasisPoints, 2);
  tinta_body_detail::write(output, 4, configuration.maximumInterval, 2);
  return 6;
}

inline size_t encodeTintaBody(const TintaBody& body, std::span<uint8_t> output) {
  using namespace tinta_body_detail;
  const size_t length = size(body.kind);
  if (!valid(body) || output.size() < length) return 0;
  output[0] = 1;
  output[1] = static_cast<uint8_t>(body.kind);
  std::copy(body.course.begin(), body.course.end(), output.begin() + 2);
  write(output, 18, body.uid, 4);
  if (body.kind == EventKind::Review) {
    if (output.size() < 34) return 0;
    output[22] = body.grade;
    output[23] = body.format;
    write(output, 24, body.responseMilliseconds, 4);
    encodeTintaConfiguration(body.configuration, output.subspan(28, 6));
  } else if (body.kind == EventKind::UndoReview) {
    if (output.size() < 54) return 0;
    std::copy(body.undoTarget.origin.begin(), body.undoTarget.origin.end(), output.begin() + 22);
    write(output, 38, body.undoTarget.epoch, 8);
    write(output, 46, body.undoTarget.sequence, 8);
  } else
    output[22] = body.enabled ? 1 : 0;
  return length;
}

inline bool decodeTintaBody(std::span<const uint8_t> input, TintaBody& output) {
  using namespace tinta_body_detail;
  if (input.size() < 2 || input[0] != 1) return false;
  const auto kind = static_cast<EventKind>(input[1]);
  const size_t length = size(kind);
  if (length == 0 || input.size() != length) return false;
  TintaBody decoded;
  decoded.kind = kind;
  std::copy_n(input.begin() + 2, decoded.course.size(), decoded.course.begin());
  decoded.uid = static_cast<uint32_t>(read(input, 18, 4));
  if (kind == EventKind::Review) {
    if (input.size() != 34) return false;
    if (input[28] != 1 || input[29] != 1) return false;
    decoded.grade = input[22];
    decoded.format = input[23];
    decoded.responseMilliseconds = static_cast<uint32_t>(read(input, 24, 4));
    decoded.configuration.retentionBasisPoints = static_cast<uint16_t>(read(input, 30, 2));
    decoded.configuration.maximumInterval = static_cast<uint16_t>(read(input, 32, 2));
  } else if (kind == EventKind::UndoReview) {
    if (input.size() != 54) return false;
    std::copy_n(input.begin() + 22, decoded.undoTarget.origin.size(), decoded.undoTarget.origin.begin());
    decoded.undoTarget.epoch = read(input, 38, 8);
    decoded.undoTarget.sequence = read(input, 46, 8);
  } else {
    if (input[22] > 1) return false;
    decoded.enabled = input[22] != 0;
  }
  if (!valid(decoded)) return false;
  output = decoded;
  return true;
}

// Digests must be computed from the received body/configuration, not copied from the envelope.
inline bool validateTintaEnvelope(const SyncEvent& event, const TintaBody& body, const Digest& bodyDigest,
                                  const Digest& configurationDigest) {
  using namespace tinta_body_detail;
  if (!valid(body) || !validIdentity(event.identity) || !nonzero(event.storageGeneration) ||
      event.ancestorCount > MAX_ANCESTORS || event.kind != body.kind || event.studyDay > UINT16_MAX ||
      !nonzero(event.resource) || event.bodyHash != bodyDigest)
    return false;
  for (size_t i = 0; i < event.ancestorCount; ++i) {
    const auto& ancestor = event.ancestors[i];
    if (!validIdentity(ancestor) || ancestor == event.identity ||
        (ancestor.origin == event.identity.origin && ancestor.epoch == event.identity.epoch &&
         ancestor.sequence >= event.identity.sequence))
      return false;
    for (size_t j = 0; j < i; ++j)
      if (event.ancestors[j] == ancestor) return false;
  }
  if (body.kind == EventKind::Review)
    return event.schedulerVersion == 1 && event.schedulerConfiguration == configurationDigest;
  if (event.schedulerVersion != 0 || nonzero(event.schedulerConfiguration)) return false;
  if (body.kind == EventKind::UndoReview) {
    for (size_t i = 0; i < event.ancestorCount; ++i)
      if (event.ancestors[i] == body.undoTarget) return true;
    return false;
  }
  return true;
}

}  // namespace companion
