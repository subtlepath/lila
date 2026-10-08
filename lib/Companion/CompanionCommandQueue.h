#pragma once

#include <array>
#include <span>

#include "CompanionFrame.h"

namespace companion {

inline constexpr size_t COMMAND_SLOT_SIZE = FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD;
inline constexpr size_t COMMAND_QUEUE_STORAGE_SIZE = COMMAND_SLOT_SIZE * MAX_PENDING_COMMANDS;
static_assert(COMMAND_QUEUE_STORAGE_SIZE < SESSION_WORKSPACE_SIZE);

// Single-consumer queue. Transport callbacks must serialize access before using
// it. Frame storage is carved from the session workspace, not allocated here.
class CommandQueue {
 public:
  enum class PushResult : uint8_t { Accepted, Full, Invalid, NoStorage };

  explicit CommandQueue(std::span<uint8_t> storage) : storage(storage) {}

  // Source bytes must not overlap queue storage. Authentication comes from the
  // transport; decoded fields never authorize a command themselves.
  PushResult push(std::span<const uint8_t> bytes, bool authenticated) {
    if (storage.size() < COMMAND_QUEUE_STORAGE_SIZE) return PushResult::NoStorage;
    if (count == MAX_PENDING_COMMANDS) return PushResult::Full;
    FrameView frame;
    if (decodeFrame(bytes, authenticated, frame) != FrameError::None) return PushResult::Invalid;
    const size_t tail = (head + count) % MAX_PENDING_COMMANDS;
    const size_t start = tail * COMMAND_SLOT_SIZE;
    for (size_t i = 0; i < bytes.size(); ++i) storage[start + i] = bytes[i];
    lengths[tail] = bytes.size();
    ++count;
    return PushResult::Accepted;
  }

  // The borrowed payload remains valid until pop(), clear(), or destruction of
  // the session workspace. Failed peek leaves output unchanged.
  bool peek(FrameView& output) const {
    if (count == 0) return false;
    return decodeFrame(storage.subspan(head * COMMAND_SLOT_SIZE, lengths[head]), true, output) == FrameError::None;
  }

  void pop() {
    if (count == 0) return;
    lengths[head] = 0;
    head = (head + 1) % MAX_PENDING_COMMANDS;
    --count;
  }

  void clear() {
    head = 0;
    count = 0;
    lengths.fill(0);
  }

  size_t size() const { return count; }

 private:
  std::span<uint8_t> storage;
  std::array<size_t, MAX_PENDING_COMMANDS> lengths{};
  size_t head = 0;
  size_t count = 0;
};

}  // namespace companion
