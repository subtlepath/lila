#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "lib/Companion/CompanionCourseContext.h"

using namespace companion;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
Identity generation() {
  Identity result;
  result.fill(0x11);
  return result;
}
}  // namespace
TEST(CompanionCourseContext, SharedRequestAndRemovedReplyFixtures) {
  const auto request = fixture("ReaderCourseContextRequest-v1.fixture");
  const auto response = fixture("ReaderCourseContextRemoved-v1.fixture");
  ASSERT_EQ(request.size(), COURSE_CONTEXT_REQUEST_SIZE);
  ASSERT_EQ(response.size(), COURSE_CONTEXT_REPLY_SIZE);
  Identity decoded{};
  ASSERT_TRUE(decodeCourseContextRequest(request, decoded));
  EXPECT_EQ(decoded, generation());
  CourseContextReply context;
  ASSERT_TRUE(decodeCourseContextReply(response, decoded, context));
  EXPECT_EQ(context.source, CourseContextSource::Removed);
  EXPECT_EQ(context.manifest.kind, ContentKind::Course);
  EXPECT_EQ(context.manifest.length, 4097u);
  EXPECT_EQ(context.manifest.formatVersion, 1u);
  EXPECT_EQ(context.manifest.contentHash[0], 0x22);
  EXPECT_EQ(context.manifest.logicalIdentity[0], 0x33);
  std::array<uint8_t, COURSE_CONTEXT_REPLY_SIZE> encoded{};
  EXPECT_EQ(encodeCourseContextRequest(decoded, encoded), request.size());
  EXPECT_TRUE(std::equal(request.begin(), request.end(), encoded.begin()));
  EXPECT_EQ(encodeCourseContextReply(context, encoded), response.size());
  EXPECT_TRUE(std::equal(response.begin(), response.end(), encoded.begin()));
  context.source = CourseContextSource::Live;
  ASSERT_EQ(encodeCourseContextReply(context, encoded), response.size());
  CourseContextReply live;
  ASSERT_TRUE(decodeCourseContextReply(encoded, decoded, live));
  EXPECT_EQ(live, context);
}
TEST(CompanionCourseContext, TruncationAndMalformedContextPreserveOutputs) {
  const auto response = fixture("ReaderCourseContextRemoved-v1.fixture");
  const auto request = fixture("ReaderCourseContextRequest-v1.fixture");
  CourseContextReply saved;
  saved.generation.fill(7);
  for (size_t size = 0; size < response.size(); ++size) {
    auto output = saved;
    EXPECT_FALSE(decodeCourseContextReply(std::span(response).first(size), generation(), output));
    EXPECT_EQ(output, saved);
  }
  auto extended = response;
  extended.push_back(0);
  auto output = saved;
  EXPECT_FALSE(decodeCourseContextReply(extended, generation(), output));
  EXPECT_EQ(output, saved);
  for (const auto& [offset, value] :
       {std::pair<size_t, uint8_t>{0, 0}, {3, 2}, {4, 255}, {5, 0}, {5, 3}, {6, 0}, {22, 2}, {56, 1}, {65, 2}}) {
    auto malformed = response;
    malformed[offset] = value;
    EXPECT_FALSE(decodeCourseContextReply(malformed, generation(), output));
    EXPECT_EQ(output, saved);
  }
  for (const auto& [offset, count] : {std::pair<size_t, size_t>{24, 32}, {57, 8}, {69, 16}}) {
    auto malformed = response;
    std::fill_n(malformed.begin() + offset, count, 0);
    EXPECT_FALSE(decodeCourseContextReply(malformed, generation(), output));
    EXPECT_EQ(output, saved);
  }
  Identity sentinel{};
  sentinel.fill(7);
  for (size_t size = 0; size < request.size(); ++size) {
    auto identity = sentinel;
    EXPECT_FALSE(decodeCourseContextRequest(std::span(request).first(size), identity));
    EXPECT_EQ(identity, sentinel);
  }
  auto zero = request;
  std::fill(zero.begin() + 4, zero.end(), 0);
  auto identity = sentinel;
  EXPECT_FALSE(decodeCourseContextRequest(zero, identity));
  EXPECT_EQ(identity, sentinel);
}
TEST(CompanionCourseContext, FailureRepliesCarryNoCourseAndWrongStorageReportsNewGeneration) {
  for (uint8_t result = 1; result <= static_cast<uint8_t>(CourseContextResult::Corrupt); ++result) {
    CourseContextReply reply;
    reply.result = static_cast<CourseContextResult>(result);
    reply.generation = generation();
    if (reply.result == CourseContextResult::WrongStorage) reply.generation[0] ^= 1;
    std::array<uint8_t, COURSE_CONTEXT_REPLY_SIZE> bytes{};
    ASSERT_EQ(encodeCourseContextReply(reply, bytes), COURSE_CONTEXT_HEADER_SIZE);
    CourseContextReply decoded;
    ASSERT_TRUE(decodeCourseContextReply(std::span(bytes).first(COURSE_CONTEXT_HEADER_SIZE), generation(), decoded));
    EXPECT_EQ(decoded.result, reply.result);
    EXPECT_EQ(decoded.source, CourseContextSource::None);
    EXPECT_EQ(decoded.manifest, ContentManifest{});
    EXPECT_FALSE(decodeCourseContextReply(bytes, generation(), decoded));
    bytes[5] = 2;
    EXPECT_FALSE(decodeCourseContextReply(std::span(bytes).first(COURSE_CONTEXT_HEADER_SIZE), generation(), decoded));
  }
}
TEST(CompanionCourseContext, InvalidEncodingDoesNotTouchDestination) {
  std::array<uint8_t, COURSE_CONTEXT_REPLY_SIZE> bytes;
  bytes.fill(7);
  const auto saved = bytes;
  EXPECT_EQ(encodeCourseContextRequest(Identity{}, bytes), 0u);
  EXPECT_EQ(bytes, saved);
  CourseContextReply context;
  context.result = CourseContextResult::Ok;
  context.generation = generation();
  EXPECT_EQ(encodeCourseContextReply(context, bytes), 0u);
  EXPECT_EQ(bytes, saved);
  context.result = CourseContextResult::Missing;
  context.source = CourseContextSource::Removed;
  EXPECT_EQ(encodeCourseContextReply(context, bytes), 0u);
  EXPECT_EQ(bytes, saved);
}
