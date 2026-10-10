#include <gtest/gtest.h>

#include <array>

#include "lib/Companion/CompanionFrame.h"
#include "lib/Companion/CompanionFrameAssembler.h"

using namespace companion;

TEST(CompanionFrame, DecodesUnalignedGoldenFrame) {
  constexpr std::array<uint8_t, 16> bytes = {0, 'L', 'C', 1, 4, 0, 0, 0x78, 0x56, 0x34, 0x12, 3, 0, 9, 8, 7};
  FrameView frame;
  ASSERT_EQ(decodeFrame(std::span(bytes).subspan(1), true, frame), FrameError::None);
  EXPECT_EQ(frame.command, Command::BeginTransfer);
  EXPECT_EQ(frame.requestId, 0x12345678U);
  EXPECT_EQ(frame.payload.size(), 3U);
  EXPECT_EQ(frame.payload[2], 7);
  std::array<uint8_t, 15> encoded{};
  ASSERT_EQ(encodeFrame(frame, encoded), encoded.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), bytes.begin() + 1));
}

TEST(CompanionFrame, RequiresAuthenticationExceptDiscovery) {
  std::array<uint8_t, FRAME_HEADER_SIZE> bytes{};
  FrameView output;
  for (uint8_t command = 1; command <= static_cast<uint8_t>(Command::CourseBaselineReview); ++command) {
    FrameView frame{static_cast<Command>(command), false, 1, {}};
    ASSERT_EQ(encodeFrame(frame, bytes), bytes.size());
    EXPECT_EQ(decodeFrame(bytes, false, output), command == 1 ? FrameError::None : FrameError::Unauthorized);
  }
}

TEST(CompanionFrame, RejectsEveryTruncatedPrefixWithoutPublishingOutput) {
  constexpr std::array<uint8_t, 15> bytes = {'L', 'C', 1, 1, 0, 0, 1, 0, 0, 0, 3, 0, 9, 8, 7};
  FrameView output{Command::Abort, true, 42, {}};
  for (size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_NE(decodeFrame(std::span(bytes).first(length), true, output), FrameError::None);
    EXPECT_EQ(output.requestId, 42U);
    EXPECT_EQ(output.command, Command::Abort);
  }
}

TEST(CompanionFrame, RejectsMalformedHeadersAndTrailingBytes) {
  constexpr std::array<uint8_t, 12> valid = {'L', 'C', 1, 1, 0, 0, 1, 0, 0, 0, 0, 0};
  FrameView output;
  for (size_t index : {0U, 1U, 2U, 3U, 4U, 5U, 10U, 11U}) {
    auto bytes = valid;
    bytes[index] = 0xff;
    EXPECT_NE(decodeFrame(bytes, true, output), FrameError::None);
  }
  std::array<uint8_t, 13> extra{};
  std::copy(valid.begin(), valid.end(), extra.begin());
  EXPECT_EQ(decodeFrame(extra, true, output), FrameError::Length);
}

TEST(CompanionFrame, EnforcesPayloadLimitAndDestinationCapacity) {
  std::array<uint8_t, MAX_CONTROL_PAYLOAD + 1> payload{};
  std::array<uint8_t, FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD> bytes{};
  FrameView frame{Command::TransferChunk, false, 7, payload};
  EXPECT_EQ(encodeFrame(frame, bytes), 0U);
  frame.payload = std::span(payload).first(MAX_CONTROL_PAYLOAD);
  EXPECT_EQ(encodeFrame(frame, std::span(bytes).first(bytes.size() - 1)), 0U);
  ASSERT_EQ(encodeFrame(frame, bytes), bytes.size());
  FrameView decoded;
  EXPECT_EQ(decodeFrame(bytes, true, decoded), FrameError::None);
  EXPECT_EQ(decoded.payload.size(), MAX_CONTROL_PAYLOAD);
}

TEST(CompanionFrame, AssemblesEveryFragmentBoundaryAndRequiresExplicitReset) {
  constexpr std::array<uint8_t, 15> bytes = {'L', 'C', 1, 4, 0, 0, 1, 0, 0, 0, 3, 0, 9, 8, 7};
  std::array<uint8_t, 64> storage{};
  FrameAssembler assembler(storage);
  FrameView output;
  for (size_t split = 0; split < bytes.size(); ++split) {
    assembler.reset();
    EXPECT_EQ(assembler.append(std::span(bytes).first(split), true, output), FrameAssembler::Result::NeedMore);
    ASSERT_EQ(assembler.append(std::span(bytes).subspan(split), true, output), FrameAssembler::Result::Ready);
    EXPECT_EQ(output.payload.size(), 3U);
    EXPECT_EQ(assembler.append(bytes, true, output), FrameAssembler::Result::Rejected);
  }
}

TEST(CompanionFrame, RejectsOversizedDeclaredBodyBeforeReceivingIt) {
  std::array<uint8_t, 32> storage{};
  FrameAssembler assembler(storage);
  FrameView output;
  constexpr std::array<uint8_t, 12> bytes = {'L', 'C', 1, 4, 0, 0, 1, 0, 0, 0, 1, 4};
  EXPECT_EQ(assembler.append(bytes, true, output), FrameAssembler::Result::Rejected);
  EXPECT_EQ(assembler.lastError(), FrameError::Length);
  assembler.reset();
  EXPECT_EQ(assembler.append(std::span(bytes).first(5), false, output), FrameAssembler::Result::NeedMore);
  EXPECT_EQ(assembler.append(std::span(bytes).subspan(5), false, output), FrameAssembler::Result::Rejected);
  EXPECT_EQ(assembler.lastError(), FrameError::Unauthorized);
}

TEST(CompanionFrame, ErrorResponsesCorrelateRequestsAndRequireAuthentication) {
  std::array<uint8_t, 1> reason{1};
  FrameView response{Command::Error, true, 42, reason};
  std::array<uint8_t, FRAME_HEADER_SIZE + 1> bytes{};
  ASSERT_EQ(encodeFrame(response, bytes), bytes.size());
  FrameView decoded;
  EXPECT_EQ(decodeFrame(bytes, false, decoded), FrameError::Unauthorized);
  ASSERT_EQ(decodeFrame(bytes, true, decoded), FrameError::None);
  EXPECT_EQ(decoded.command, Command::Error);
  EXPECT_TRUE(decoded.response);
  EXPECT_EQ(decoded.requestId, 42U);
  EXPECT_EQ(decoded.payload[0], 1U);
}
