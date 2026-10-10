#pragma once

#include <cstddef>
#include <cstdint>

namespace companion {
// Transport mutex protects this state together with its queue and assembler.
// Disconnect invalidates permission, but does not release the borrowed buffer.
class ControlWorkspaceLease final {
 public:
  bool acquire(uint64_t session, bool authenticated, size_t queued, bool assemblerIdle) {
    if (token || !session || !authenticated || queued || !assemblerIdle) return false;
    token = session;
    return true;
  }
  bool active() const { return token != 0; }
  bool owns(uint64_t session) const { return session && token == session; }
  bool valid(uint64_t session, bool authenticated) const { return owns(session) && authenticated; }
  bool release(uint64_t session) {
    if (!owns(session)) return false;
    token = 0;
    return true;
  }

 private:
  uint64_t token = 0;
};
}  // namespace companion
