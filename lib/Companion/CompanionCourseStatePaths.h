#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "CompanionRecords.h"

namespace companion {
inline constexpr std::string_view COURSE_STATE_ROOT = "/tinta/courses/";
inline constexpr size_t COURSE_STATE_DIRECTORY_SIZE = COURSE_STATE_ROOT.size() + 32 + 1;
inline constexpr size_t COURSE_STATE_PATH_SIZE = COURSE_STATE_DIRECTORY_SIZE + 24;
inline bool courseStateDirectory(const Identity& course, std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  if (output.size() < COURSE_STATE_DIRECTORY_SIZE ||
      std::none_of(course.begin(), course.end(), [](uint8_t byte) { return byte != 0; }))
    return false;
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::copy(COURSE_STATE_ROOT.begin(), COURSE_STATE_ROOT.end(), output.begin());
  for (size_t index = 0; index < course.size(); ++index) {
    output[COURSE_STATE_ROOT.size() + index * 2] = HEX_DIGITS[course[index] >> 4];
    output[COURSE_STATE_ROOT.size() + index * 2 + 1] = HEX_DIGITS[course[index] & 15];
  }
  output[COURSE_STATE_DIRECTORY_SIZE - 1] = '\0';
  return true;
}
inline bool courseStatePath(const Identity& course, std::string_view name, std::span<char> output) {
  if (!output.empty()) output[0] = '\0';
  if (name.empty() || name.size() >= 24 || name == "." || name == ".." ||
      !std::all_of(name.begin(), name.end(),
                   [](unsigned char byte) {
                     return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' || byte == '_';
                   }) ||
      output.size() < COURSE_STATE_DIRECTORY_SIZE + name.size() + 1)
    return false;
  if (!courseStateDirectory(course, output)) return false;
  output[COURSE_STATE_DIRECTORY_SIZE - 1] = '/';
  std::copy(name.begin(), name.end(), output.begin() + COURSE_STATE_DIRECTORY_SIZE);
  output[COURSE_STATE_DIRECTORY_SIZE + name.size()] = '\0';
  return true;
}
}  // namespace companion
