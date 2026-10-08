#pragma once
#include <cstdint>
#include <limits>

namespace companion {
// Serialized by the transport mutex. Tokens never repeat during its lifetime.
class Session {
 public:
  bool connect() {
    if (active || generation == std::numeric_limits<uint64_t>::max()) return false;
    ++generation;
    active = true;
    authenticated = false;
    return true;
  }
  void disconnect() {
    active = false;
    authenticated = false;
  }
  void authenticate(bool authorized) { authenticated = active && authorized; }
  uint64_t token() const { return authenticated ? generation : 0; }
  bool accepts(uint64_t token) const { return token != 0 && token == this->token(); }

 private:
  uint64_t generation = 0;
  bool active = false;
  bool authenticated = false;
};
}  // namespace companion
