#pragma once

#include <algorithm>
#include <cstring>

#include "CompanionFrame.h"
#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t CONTENT_READ_REQUEST_SIZE = 93;
inline constexpr size_t CONTENT_READ_REPLY_HEADER_SIZE = 63;
inline constexpr size_t MAX_CONTENT_READ_BYTES = MAX_CONTROL_PAYLOAD - CONTENT_READ_REPLY_HEADER_SIZE;
struct ContentReadRequest {
  Identity generation{};
  ContentManifest manifest{};
  uint64_t offset = 0;
  uint16_t maximumBytes = 0;
  bool operator==(const ContentReadRequest&) const = default;
};
static_assert(sizeof(ContentReadRequest) < 256);
enum class ContentReadResult : uint8_t { Ok, Invalid, Unauthorized, WrongStorage, Busy, NotFound, Corrupt, IoError };
struct ContentReadReplyView {
  ContentReadResult result = ContentReadResult::Invalid;
  std::span<const uint8_t> bytes{};
};
namespace content_read_detail {
inline bool nonzero(std::span<const uint8_t> bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value != 0; });
}
inline uint64_t read(std::span<const uint8_t> input, size_t offset, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(input[offset + i]) << (8 * i);
  return value;
}
inline void write(std::span<uint8_t> output, size_t offset, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) output[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
}  // namespace content_read_detail
inline bool validContentReadRequest(const ContentReadRequest& request) {
  const auto& manifest = request.manifest;
  if (!content_read_detail::nonzero(request.generation) || !content_read_detail::nonzero(manifest.contentHash) ||
      manifest.length == 0 || request.offset >= manifest.length || request.maximumBytes == 0 ||
      request.maximumBytes > MAX_CONTENT_READ_BYTES)
    return false;
  const bool family = content_read_detail::nonzero(manifest.logicalIdentity);
  switch (manifest.kind) {
    case ContentKind::Epub:
      return manifest.formatVersion <= 1 && !family;
    case ContentKind::Course:
      return manifest.formatVersion == 1 && family;
    case ContentKind::Font:
      return (manifest.formatVersion == 1 || manifest.formatVersion == 4) && !family;
    case ContentKind::Dictionary:
      return manifest.formatVersion == 1 && !family;
    default:
      return false;
  }
}
inline size_t encodeContentReadRequest(const ContentReadRequest& request, std::span<uint8_t> output) {
  if (output.size() < CONTENT_READ_REQUEST_SIZE || !validContentReadRequest(request)) return 0;
  output[0] = 'L';
  output[1] = 'C';
  output[2] = 'R';
  output[3] = 1;
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 4);
  encodeRecord(request.manifest, output.subspan(20, CONTENT_MANIFEST_SIZE));
  content_read_detail::write(output, 83, request.offset, 8);
  content_read_detail::write(output, 91, request.maximumBytes, 2);
  return CONTENT_READ_REQUEST_SIZE;
}
inline bool decodeContentReadRequest(std::span<const uint8_t> input, ContentReadRequest& output) {
  if (input.size() != CONTENT_READ_REQUEST_SIZE || input[0] != 'L' || input[1] != 'C' || input[2] != 'R' ||
      input[3] != 1)
    return false;
  ContentReadRequest parsed;
  std::copy_n(input.begin() + 4, 16, parsed.generation.begin());
  if (!decodeRecord(input.subspan(20, CONTENT_MANIFEST_SIZE), parsed.manifest)) return false;
  parsed.offset = content_read_detail::read(input, 83, 8);
  parsed.maximumBytes = static_cast<uint16_t>(content_read_detail::read(input, 91, 2));
  if (!validContentReadRequest(parsed)) return false;
  output = parsed;
  return true;
}
inline size_t contentReadByteCount(const ContentReadRequest& request) {
  return validContentReadRequest(request)
             ? static_cast<size_t>(std::min<uint64_t>(request.maximumBytes, request.manifest.length - request.offset))
             : 0;
}
// File bytes may already occupy output.subspan(CONTENT_READ_REPLY_HEADER_SIZE).
inline size_t encodeContentReadReply(const ContentReadRequest& request, ContentReadResult result,
                                     std::span<const uint8_t> bytes, std::span<uint8_t> output) {
  if (!validContentReadRequest(request) ||
      static_cast<uint8_t>(result) > static_cast<uint8_t>(ContentReadResult::IoError) ||
      (result == ContentReadResult::Ok ? bytes.size() != contentReadByteCount(request) : !bytes.empty()) ||
      output.size() < CONTENT_READ_REPLY_HEADER_SIZE + bytes.size())
    return 0;
  if (!bytes.empty()) std::memmove(output.data() + CONTENT_READ_REPLY_HEADER_SIZE, bytes.data(), bytes.size());
  output[0] = 'L';
  output[1] = 'C';
  output[2] = 'S';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(result);
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 5);
  std::copy(request.manifest.contentHash.begin(), request.manifest.contentHash.end(), output.begin() + 21);
  content_read_detail::write(output, 53, request.offset, 8);
  content_read_detail::write(output, 61, bytes.size(), 2);
  return CONTENT_READ_REPLY_HEADER_SIZE + bytes.size();
}
inline bool decodeContentReadReply(std::span<const uint8_t> input, const ContentReadRequest& request,
                                   ContentReadReplyView& output) {
  if (!validContentReadRequest(request) || input.size() < CONTENT_READ_REPLY_HEADER_SIZE ||
      input.size() > MAX_CONTROL_PAYLOAD || input[0] != 'L' || input[1] != 'C' || input[2] != 'S' || input[3] != 1 ||
      input[4] > static_cast<uint8_t>(ContentReadResult::IoError) ||
      !std::equal(request.generation.begin(), request.generation.end(), input.begin() + 5) ||
      !std::equal(request.manifest.contentHash.begin(), request.manifest.contentHash.end(), input.begin() + 21) ||
      content_read_detail::read(input, 53, 8) != request.offset)
    return false;
  const auto result = static_cast<ContentReadResult>(input[4]);
  const size_t count = static_cast<size_t>(content_read_detail::read(input, 61, 2));
  if (input.size() != CONTENT_READ_REPLY_HEADER_SIZE + count ||
      (result == ContentReadResult::Ok ? count != contentReadByteCount(request) : count != 0))
    return false;
  output.result = result;
  output.bytes = input.subspan(CONTENT_READ_REPLY_HEADER_SIZE);
  return true;
}
}  // namespace companion
