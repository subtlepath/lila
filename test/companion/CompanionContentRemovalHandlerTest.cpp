#include <gtest/gtest.h>

#include "lib/Companion/CompanionContentRemovalHandler.h"
using namespace companion;
namespace {
class Backend final : public ContentRemovalBackend {
 public:
  ContentRemovalResult lookupResult = ContentRemovalResult::NotFound;
  ContentRemovalResult removeResult = ContentRemovalResult::Ok;
  unsigned lookups = 0, removals = 0;
  ContentRemovalRequest captured;
  ContentRemovalResult completed(const ContentRemovalRequest& request) override {
    ++lookups;
    captured = request;
    return lookupResult;
  }
  ContentRemovalResult remove(const ContentRemovalRequest& request) override {
    ++removals;
    captured = request;
    return removeResult;
  }
};
class RemovalHandlerTest : public testing::Test {
 protected:
  Backend backend;
  ContentRemovalHandler handler{backend};
  ContentRemovalRequest request;
  std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> body{};
  std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
  void SetUp() override {
    request.transaction.fill(1);
    request.owner.fill(2);
    request.generation.fill(3);
    request.manifest.kind = ContentKind::Epub;
    request.manifest.formatVersion = 1;
    request.manifest.length = 123;
    request.manifest.contentHash.fill(4);
    ASSERT_EQ(encodeContentRemovalRequest(request, body), body.size());
  }
  ContentRemovalResult run() {
    EXPECT_EQ(handler.handle(true, request.owner, request.generation, body, reply), reply.size());
    EXPECT_TRUE(std::equal(request.transaction.begin(), request.transaction.end(), reply.begin() + 1));
    return static_cast<ContentRemovalResult>(reply[0]);
  }
};
}  // namespace
TEST_F(RemovalHandlerTest, AuthorizationAndCardBindingPrecedeAllBackendCalls) {
  ASSERT_EQ(handler.handle(false, request.owner, request.generation, body, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
  EXPECT_TRUE(std::all_of(reply.begin() + 1, reply.end(), [](auto byte) { return byte == 0; }));
  Identity foreign{};
  foreign.fill(99);
  ASSERT_EQ(handler.handle(true, foreign, request.generation, body, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
  ASSERT_EQ(handler.handle(true, request.owner, foreign, body, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::WrongStorage));
  EXPECT_EQ(backend.lookups, 0U);
  EXPECT_EQ(backend.removals, 0U);
}
TEST_F(RemovalHandlerTest, MalformedRequestsAndShortRepliesCannotStartWork) {
  reply.fill(99);
  EXPECT_EQ(handler.handle(true, request.owner, request.generation, body, std::span(reply).first(16)), 0U);
  EXPECT_EQ(reply[0], 99);
  for (size_t length = 0; length < body.size(); ++length) {
    ASSERT_EQ(handler.handle(true, request.owner, request.generation, std::span(body).first(length), reply),
              reply.size());
    EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Invalid));
  }
  body[3] = 2;
  ASSERT_EQ(handler.handle(true, request.owner, request.generation, body, reply), reply.size());
  EXPECT_EQ(reply[0], static_cast<uint8_t>(ContentRemovalResult::Invalid));
  EXPECT_EQ(backend.lookups, 0U);
  EXPECT_EQ(backend.removals, 0U);
}
TEST_F(RemovalHandlerTest, CompletedRequestSkipsAdmissionAndParticipantWork) {
  backend.lookupResult = ContentRemovalResult::Ok;
  backend.removeResult = ContentRemovalResult::Busy;
  EXPECT_EQ(run(), ContentRemovalResult::Ok);
  EXPECT_EQ(run(), ContentRemovalResult::Ok);
  EXPECT_EQ(backend.lookups, 2U);
  EXPECT_EQ(backend.removals, 0U);
  EXPECT_EQ(backend.captured, request);
}
TEST_F(RemovalHandlerTest, OnlyCheckedReceiptAbsenceAllowsRemoval) {
  for (auto result : {ContentRemovalResult::Conflict, ContentRemovalResult::Corrupt, ContentRemovalResult::IoError}) {
    backend.lookupResult = result;
    EXPECT_EQ(run(), result);
  }
  EXPECT_EQ(backend.removals, 0U);
  backend.lookupResult = ContentRemovalResult::NotFound;
  backend.removeResult = ContentRemovalResult::Busy;
  EXPECT_EQ(run(), ContentRemovalResult::Busy);
  EXPECT_EQ(backend.removals, 1U);
  backend.removeResult = ContentRemovalResult::Unsupported;
  EXPECT_EQ(run(), ContentRemovalResult::Unsupported);
  EXPECT_EQ(backend.removals, 2U);
}

TEST_F(RemovalHandlerTest, FontFormatsMatchInstalledVectorAndBitmapManifests) {
  request.manifest.kind = ContentKind::Font;
  for (const auto format : {0U, 1U, 2U, 3U, 4U, 5U}) {
    request.manifest.formatVersion = format;
    const bool accepted = format == 1 || format == 4;
    EXPECT_EQ(validContentRemovalRequest(request), accepted);
    EXPECT_EQ(encodeContentRemovalRequest(request, body), accepted ? body.size() : 0U);
    if (accepted) {
      ContentRemovalRequest decoded;
      ASSERT_TRUE(decodeContentRemovalRequest(body, decoded));
      EXPECT_EQ(decoded, request);
    }
  }
}
