#include <gtest/gtest.h>

#include "../../lib/hal/HalInventoryDirectoryWalker.h"
using Walker = HalInventoryDirectoryWalker;
using Result = Walker::Result;
class DirectoryWalkerTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_test::state = {};
    directory_test::state.directories["/"] = {};
  }
};
TEST_F(DirectoryWalkerTest, DepthFirstAndReusesPreparedHandles) {
  auto& tree = directory_test::state.directories;
  tree["/"] = {{"a", true}, {"b", true}, {"book.epub", false}};
  tree["/a"] = {{"course.pack", false}};
  tree["/b"] = {{"other.epub", false}};
  Walker walker;
  ASSERT_TRUE(walker.begin());
  for (const char* path : {"/a", "/a/course.pack", "/b", "/b/other.epub", "/book.epub"}) {
    ASSERT_EQ(walker.next(), Result::Entry);
    EXPECT_STREQ(walker.entryPath(), path);
    EXPECT_EQ(walker.entryFile().path, path);
    EXPECT_TRUE(walker.entryFile().isOpen());
  }
  EXPECT_EQ(walker.next(), Result::End);
  EXPECT_EQ(walker.next(), Result::End);
  EXPECT_EQ(directory_test::state.preparations, 2u);
}
TEST_F(DirectoryWalkerTest, ExplicitPruningAndNormalizedRoot) {
  auto& tree = directory_test::state.directories;
  tree["/books"] = {{"private", true}, {"book.epub", false}};
  Walker walker;
  ASSERT_TRUE(walker.begin("/books///"));
  ASSERT_EQ(walker.next(), Result::Entry);
  walker.skipDirectory();
  ASSERT_EQ(walker.next(), Result::Entry);
  EXPECT_STREQ(walker.entryPath(), "/books/book.epub");
  EXPECT_EQ(walker.next(), Result::End);
}
TEST_F(DirectoryWalkerTest, ReadFailureCannotBecomeEnd) {
  Walker walker;
  ASSERT_TRUE(walker.begin());
  directory_test::state.errorDirectory = "/";
  EXPECT_EQ(walker.next(), Result::Error);
  directory_test::state.errorDirectory.clear();
  EXPECT_EQ(walker.next(), Result::Error);
  ASSERT_TRUE(walker.begin());
  EXPECT_EQ(walker.next(), Result::End);
}
TEST_F(DirectoryWalkerTest, RejectsInvalidAndTruncatedNames) {
  for (const auto& name : {std::string("bad/name"), std::string("bad\\name"), std::string("bad:name"),
                           std::string("bad\nname"), std::string(256, 'a')}) {
    directory_test::state.directories["/"] = {{name, false}};
    Walker walker;
    ASSERT_TRUE(walker.begin());
    EXPECT_EQ(walker.next(), Result::Error);
    EXPECT_EQ(walker.next(), Result::Error);
  }
}
TEST_F(DirectoryWalkerTest, DepthLimitFailsInsteadOfTruncating) {
  std::string path = "/";
  for (size_t i = 0; i <= Walker::MAX_DEPTH; ++i) {
    directory_test::state.directories[path] = {{"d", true}};
    path += (path == "/" ? "" : "/");
    path += "d";
  }
  Walker walker;
  ASSERT_TRUE(walker.begin());
  for (size_t i = 0; i < Walker::MAX_DEPTH; ++i) ASSERT_EQ(walker.next(), Result::Entry);
  EXPECT_EQ(walker.next(), Result::Error);
}
TEST_F(DirectoryWalkerTest, PathLimitAndMissingRootFail) {
  Walker walker;
  EXPECT_FALSE(walker.begin("relative"));
  EXPECT_FALSE(walker.begin("/missing"));
  std::array<char, Walker::PATH_CAPACITY> unterminated;
  unterminated.fill('a');
  unterminated[0] = '/';
  EXPECT_FALSE(walker.begin(unterminated.data()));
  EXPECT_EQ(walker.next(), Result::Error);
  const std::string maximum = "/" + std::string(Walker::PATH_CAPACITY - 2, 'a');
  directory_test::state.directories[maximum] = {};
  ASSERT_TRUE(walker.begin(maximum.c_str()));
  EXPECT_EQ(walker.next(), Result::End);
  const std::string root = "/" + std::string(500, 'a');
  directory_test::state.directories[root] = {{"long-file-name", false}};
  ASSERT_TRUE(walker.begin(root.c_str()));
  EXPECT_EQ(walker.next(), Result::Error);
}
TEST_F(DirectoryWalkerTest, CloseAndAllocationAndNameFailuresAreErrors) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    directory_test::state = {};
    directory_test::state.directories["/"] = {{"book", false}};
    Walker walker;
    ASSERT_TRUE(walker.begin());
    if (failure == 0) directory_test::state.failPrepare = true;
    if (failure == 1) directory_test::state.failName = true;
    if (failure == 2) {
      ASSERT_EQ(walker.next(), Result::Entry);
      directory_test::state.failClose = true;
    }
    EXPECT_EQ(walker.next(), Result::Error);
    EXPECT_GT(directory_test::state.errors, 0u);
  }
}
