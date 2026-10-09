#include <gtest/gtest.h>

#include <array>

#include "lib/Companion/CompanionCourseBinding.h"
#include "lib/Companion/CompanionTransferHandler.h"
#include "lib/EpdFont/VectorFontSupport.h"
using namespace companion;
namespace {
class DisabledStorage final : public TransferStorage {
 public:
  int writes = 0;
  bool prepare() override { return true; }
  FileStatus stat(const char*, uint64_t&) override { return FileStatus::Missing; }
  bool read(const char*, uint64_t, std::span<uint8_t>) override { return false; }
  bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override {
    ++writes;
    return true;
  }
  bool resize(const char*, uint64_t) override { return false; }
  bool rename(const char*, const char*) override { return false; }
  bool remove(const char*) override { return false; }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return false; }
};
}  // namespace
TEST(CompanionTransferDisabled, CourseDeclarationsAreRejectedBeforeStorageMutation) {
  DisabledStorage storage;
  std::array<uint8_t, TRANSFER_JOURNAL_SIZE> scratch{};
  Identity generation{};
  generation[0] = 1;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  TransferDeclaration declaration;
  declaration.state.storageGeneration = generation;
  declaration.state.transaction[0] = 2;
  declaration.state.owner[0] = 3;
  declaration.manifest.kind = ContentKind::Course;
  declaration.manifest.length = declaration.state.length = 9;
  declaration.manifest.formatVersion = 1;
  declaration.manifest.logicalIdentity[0] = 4;
  std::array<uint8_t, 300> body{};
  ASSERT_EQ(encodeTransferDeclaration(declaration, body), TRANSFER_DECLARATION_SIZE);
  constexpr std::string_view path = ACTIVE_COURSE_PATH;
  body[TRANSFER_DECLARATION_SIZE] = path.size();
  std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
  std::array<uint8_t, 100> response{};
  EXPECT_EQ(handleTransfer(transfer, Command::BeginTransfer,
                           std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size()), declaration.state.owner,
                           response),
            1);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Invalid));
  EXPECT_EQ(storage.writes, 0);
  EXPECT_EQ(transfer.current(), nullptr);
}
TEST(CompanionTransferDisabled, DeclaredEPUBStillUsesCanonicalDestination) {
  DisabledStorage storage;
  std::array<uint8_t, TRANSFER_JOURNAL_SIZE> scratch{};
  Identity generation{};
  generation[0] = 1;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  TransferDeclaration declaration;
  declaration.state.storageGeneration = generation;
  declaration.state.transaction[0] = 2;
  declaration.state.owner[0] = 3;
  declaration.manifest.kind = ContentKind::Epub;
  declaration.manifest.length = declaration.state.length = 9;
  constexpr std::string_view path =
      "/Books/Companion/0000000000000000000000000000000000000000000000000000000000000000.epub";
  std::array<uint8_t, 300> body{};
  ASSERT_EQ(encodeTransferDeclaration(declaration, body), TRANSFER_DECLARATION_SIZE);
  body[TRANSFER_DECLARATION_SIZE] = path.size();
  std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
  std::array<uint8_t, 100> response{};
  EXPECT_EQ(handleTransfer(transfer, Command::BeginTransfer,
                           std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size()), declaration.state.owner,
                           response),
            100);
  EXPECT_EQ(response[0], static_cast<uint8_t>(TransferResult::Ok));
  EXPECT_EQ(storage.writes, 2);
  ASSERT_NE(transfer.contentManifest(), nullptr);
  EXPECT_EQ(*transfer.contentManifest(), declaration.manifest);
}

TEST(CompanionTransferDisabled, VectorFontBeginFollowsBoardCapability) {
  DisabledStorage storage;
  std::array<uint8_t, TRANSFER_JOURNAL_SIZE> scratch{};
  Identity generation{};
  generation[0] = 1;
  Transfer transfer(storage, scratch);
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  TransferDeclaration declaration;
  declaration.state.storageGeneration = generation;
  declaration.state.transaction[0] = 2;
  declaration.state.owner[0] = 3;
  declaration.state.contentHash[0] = 4;
  declaration.state.length = 100;
  declaration.manifest = {declaration.state.contentHash, ContentKind::Font, 100, 1, {}};
  std::array<uint8_t, 300> body{};
  std::array<uint8_t, 100> response{};
  auto begin = [&](std::string_view path) {
    EXPECT_EQ(encodeTransferDeclaration(declaration, body), TRANSFER_DECLARATION_SIZE);
    body[TRANSFER_DECLARATION_SIZE] = path.size();
    std::copy(path.begin(), path.end(), body.begin() + TRANSFER_DECLARATION_SIZE + 1);
    EXPECT_GT(handleTransfer(transfer, Command::BeginTransfer,
                             std::span(body).first(TRANSFER_DECLARATION_SIZE + 1 + path.size()),
                             declaration.state.owner, response),
              0);
    return static_cast<TransferResult>(response[0]);
  };
  EXPECT_EQ(begin("/Books/Fixture.ttf"), TransferResult::Invalid);
  EXPECT_EQ(begin("/fonts/Fixture/extra/Fixture.ttf"), TransferResult::Invalid);
  EXPECT_EQ(storage.writes, 0);
  EXPECT_EQ(transfer.current(), nullptr);
#if CROSSPOINT_VECTOR_FONTS
  EXPECT_EQ(begin("/fonts/Fixture.TTF"), TransferResult::Ok);
  EXPECT_EQ(storage.writes, 2);
  ASSERT_NE(transfer.contentManifest(), nullptr);
  EXPECT_EQ(*transfer.contentManifest(), declaration.manifest);
#else
  EXPECT_EQ(begin("/fonts/Fixture.TTF"), TransferResult::Invalid);
  EXPECT_EQ(storage.writes, 0);
  EXPECT_EQ(transfer.current(), nullptr);
#endif
}
