#pragma once

#include <algorithm>

#include "CompanionCourseBinding.h"
#include "CompanionTransferDeclaration.h"

namespace companion {
inline constexpr size_t COURSE_SWITCH_REQUEST_SIZE = 132;
// Consent applies to exact pack bytes and one transfer on one SD generation.
struct CourseSwitchRequest {
  Identity generation{}, transaction{}, previousCourse{}, nextCourse{};
  Digest previousHash{}, nextHash{};
  bool operator==(const CourseSwitchRequest&) const = default;
};
static_assert(sizeof(CourseSwitchRequest) < 256);
inline bool validCourseSwitchRequest(const CourseSwitchRequest& request) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  return nonzero(request.generation) && nonzero(request.transaction) && nonzero(request.previousCourse) &&
         nonzero(request.nextCourse) && nonzero(request.previousHash) && nonzero(request.nextHash) &&
         request.previousCourse != request.nextCourse && request.previousHash != request.nextHash;
}
// Commit/recovery context; declaration validation happened before receipt of content.
inline bool matchesCourseSwitchTransfer(const CourseSwitchRequest& request, const ContentManifest& next,
                                        const TransferState& state) {
  return validCourseSwitchRequest(request) && validCourseBinding(next) && matchesTransferManifest(next, state) &&
         state.durableOffset == state.length &&
         (state.phase == TransferPhase::Receiving || state.phase == TransferPhase::Installing ||
          state.phase == TransferPhase::Committed) &&
         request.generation == state.storageGeneration && request.transaction == state.transaction &&
         request.nextCourse == next.logicalIdentity && request.nextHash == next.contentHash;
}
// Initial authorization only; recovery uses the durable intent and saved declarations.
inline bool matchesCourseSwitchRequest(const CourseSwitchRequest& request, const Identity& generation,
                                       const ContentManifest& current, const TransferDeclaration& next) {
  return validCourseSwitchRequest(request) && validCourseBinding(current) && validTransferDeclaration(next) &&
         validCourseBinding(next.manifest) && request.generation == generation &&
         request.generation == next.state.storageGeneration && request.transaction == next.state.transaction &&
         request.previousCourse == current.logicalIdentity && request.previousHash == current.contentHash &&
         request.nextCourse == next.manifest.logicalIdentity && request.nextHash == next.manifest.contentHash;
}
inline bool encodeCourseSwitchRequest(const CourseSwitchRequest& request, std::span<uint8_t> output) {
  if (output.size() != COURSE_SWITCH_REQUEST_SIZE || !validCourseSwitchRequest(request)) return false;
  output[0] = 'T';
  output[1] = 'C';
  output[2] = 'S';
  output[3] = 1;
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 4);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 20);
  std::copy(request.previousCourse.begin(), request.previousCourse.end(), output.begin() + 36);
  std::copy(request.nextCourse.begin(), request.nextCourse.end(), output.begin() + 52);
  std::copy(request.previousHash.begin(), request.previousHash.end(), output.begin() + 68);
  std::copy(request.nextHash.begin(), request.nextHash.end(), output.begin() + 100);
  return true;
}
inline bool decodeCourseSwitchRequest(std::span<const uint8_t> input, CourseSwitchRequest& output) {
  if (input.size() != COURSE_SWITCH_REQUEST_SIZE || input[0] != 'T' || input[1] != 'C' || input[2] != 'S' ||
      input[3] != 1)
    return false;
  CourseSwitchRequest request;
  std::copy_n(input.begin() + 4, 16, request.generation.begin());
  std::copy_n(input.begin() + 20, 16, request.transaction.begin());
  std::copy_n(input.begin() + 36, 16, request.previousCourse.begin());
  std::copy_n(input.begin() + 52, 16, request.nextCourse.begin());
  std::copy_n(input.begin() + 68, 32, request.previousHash.begin());
  std::copy_n(input.begin() + 100, 32, request.nextHash.begin());
  if (!validCourseSwitchRequest(request)) return false;
  output = request;
  return true;
}
}  // namespace companion
