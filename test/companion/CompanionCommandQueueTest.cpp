#include <gtest/gtest.h>

#include <array>

#include "lib/Companion/CompanionCommandQueue.h"

using namespace companion;

TEST(CompanionCommandQueue, BoundsCapacityPreservesFifoAndWrapsSlots) {
  std::array<uint8_t, COMMAND_QUEUE_STORAGE_SIZE> workspace{};
  CommandQueue queue(workspace);
  std::array<uint8_t, COMMAND_SLOT_SIZE> bytes{};
  std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload{};
  FrameView input{Command::TransferChunk, false, 0, payload};
  FrameView output;
  for (uint32_t i = 0; i < MAX_PENDING_COMMANDS; ++i) {
    input.requestId = i;
    payload[0] = i;
    ASSERT_EQ(encodeFrame(input, bytes), bytes.size());
    ASSERT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Accepted);
  }
  EXPECT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Full);
  ASSERT_TRUE(queue.peek(output));
  EXPECT_EQ(output.requestId, 0U);
  EXPECT_EQ(output.payload[0], 0U);
  for (uint32_t i = 0; i < 12; ++i) {
    ASSERT_TRUE(queue.peek(output));
    EXPECT_EQ(output.requestId, i);
    EXPECT_EQ(output.payload[0], i);
    queue.pop();
    input.requestId = i + MAX_PENDING_COMMANDS;
    payload[0] = input.requestId;
    encodeFrame(input, bytes);
    ASSERT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Accepted);
  }
  EXPECT_EQ(queue.size(), MAX_PENDING_COMMANDS);
  queue.clear();
  EXPECT_EQ(queue.size(), 0U);
  output.requestId = 42;
  EXPECT_FALSE(queue.peek(output));
  EXPECT_EQ(output.requestId, 42U);
  queue.pop();
  EXPECT_EQ(queue.size(), 0U);
}

TEST(CompanionCommandQueue, CopiesBorrowedPayloadAndRejectsUnauthorizedFrames) {
  std::array<uint8_t, COMMAND_QUEUE_STORAGE_SIZE> workspace{};
  CommandQueue queue(workspace);
  std::array<uint8_t, 13> bytes{};
  std::array<uint8_t, 1> payload{7};
  FrameView input{Command::Commit, false, 1, payload};
  ASSERT_EQ(encodeFrame(input, bytes), bytes.size());
  EXPECT_EQ(queue.push(bytes, false), CommandQueue::PushResult::Invalid);
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Accepted);
  bytes.fill(0);
  FrameView output;
  ASSERT_TRUE(queue.peek(output));
  EXPECT_EQ(output.payload[0], 7U);
  EXPECT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Invalid);
  EXPECT_EQ(queue.size(), 1U);
}

TEST(CompanionCommandQueue, RejectsInsufficientWorkspace) {
  std::array<uint8_t, FRAME_HEADER_SIZE> bytes{};
  FrameView input;
  ASSERT_EQ(encodeFrame(input, bytes), bytes.size());
  CommandQueue queue(bytes);
  EXPECT_EQ(queue.push(bytes, true), CommandQueue::PushResult::NoStorage);
  EXPECT_EQ(queue.size(), 0U);
}
