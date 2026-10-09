#pragma once

#include "CompanionContentExportBinding.h"
#include "HalContentReadSession.h"

namespace companion {
inline size_t admitContentExport(ContentExportBinding& binding, const ContentHandoffRequest& request, bool authorized,
                                 const Identity& installation, const Identity& generation, uint64_t revision, bool busy,
                                 HalContentReadSession& session, InventoryPaths& paths, std::span<uint8_t> scratch,
                                 std::span<uint8_t> output) {
  auto result = ContentReadResult::Unauthorized;
  if (!authorized || !content_read_detail::nonzero(installation))
    return encodeContentHandoffReply(request, result, output);
  if (request.read.generation != generation)
    return encodeContentHandoffReply(request, ContentReadResult::WrongStorage, output);
  if (!validContentHandoffRequest(request) || output.size() < CONTENT_HANDOFF_REPLY_SIZE) return 0;
  std::array<uint8_t, CONTENT_READ_REQUEST_SIZE> wire{};
  if (!encodeContentReadRequest(request.read, wire)) return 0;
  const size_t length = session.reply(true, installation, generation, revision, busy, paths, wire, scratch, output);
  ContentReadReplyView verified;
  if (!decodeContentReadReply(output.first(length), request.read, verified))
    result = ContentReadResult::IoError;
  else
    result = verified.result;
  result = binding.admit(request, true, installation, generation, revision, result);
  return encodeContentHandoffReply(request, result, output);
}
}  // namespace companion
