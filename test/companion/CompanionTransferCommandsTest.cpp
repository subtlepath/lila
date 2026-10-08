#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "lib/Companion/CompanionTransferCommands.h"

using namespace companion;
TEST(CompanionTransferCommands, BeginRequiresExactBoundedPathAndInitialState) {
  std::array<uint8_t, 240> bytes{};
  TransferState state;
  state.length = 123;
  ASSERT_EQ(encodeRecord(state, bytes), TRANSFER_STATE_SIZE);
  constexpr char path[] = "/books/book.epub";
  bytes[TRANSFER_STATE_SIZE] = sizeof(path) - 1;
  std::memcpy(bytes.data() + TRANSFER_STATE_SIZE + 1, path, sizeof(path) - 1);
  const size_t size = TRANSFER_STATE_SIZE + sizeof(path);
  BeginTransferCommand output;
  ASSERT_TRUE(decodeBeginTransfer(std::span(bytes).first(size), output));
  EXPECT_EQ(output.destination, path);
  EXPECT_EQ(output.state, state);
  for (size_t n = 0; n < size; ++n) EXPECT_FALSE(decodeBeginTransfer(std::span(bytes).first(n), output));
  EXPECT_FALSE(decodeBeginTransfer(std::span(bytes).first(size + 1), output));
  bytes[TRANSFER_STATE_SIZE + 2] = 0;
  EXPECT_FALSE(decodeBeginTransfer(std::span(bytes).first(size), output));
  bytes[TRANSFER_STATE_SIZE + 2] = 'b';
  state.durableOffset = 1;
  encodeRecord(state, bytes);
  EXPECT_FALSE(decodeBeginTransfer(std::span(bytes).first(size), output));
}
TEST(CompanionTransferCommands, ChunkSupportsUnalignedOffsetsAndBoundedBodies) {
  std::array<uint8_t, MAX_CONTROL_PAYLOAD + 2> bytes{};
  auto payload = std::span(bytes).subspan(1);
  payload[0] = 7;
  payload[16] = 0x89;
  payload[23] = 0xAB;
  TransferChunkCommand output;
  ASSERT_TRUE(decodeTransferChunk(payload.first(MAX_CONTROL_PAYLOAD), output));
  EXPECT_EQ(output.transaction[0], 7);
  EXPECT_EQ(output.offset, 0xAB00000000000089ULL);
  EXPECT_EQ(output.bytes.size(), MAX_CHUNK_SIZE);
  for (size_t n = 0; n <= CHUNK_HEADER_SIZE; ++n) EXPECT_FALSE(decodeTransferChunk(payload.first(n), output));
  EXPECT_FALSE(decodeTransferChunk(payload, output));
  EXPECT_EQ(output.offset, 0xAB00000000000089ULL);
}
TEST(CompanionTransferCommands, TransactionCommandsRequireExactIdentity) {
  std::array<uint8_t, 17> bytes{};
  bytes[0] = 42;
  Identity output{};
  EXPECT_FALSE(decodeTransaction(bytes, output));
  EXPECT_FALSE(decodeTransaction(std::span(bytes).first(15), output));
  EXPECT_TRUE(decodeTransaction(std::span(bytes).first(16), output));
  EXPECT_EQ(output[0], 42);
}
