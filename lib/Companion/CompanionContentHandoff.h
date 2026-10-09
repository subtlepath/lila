#pragma once

#include "CompanionContentRead.h"

namespace companion {
inline constexpr size_t CONTENT_HANDOFF_REQUEST_SIZE = 107;
inline constexpr size_t CONTENT_HANDOFF_REPLY_SIZE = 77;
struct ContentHandoffRequest {
  Identity transaction{};
  ContentReadRequest read{};
};
static_assert(sizeof(ContentHandoffRequest) < 256);
inline bool validContentHandoffRequest(const ContentHandoffRequest& request) {
  return content_read_detail::nonzero(request.transaction) && validContentReadRequest(request.read) &&
         request.read.maximumBytes == 1 && request.read.manifest.length - request.read.offset > WIFI_TRANSFER_THRESHOLD;
}
inline bool decodeContentHandoffRequest(std::span<const uint8_t> input, ContentHandoffRequest& output) {
  if (input.size() != CONTENT_HANDOFF_REQUEST_SIZE || input[0] != 'L' || input[1] != 'C' || input[2] != 'W' ||
      input[3] != 1)
    return false;
  ContentHandoffRequest parsed;
  std::copy_n(input.begin() + 4, 16, parsed.transaction.begin());
  std::copy_n(input.begin() + 20, 16, parsed.read.generation.begin());
  if (!decodeRecord(input.subspan(36, CONTENT_MANIFEST_SIZE), parsed.read.manifest)) return false;
  parsed.read.offset = content_read_detail::read(input, 99, 8);
  parsed.read.maximumBytes = 1;
  if (!validContentHandoffRequest(parsed)) return false;
  output = parsed;
  return true;
}
inline size_t encodeContentHandoffReply(const ContentHandoffRequest& request, ContentReadResult result,
                                        std::span<uint8_t> output) {
  if (!validContentHandoffRequest(request) || result > ContentReadResult::IoError ||
      output.size() < CONTENT_HANDOFF_REPLY_SIZE)
    return 0;
  output[0] = 'L';
  output[1] = 'C';
  output[2] = 'T';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(result);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 5);
  std::copy(request.read.generation.begin(), request.read.generation.end(), output.begin() + 21);
  std::copy(request.read.manifest.contentHash.begin(), request.read.manifest.contentHash.end(), output.begin() + 37);
  content_read_detail::write(output, 69, request.read.offset, 8);
  return CONTENT_HANDOFF_REPLY_SIZE;
}
}  // namespace companion
