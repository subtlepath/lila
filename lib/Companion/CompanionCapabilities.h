#pragma once
#include <cstdint>
namespace companion {
inline constexpr uint32_t CAPABILITY_DECLARED_TRANSFERS = uint32_t{1} << 0;
inline constexpr uint32_t CAPABILITY_COURSE_TRANSFERS = uint32_t{1} << 1;
inline constexpr uint32_t CAPABILITY_JOURNAL_FORMATS = uint32_t{1} << 2;
inline constexpr uint32_t CAPABILITY_JOURNAL_EXPORT = uint32_t{1} << 3;
inline constexpr uint32_t CAPABILITY_COURSE_SWITCHES = uint32_t{1} << 4;
inline constexpr uint32_t CAPABILITY_FONT_TRANSFERS = uint32_t{1} << 5;
inline constexpr uint32_t CAPABILITY_VECTOR_FONT_TRANSFERS = uint32_t{1} << 6;
inline constexpr uint32_t CAPABILITY_DICTIONARY_TRANSFERS = uint32_t{1} << 7;
inline constexpr uint32_t CAPABILITY_EPUB_REMOVALS = uint32_t{1} << 8;
inline constexpr uint32_t CAPABILITY_FONT_REMOVALS = uint32_t{1} << 13;
inline constexpr uint32_t CAPABILITY_DICTIONARY_REMOVALS = uint32_t{1} << 14;
inline constexpr uint32_t CAPABILITY_JOURNAL_MERGE_READINESS = uint32_t{1} << 9;
inline constexpr uint32_t CAPABILITY_WIFI_CONTENT_READS = uint32_t{1} << 12;
inline constexpr uint32_t CAPABILITY_CONTENT_METADATA = uint32_t{1} << 11;
inline constexpr uint32_t CAPABILITY_CONTENT_READS = uint32_t{1} << 10;
inline constexpr bool supportsCourseTransfer(uint32_t capabilities) {
  constexpr uint32_t required = CAPABILITY_DECLARED_TRANSFERS | CAPABILITY_COURSE_TRANSFERS;
  return (capabilities & required) == required;
}
}  // namespace companion
