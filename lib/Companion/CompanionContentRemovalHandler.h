#pragma once

#include "CompanionContentRemovalRequest.h"

namespace companion {
inline constexpr size_t CONTENT_REMOVAL_REPLY_SIZE = 17;
enum class ContentRemovalResult : uint8_t {
  Ok,
  Invalid,
  Unauthorized,
  WrongStorage,
  Busy,
  NotFound,
  Unsupported,
  Conflict,
  Corrupt,
  IoError
};
// The serialized native owner implements durable lookup and plan admission.
// completed() returns NotFound only for checked absence of this request's receipt.
class ContentRemovalBackend {
 public:
  virtual ~ContentRemovalBackend() = default;
  virtual ContentRemovalResult completed(const ContentRemovalRequest& request) = 0;
  virtual ContentRemovalResult remove(const ContentRemovalRequest& request) = 0;
};
// Session-owned: retained request bytes avoid nested decoder/handler stack use.
class ContentRemovalHandler final {
 public:
  explicit ContentRemovalHandler(ContentRemovalBackend& backend) : backend(backend) {}
  size_t handle(bool authorized, const Identity& owner, const Identity& generation, std::span<const uint8_t> body,
                std::span<uint8_t> reply) {
    if (reply.size() < CONTENT_REMOVAL_REPLY_SIZE) return 0;
    request = {};
    ContentRemovalResult result = ContentRemovalResult::Unauthorized;
    if (authorized) {
      result = ContentRemovalResult::Invalid;
      if (decodeContentRemovalRequest(body, request)) {
        if (request.owner != owner)
          result = ContentRemovalResult::Unauthorized;
        else if (request.generation != generation)
          result = ContentRemovalResult::WrongStorage;
        else {
          result = backend.completed(request);
          if (result == ContentRemovalResult::NotFound) result = backend.remove(request);
        }
      }
    }
    reply[0] = static_cast<uint8_t>(result);
    std::copy(request.transaction.begin(), request.transaction.end(), reply.begin() + 1);
    return CONTENT_REMOVAL_REPLY_SIZE;
  }

 private:
  ContentRemovalBackend& backend;
  ContentRemovalRequest request;
};
}  // namespace companion
