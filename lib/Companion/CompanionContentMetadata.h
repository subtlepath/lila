#pragma once

#include <string_view>

#include "../hal/HalFilenameCodec.h"
#include "CompanionContentRead.h"

namespace companion {
inline constexpr size_t CONTENT_METADATA_REQUEST_SIZE = 83;
inline constexpr size_t CONTENT_METADATA_REPLY_HEADER_SIZE = 85;
struct ContentMetadataRequest {
  Identity generation{};
  ContentManifest manifest{};
};
static_assert(sizeof(ContentMetadataRequest) < 256);
inline bool validContentMetadataRequest(const ContentMetadataRequest& request) {
  ContentReadRequest read;
  read.generation = request.generation;
  read.manifest = request.manifest;
  read.maximumBytes = 1;
  return validContentReadRequest(read);
}
inline bool decodeContentMetadataRequest(std::span<const uint8_t> input, ContentMetadataRequest& output) {
  if (input.size() != CONTENT_METADATA_REQUEST_SIZE || input[0] != 'L' || input[1] != 'C' || input[2] != 'M' ||
      input[3] != 1)
    return false;
  ContentMetadataRequest parsed;
  std::copy_n(input.begin() + 4, 16, parsed.generation.begin());
  if (!decodeRecord(input.subspan(20, CONTENT_MANIFEST_SIZE), parsed.manifest) || !validContentMetadataRequest(parsed))
    return false;
  output = parsed;
  return true;
}
inline bool validContentMetadataFilename(const ContentManifest& manifest, std::string_view name) {
  if (name.empty() || name.size() > 255 || !hal_filename::valid(name)) return false;
  for (const unsigned char byte : name)
    if (byte < 32 || byte == 127 || byte == '/' || byte == '\\') return false;
  const auto dot = name.rfind('.');
  if (dot == std::string_view::npos) return false;
  const auto suffix = name.substr(dot + 1);
  const auto matches = [suffix](std::string_view expected) {
    if (suffix.size() != expected.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
      const char c = suffix[i] >= 'A' && suffix[i] <= 'Z' ? suffix[i] + ('a' - 'A') : suffix[i];
      if (c != expected[i]) return false;
    }
    return true;
  };
  switch (manifest.kind) {
    case ContentKind::Epub:
      return matches("epub");
    case ContentKind::Course:
      return matches("pack");
    case ContentKind::Dictionary:
      return matches("zip");
    case ContentKind::Font:
      return manifest.formatVersion == 4 ? matches("cpfont") : matches("ttf") || matches("otf") || matches("ttc");
    default:
      return false;
  }
}
// The basename may already occupy the output payload; validate before moving it.
inline size_t encodeContentMetadataReply(const ContentMetadataRequest& request, ContentReadResult result,
                                         std::string_view name, std::span<uint8_t> output) {
  if (!validContentMetadataRequest(request) || result > ContentReadResult::IoError ||
      (result == ContentReadResult::Ok ? !validContentMetadataFilename(request.manifest, name) : !name.empty()) ||
      output.size() < CONTENT_METADATA_REPLY_HEADER_SIZE + name.size())
    return 0;
  if (!name.empty()) memmove(output.data() + CONTENT_METADATA_REPLY_HEADER_SIZE, name.data(), name.size());
  output[0] = 'L';
  output[1] = 'C';
  output[2] = 'N';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(result);
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 5);
  encodeRecord(request.manifest, output.subspan(21, CONTENT_MANIFEST_SIZE));
  output[84] = static_cast<uint8_t>(name.size());
  return CONTENT_METADATA_REPLY_HEADER_SIZE + name.size();
}
}  // namespace companion
