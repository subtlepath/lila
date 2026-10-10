#pragma once

#include <algorithm>

#include "CompanionTransfer.h"

namespace companion {
enum class TintaReplayStoreTarget { Course, BaselineProof };
enum class BaselineReplayFile : uint8_t { Working, Items, LocalReviews, Lessons, Readings, Days };
// Disposable baseline proof files must not enter the reviewed learner cohort.
inline bool baselineReplayFilePath(const Identity& course, BaselineReplayFile file, std::span<char> output) {
  if (!output.empty()) output[0] = 0;
  static constexpr std::string_view PREFIX = "/.crosspoint/companion/bp-";
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  static constexpr char PARTS[] = {'w', 'i', 'v', 'l', 'r', 'd'};
  const auto part = static_cast<unsigned>(file);
  if (part >= sizeof(PARTS) || output.size() < PREFIX.size() + 35 ||
      std::none_of(course.begin(), course.end(), [](uint8_t byte) { return byte != 0; }))
    return false;
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  size_t at = PREFIX.size();
  for (const auto byte : course) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  output[at++] = '-';
  output[at++] = PARTS[part];
  output[at] = 0;
  return true;
}
}  // namespace companion
