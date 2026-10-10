#include <gtest/gtest.h>

#include <array>

#include "lib/Companion/CompanionCommandQueue.h"
#include "lib/Companion/CompanionControlWorkspaceLease.h"
#include "lib/Companion/CompanionFrameAssembler.h"
#include "lib/Companion/CompanionSession.h"

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

TEST(CompanionControlWorkspaceLease, AcquisitionPreservesQueuedCommandsAndIncompleteFrames) {
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  CommandQueue queue{std::span(workspace).first(COMMAND_QUEUE_STORAGE_SIZE)};
  FrameAssembler assembler{std::span(workspace).subspan(COMMAND_QUEUE_STORAGE_SIZE, COMMAND_SLOT_SIZE)};
  Session session;
  ControlWorkspaceLease lease;
  ASSERT_TRUE(session.connect());
  EXPECT_FALSE(lease.acquire(session.token(), session.accepts(session.token()), queue.size(), assembler.idle()));
  session.authenticate(true);
  const auto token = session.token();
  std::array<uint8_t, FRAME_HEADER_SIZE + 1> bytes{};
  std::array<uint8_t, 1> payload{7};
  FrameView input{Command::Commit, false, 1, payload}, output;
  ASSERT_EQ(encodeFrame(input, bytes), bytes.size());
  ASSERT_EQ(queue.push(bytes, true), CommandQueue::PushResult::Accepted);
  const auto queued = workspace;
  EXPECT_FALSE(lease.acquire(token, session.accepts(token), queue.size(), assembler.idle()));
  EXPECT_EQ(workspace, queued);
  ASSERT_TRUE(queue.peek(output));
  EXPECT_EQ(output.requestId, 1U);
  EXPECT_EQ(output.payload[0], 7U);
  queue.pop();
  ASSERT_EQ(assembler.append(std::span(bytes).first(5), true, output), FrameAssembler::Result::NeedMore);
  const auto partial = workspace;
  EXPECT_FALSE(assembler.idle());
  EXPECT_FALSE(lease.acquire(token, session.accepts(token), queue.size(), assembler.idle()));
  EXPECT_EQ(workspace, partial);
  ASSERT_EQ(assembler.append(std::span(bytes).subspan(5), true, output), FrameAssembler::Result::Ready);
  EXPECT_EQ(output.payload[0], 7U);
  EXPECT_FALSE(lease.acquire(token, session.accepts(token), queue.size(), assembler.idle()));
  assembler.reset();
  ASSERT_TRUE(lease.acquire(token, session.accepts(token), queue.size(), assembler.idle()));
  EXPECT_TRUE(lease.valid(token, session.accepts(token)));
  EXPECT_FALSE(lease.acquire(token, true, 0, true));
}

TEST(CompanionControlWorkspaceLease, DisconnectRevokesPermissionWithoutReleasingBorrowedStorage) {
  Session session;
  ControlWorkspaceLease lease;
  ASSERT_TRUE(session.connect());
  session.authenticate(true);
  const auto original = session.token();
  ASSERT_TRUE(lease.acquire(original, session.accepts(original), 0, true));
  session.disconnect();
  EXPECT_FALSE(lease.valid(original, session.accepts(original)));
  EXPECT_TRUE(lease.active());
  ASSERT_TRUE(session.connect());
  session.authenticate(true);
  const auto next = session.token();
  ASSERT_NE(next, original);
  EXPECT_FALSE(lease.acquire(next, session.accepts(next), 0, true));
  EXPECT_FALSE(lease.release(next));
  EXPECT_TRUE(lease.owns(original));
  ASSERT_TRUE(lease.release(original));
  EXPECT_FALSE(lease.active());
  EXPECT_FALSE(lease.release(original));
  EXPECT_TRUE(lease.acquire(next, session.accepts(next), 0, true));
  EXPECT_FALSE(lease.valid(original, true));
  EXPECT_FALSE(lease.release(0));
}

TEST(CompanionControlWorkspaceLease, FullScratchReuseCanReturnToFreshFrameAssembly) {
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace{};
  CommandQueue queue{std::span(workspace).first(COMMAND_QUEUE_STORAGE_SIZE)};
  FrameAssembler assembler{std::span(workspace).subspan(COMMAND_QUEUE_STORAGE_SIZE, COMMAND_SLOT_SIZE)};
  ControlWorkspaceLease lease;
  ASSERT_TRUE(lease.acquire(1, true, queue.size(), assembler.idle()));
  workspace.fill(0xa5);
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_TRUE(assembler.idle());
  ASSERT_TRUE(lease.release(1));
  queue.clear();
  assembler.reset();
  std::array<uint8_t, FRAME_HEADER_SIZE + 1> encoded{};
  std::array<uint8_t, 1> payload{9};
  FrameView input{Command::Commit, false, 42, payload}, output;
  ASSERT_EQ(encodeFrame(input, encoded), encoded.size());
  ASSERT_EQ(assembler.append(encoded, true, output), FrameAssembler::Result::Ready);
  ASSERT_EQ(queue.push(std::span(workspace).subspan(COMMAND_QUEUE_STORAGE_SIZE, encoded.size()), true),
            CommandQueue::PushResult::Accepted);
  assembler.reset();
  ASSERT_TRUE(queue.peek(output));
  EXPECT_EQ(output.requestId, 42U);
  EXPECT_EQ(output.payload[0], 9U);
}
