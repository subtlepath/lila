#pragma once

#include "CompanionBaselineReplayPaths.h"
#include "CompanionTintaDerivedPaths.h"

namespace companion {
enum class TintaReplayExportTarget : uint8_t { Candidate, Proof, BaselineProof };
inline constexpr std::string_view TINTA_REPLAY_PROOF_NAMES[] = {"items.proof", "reviews.proof", "lessons.proof",
                                                                "readings.proof", "days.proof"};
inline bool tintaReplayExportPath(const Identity& course, TintaDerivedFile file, TintaReplayExportTarget target,
                                  std::span<char> output) {
  if (!output.empty()) output[0] = 0;
  if (target == TintaReplayExportTarget::Candidate)
    return tintaDerivedFilePath(course, file, TintaDerivedRole::Candidate, output);
  const auto index = static_cast<unsigned>(file);
  if (target == TintaReplayExportTarget::BaselineProof && index < 5)
    return baselineReplayFilePath(course, static_cast<BaselineReplayFile>(index + 1), output);
  if (target != TintaReplayExportTarget::Proof || index >= 5) return false;
  return courseStatePath(course, TINTA_REPLAY_PROOF_NAMES[index], output);
}
}  // namespace companion
