#pragma once

#include <string_view>

#include "CompanionZipPathValidation.h"

namespace companion {
inline bool validFontDestination(std::string_view path, uint32_t format) {
  if (path.size() >= 128 || (format != 4 && format != 1)) return false;
  if (path.starts_with("/fonts/"))
    path.remove_prefix(7);
  else if (path.starts_with("/.fonts/"))
    path.remove_prefix(8);
  else
    return false;
  ZipPathValidation validation;
  ZipPathDetails details;
  validation.reset();
  if (!validation.consume(std::span(reinterpret_cast<const uint8_t*>(path.data()), path.size())) ||
      !validation.finish(false, details) || path.front() == '.' || path.front() == '_')
    return false;
  const auto slash = path.find('/');
  const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (name.empty() || name.front() == '.' || name.front() == '_' || name.find('/') != std::string_view::npos)
    return false;
  if (format == 4) {
    if (slash == std::string_view::npos || !name.ends_with(".cpfont")) return false;
    const auto stem = name.substr(0, name.size() - 7);
    const auto underscore = stem.rfind('_');
    if (underscore == std::string_view::npos || underscore == 0 || underscore + 1 == stem.size()) return false;
    unsigned size = 0;
    for (const char byte : stem.substr(underscore + 1)) {
      if (byte < '0' || byte > '9' || size > 255) return false;
      size = size * 10 + static_cast<unsigned>(byte - '0');
    }
    return size > 0 && size <= 255;
  }
  if (name.size() <= 4) return false;
  char suffix[4];
  for (unsigned at = 0; at < 4; ++at) {
    const char byte = name[name.size() - 4 + at];
    suffix[at] = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
  }
  const std::string_view extension(suffix, sizeof(suffix));
  return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}
}  // namespace companion
