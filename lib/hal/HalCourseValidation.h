#pragma once

#if LILA_TINTA

#include <span>

#include "core/pack/Pack.h"

namespace companion {
struct CourseCandidateDetails {
  uint16_t major = 0, minor = 0;
  uint32_t edition = 0, items = 0;
  uint16_t lessons = 0, stories = 0;
  char locale[9]{};
};
// Pack is caller-owned outside the stack. Scratch needs at least 512 bytes.
// Output changes only on success; Pack is closed on every return.
bool validateStagedCourse(const char* path, tinta::core::pack::Pack& pack, std::span<uint8_t> scratch,
                          CourseCandidateDetails& details);
}  // namespace companion

#endif  // LILA_TINTA
