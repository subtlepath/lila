#pragma once

#include "CompanionDictionaryInstallationPlan.h"

namespace companion {
// Caller-owned buffers keep publication/recovery paths outside the task stack.
inline constexpr size_t DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY = 128 + 8;
inline bool dictionaryInstallationMemberPath(const DictionaryInstallationPlan& plan, unsigned member,
                                             std::span<char> output) {
  if (member >= 4 || !validDictionaryInstallationPlan(plan) || (member == 3 && !plan.extraction.synonyms)) return false;
  static constexpr std::string_view SUFFIXES[] = {".dict", ".idx", ".ifo", ".syn"};
  const auto suffix = member == 0 && plan.extraction.compressed ? std::string_view(".dict.dz") : SUFFIXES[member];
  const auto length = strnlen(plan.base.data(), plan.base.size());
  if (output.size() <= length + suffix.size()) return false;
  // The output may reuse the caller's base-path buffer.
  memmove(output.data(), plan.base.data(), length);
  memcpy(output.data() + length, suffix.data(), suffix.size());
  output[length + suffix.size()] = '\0';
  return true;
}
}  // namespace companion
