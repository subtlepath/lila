#pragma once

#include <algorithm>
#include <span>

#include "CompanionFrame.h"

namespace companion {

// One in-flight frame. Storage belongs to the session workspace; reset only
// after the consumer has finished using the decoded payload view.
class FrameAssembler {
 public:
  enum class Result : uint8_t { NeedMore, Ready, Rejected };

  explicit FrameAssembler(std::span<uint8_t> storage) : storage(storage) {}

  void reset() {
    used = 0;
    expected = FRAME_HEADER_SIZE;
    terminal = false;
    error = FrameError::None;
  }

  Result append(std::span<const uint8_t> fragment, bool authenticated, FrameView& output) {
    if (terminal) return Result::Rejected;
    if (storage.size() < FRAME_HEADER_SIZE || fragment.size() > storage.size() - used) {
      return reject(FrameError::Length);
    }
    // Copy only the header until its declared bound has been checked.
    const size_t headerBytes = std::min(fragment.size(), FRAME_HEADER_SIZE - std::min(used, FRAME_HEADER_SIZE));
    for (size_t i = 0; i < headerBytes; ++i) storage[used++] = fragment[i];
    fragment = fragment.subspan(headerBytes);
    if (used < FRAME_HEADER_SIZE) return Result::NeedMore;
    if (storage[0] != 'L' || storage[1] != 'C') return reject(FrameError::Magic);
    if (storage[2] != PROTOCOL_VERSION) return reject(FrameError::Version);
    if (!validCommand(storage[3])) return reject(FrameError::Command);
    if ((storage[4] & ~1U) != 0 || storage[5] != 0) return reject(FrameError::Flags);
    if (!authenticated && storage[3] != static_cast<uint8_t>(Command::Discover)) {
      return reject(FrameError::Unauthorized);
    }
    const size_t length = static_cast<size_t>(storage[10]) | (static_cast<size_t>(storage[11]) << 8U);
    if (length > MAX_CONTROL_PAYLOAD || FRAME_HEADER_SIZE + length > storage.size()) {
      return reject(FrameError::Length);
    }
    expected = FRAME_HEADER_SIZE + length;
    if (fragment.size() > expected - used) return reject(FrameError::Length);
    for (const uint8_t byte : fragment) storage[used++] = byte;
    if (used < expected) return Result::NeedMore;
    error = decodeFrame(storage.first(used), authenticated, output);
    terminal = true;
    return error == FrameError::None ? Result::Ready : Result::Rejected;
  }

  FrameError lastError() const { return error; }

 private:
  Result reject(FrameError reason) {
    terminal = true;
    error = reason;
    return Result::Rejected;
  }

  std::span<uint8_t> storage;
  size_t used = 0;
  size_t expected = FRAME_HEADER_SIZE;
  bool terminal = false;
  FrameError error = FrameError::None;
};

}  // namespace companion
