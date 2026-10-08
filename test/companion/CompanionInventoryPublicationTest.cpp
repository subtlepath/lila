#include <gtest/gtest.h>

#include <map>

#include "../../lib/Companion/CompanionInventoryPairValidation.h"
#include "../../lib/Companion/CompanionInventoryPublication.h"
#include "../../lib/Companion/CompanionInventoryRecovery.h"
#include "../../lib/Companion/CompanionInventoryRollback.h"
using namespace companion;
namespace {
struct Storage : TransferStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned mutation = 0, failAt = 0;
  bool after = false, failRead = false, torn = false;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    const auto it = files.find(path);
    if (it == files.end()) return FileStatus::Missing;
    size = it->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    const auto it = files.find(path);
    if (failRead || it == files.end() || offset > it->second.size() || bytes.size() > it->second.size() - offset)
      return false;
    std::copy_n(it->second.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
    const bool fail = ++mutation == failAt;
    if (fail && !after && !torn) return false;
    auto& output = files[path];
    if (truncate) output.clear();
    output.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), output.begin() + offset);
    if (fail && torn) output.resize(3);
    return !fail;
  }
  bool resize(const char*, uint64_t) override { return false; }
  bool rename(const char* from, const char* to) override {
    const bool fail = ++mutation == failAt;
    if (fail && !after) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files.at(from));
    files.erase(from);
    return !fail;
  }
  bool remove(const char* path) override {
    const bool fail = ++mutation == failAt;
    if (fail && !after) return false;
    files.erase(path);
    return !fail;
  }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return false; }
};
struct Validator : InventoryPublicationValidator {
  Storage& storage;
  explicit Validator(Storage& storage) : storage(storage) {}
  InventoryValidation file(const char* path, bool paths, const Identity& generation, uint64_t revision) override {
    if (storage.failRead) return InventoryValidation::IoError;
    const auto it = storage.files.find(path);
    if (it == storage.files.end() || it->second.size() != 3 || it->second[0] != generation[0] ||
        it->second[2] != (paths ? 2 : 1) || (revision && it->second[1] != revision))
      return InventoryValidation::Invalid;
    return InventoryValidation::Valid;
  }
  InventoryValidation pair(const char* index, const char* paths, const Identity& generation, uint64_t revision,
                           uint64_t* actual = nullptr) override {
    const auto left = file(index, false, generation, revision), right = file(paths, true, generation, revision);
    if (left == InventoryValidation::IoError || right == InventoryValidation::IoError)
      return InventoryValidation::IoError;
    if (left != InventoryValidation::Valid || right != InventoryValidation::Valid ||
        storage.files.at(index)[1] != storage.files.at(paths)[1])
      return InventoryValidation::Invalid;
    if (actual) *actual = storage.files.at(index)[1];
    return InventoryValidation::Valid;
  }
};
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
Storage prepared(bool old = true) {
  Storage result;
  if (old) {
    result.files[InventoryPublication::INDEX] = {1, 1, 1};
    result.files[InventoryPublication::PATHS] = {1, 1, 2};
  }
  result.files[InventoryPublication::INDEX_NEXT] = {1, 2, 1};
  result.files[InventoryPublication::PATHS_NEXT] = {1, 2, 2};
  return result;
}
}  // namespace
TEST(CompanionInventoryPublication, PublishesPairAndRepeatedCommitIsHarmless) {
  auto storage = prepared();
  Validator validator(storage);
  std::array<uint8_t, 64> scratch;
  InventoryPublication publisher(storage, validator, scratch);
  ASSERT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::Ok);
  ASSERT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 2),
            InventoryValidation::Valid);
  const auto mutations = storage.mutation;
  EXPECT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::Ok);
  EXPECT_EQ(storage.mutation, mutations);
  EXPECT_EQ(publisher.publish(generation(), 1), InventoryPublicationResult::Invalid);
  EXPECT_EQ(storage.files.size(), 2u);
}
TEST(CompanionInventoryPublication, EveryInterruptedMutationRecoversCoherentPair) {
  for (bool old : {false, true}) {
    auto success = prepared(old);
    Validator successValidator(success);
    std::array<uint8_t, 64> scratch;
    InventoryPublication successful(success, successValidator, scratch);
    ASSERT_EQ(successful.publish(generation(), 2), InventoryPublicationResult::Ok);
    for (bool after : {false, true})
      for (unsigned failure = 1; failure <= success.mutation; ++failure) {
        SCOPED_TRACE(testing::Message() << old << " " << after << " " << failure);
        auto storage = prepared(old);
        storage.failAt = failure;
        storage.after = after;
        Validator validator(storage);
        InventoryPublication first(storage, validator, scratch);
        EXPECT_NE(first.publish(generation(), 2), InventoryPublicationResult::Ok);
        storage.failAt = 0;
        InventoryPublication restarted(storage, validator, scratch);
        ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
        if (storage.files.contains(InventoryPublication::INDEX)) {
          EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 0),
                    InventoryValidation::Valid);
        } else
          EXPECT_FALSE(storage.files.contains(InventoryPublication::PATHS));
        ASSERT_EQ(restarted.publish(generation(), 2), InventoryPublicationResult::Ok);
        EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 2),
                  InventoryValidation::Valid);
      }
  }
}
TEST(CompanionInventoryPublication, TornIntentBeforeMutationPreservesOldPairAndCanRetry) {
  for (unsigned slot = 1; slot <= 2; ++slot) {
    auto storage = prepared();
    storage.failAt = slot;
    storage.torn = true;
    Validator validator(storage);
    std::array<uint8_t, 64> scratch;
    InventoryPublication publisher(storage, validator, scratch);
    EXPECT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::IoError);
    storage.failAt = 0;
    ASSERT_EQ(publisher.recover(generation()), InventoryPublicationResult::Ok);
    ASSERT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::Ok);
  }
}
TEST(CompanionInventoryPublication, MismatchedCandidatesAndReadFailuresNeverMutateSnapshots) {
  auto storage = prepared();
  Validator validator(storage);
  std::array<uint8_t, 64> scratch;
  InventoryPublication publisher(storage, validator, scratch);
  storage.files[InventoryPublication::PATHS_NEXT][1] = 3;
  EXPECT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::Corrupt);
  EXPECT_EQ(storage.mutation, 0u);
  storage.failRead = true;
  EXPECT_EQ(publisher.recover(generation()), InventoryPublicationResult::IoError);
  EXPECT_EQ(storage.mutation, 0u);
}

TEST(CompanionInventoryPublication, WrongGenerationAndConflictingValidIntentsPreserveFiles) {
  auto storage = prepared();
  storage.failAt = 3;
  Validator validator(storage);
  std::array<uint8_t, 64> scratch;
  InventoryPublication publisher(storage, validator, scratch);
  ASSERT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::IoError);
  storage.failAt = 0;
  const auto previous = storage.files;
  auto wrong = generation();
  wrong[0] = 9;
  EXPECT_EQ(publisher.recover(wrong), InventoryPublicationResult::WrongStorage);
  EXPECT_EQ(storage.files, previous);
  auto& second = storage.files.at(InventoryPublication::INTENTS[1]);
  inventory_detail::write(second, 20, 3, 8);
  inventory_detail::write(second, 28, inventoryIndexCrc(std::span(second).first(28)), 4);
  const auto conflicting = storage.files;
  EXPECT_EQ(publisher.recover(generation()), InventoryPublicationResult::Corrupt);
  EXPECT_EQ(storage.files, conflicting);
}

namespace {
struct View : InventoryIndexStorage {
  Storage& storage;
  const char* path = nullptr;
  explicit View(Storage& storage) : storage(storage) {}
  bool size(uint64_t& bytes) override { return path && storage.stat(path, bytes) == FileStatus::Present; }
  bool read(uint64_t offset, std::span<uint8_t> bytes) override { return path && storage.read(path, offset, bytes); }
};
struct RealValidator : InventoryPublicationValidator {
  Storage& storage;
  View index, paths;
  std::span<uint8_t> scratch;
  InventoryPairValidation validation;
  RealValidator(Storage& storage, std::span<uint8_t> scratch)
      : storage(storage), index(storage), paths(storage), scratch(scratch), validation(index, paths, scratch) {}
  InventoryValidation file(const char* path, bool isPaths, const Identity& generation, uint64_t revision) override {
    if (storage.failRead) return InventoryValidation::IoError;
    index.path = path;
    if (!isPaths) {
      IndexedInventoryCatalog catalog(index, scratch);
      return catalog.open(generation) && (revision == 0 || catalog.revision() == revision)
                 ? InventoryValidation::Valid
                 : InventoryValidation::Invalid;
    }
    InventoryIndexHeader header;
    if (!index.read(0, scratch.first(INVENTORY_INDEX_HEADER_SIZE)) ||
        !decodeInventoryPathsHeader(scratch.first(INVENTORY_INDEX_HEADER_SIZE), header))
      return InventoryValidation::Invalid;
    InventoryPaths map(index, scratch);
    return map.open(generation, revision == 0 ? header.revision : revision) ? InventoryValidation::Valid
                                                                            : InventoryValidation::Invalid;
  }
  InventoryValidation pair(const char* indexPath, const char* pathsPath, const Identity& generation, uint64_t revision,
                           uint64_t* actual = nullptr) override {
    if (storage.failRead) return InventoryValidation::IoError;
    index.path = indexPath;
    paths.path = pathsPath;
    if (!validation.validate(generation, revision)) return InventoryValidation::Invalid;
    if (actual) *actual = validation.revision();
    return InventoryValidation::Valid;
  }
};
ContentManifest realItem(unsigned value) {
  ContentManifest manifest;
  manifest.contentHash[0] = value;
  manifest.length = 10;
  return manifest;
}
std::vector<uint8_t> realSnapshot(uint64_t revision, bool paths, bool mismatch = false) {
  std::vector<uint8_t> bytes(INVENTORY_INDEX_HEADER_SIZE + 2 * INVENTORY_PATH_MAX_RECORD);
  size_t offset = INVENTORY_INDEX_HEADER_SIZE;
  for (unsigned value = 1; value <= 2; ++value) {
    auto manifest = realItem(value);
    if (mismatch && value == 2) ++manifest.length;
    offset += paths ? encodeInventoryPath(manifest, "/books/book.epub", std::span(bytes).subspan(offset))
                    : encodeInventoryIndexEntry(manifest, std::span(bytes).subspan(offset));
  }
  bytes.resize(offset);
  InventoryIndexHeader header{generation(), revision, 2,
                              inventoryIndexCrc(std::span(bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  if (paths)
    encodeInventoryPathsHeader(header, bytes);
  else
    encodeInventoryIndexHeader(header, bytes);
  return bytes;
}
Storage realPrepared() {
  Storage storage;
  storage.files[InventoryPublication::INDEX] = realSnapshot(1, false);
  storage.files[InventoryPublication::PATHS] = realSnapshot(1, true);
  storage.files[InventoryPublication::INDEX_NEXT] = realSnapshot(2, false);
  storage.files[InventoryPublication::PATHS_NEXT] = realSnapshot(2, true);
  return storage;
}
}  // namespace
TEST(CompanionInventoryPublication, RealFormatsRecoverEveryBeforeAndAfterMutationFailure) {
  std::array<uint8_t, 8192> workspace;
  auto success = realPrepared();
  RealValidator successValidator(success, workspace);
  InventoryPublication complete(success, successValidator, workspace);
  ASSERT_EQ(complete.publish(generation(), 2), InventoryPublicationResult::Ok);
  for (bool after : {false, true})
    for (unsigned failure = 1; failure <= success.mutation; ++failure) {
      auto storage = realPrepared();
      storage.failAt = failure;
      storage.after = after;
      RealValidator validator(storage, workspace);
      InventoryPublication publisher(storage, validator, workspace);
      EXPECT_NE(publisher.publish(generation(), 2), InventoryPublicationResult::Ok);
      storage.failAt = 0;
      InventoryPublication restarted(storage, validator, workspace);
      ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
      ASSERT_EQ(restarted.publish(generation(), 2), InventoryPublicationResult::Ok);
      EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 2),
                InventoryValidation::Valid);
    }
}
TEST(CompanionInventoryPublication, RealFormatCorrespondenceRejectsValidCrcMismatchBeforeMutation) {
  auto storage = realPrepared();
  storage.files[InventoryPublication::PATHS_NEXT] = realSnapshot(2, true, true);
  std::array<uint8_t, 8192> workspace;
  RealValidator validator(storage, workspace);
  InventoryPublication publisher(storage, validator, workspace);
  EXPECT_EQ(publisher.publish(generation(), 2), InventoryPublicationResult::Corrupt);
  EXPECT_EQ(storage.mutation, 0u);
}

namespace {
Storage mixedSnapshots(std::span<uint8_t> scratch, bool mapInBackup) {
  auto storage = realPrepared();
  RealValidator validator(storage, scratch);
  InventoryPublication publication(storage, validator, scratch);
  storage.failAt = mapInBackup ? 6 : 5;
  EXPECT_EQ(publication.publish(generation(), 2), InventoryPublicationResult::IoError);
  storage.files.at(InventoryPublication::PATHS_NEXT).back() ^= 1;
  storage.failAt = 0;
  storage.mutation = 0;
  return storage;
}
}  // namespace
TEST(CompanionInventoryPublication, RollbackRecoversEveryBeforeAndAfterMutationFailure) {
  std::array<uint8_t, 8192> scratch;
  for (bool mapInBackup : {false, true}) {
    auto success = mixedSnapshots(scratch, mapInBackup);
    RealValidator successValidator(success, scratch);
    InventoryRollback complete(success, successValidator, scratch);
    ASSERT_EQ(complete.begin(generation(), 2), InventoryPublicationResult::Ok);
    for (bool after : {false, true})
      for (unsigned failure = 1; failure <= success.mutation; ++failure) {
        SCOPED_TRACE(testing::Message() << mapInBackup << " " << after << " " << failure);
        auto storage = mixedSnapshots(scratch, mapInBackup);
        storage.failAt = failure;
        storage.after = after;
        RealValidator validator(storage, scratch);
        InventoryRollback first(storage, validator, scratch);
        EXPECT_NE(first.begin(generation(), 2), InventoryPublicationResult::Ok);
        storage.failAt = 0;
        InventoryRollback restarted(storage, validator, scratch);
        bool pending = false;
        ASSERT_TRUE(restarted.pending(pending));
        if (pending)
          ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
        else if (validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 1) !=
                 InventoryValidation::Valid) {
          ASSERT_EQ(restarted.begin(generation(), 2), InventoryPublicationResult::Ok);
        }
        EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 1),
                  InventoryValidation::Valid);
        ASSERT_TRUE(restarted.pending(pending));
        EXPECT_FALSE(pending);
        EXPECT_FALSE(storage.files.contains(InventoryPublication::INTENTS[0]));
        EXPECT_FALSE(storage.files.contains(InventoryPublication::INTENTS[1]));
      }
  }
}
TEST(CompanionInventoryPublication, TornRollbackWritesNeverDestroyRestorableOldPair) {
  std::array<uint8_t, 8192> scratch;
  for (unsigned failure : {1u, 2u}) {
    auto storage = mixedSnapshots(scratch, true);
    storage.failAt = failure;
    storage.torn = true;
    RealValidator validator(storage, scratch);
    InventoryRollback rollback(storage, validator, scratch);
    EXPECT_EQ(rollback.begin(generation(), 2), InventoryPublicationResult::IoError);
    EXPECT_EQ(validator.pair(InventoryPublication::INDEX_OLD, InventoryPublication::PATHS_OLD, generation(), 1),
              InventoryValidation::Valid);
    storage.failAt = 0;
    if (failure == 1) {
      EXPECT_EQ(rollback.recover(generation()), InventoryPublicationResult::Corrupt);
      ASSERT_EQ(rollback.begin(generation(), 2), InventoryPublicationResult::Ok);
    } else
      ASSERT_EQ(rollback.recover(generation()), InventoryPublicationResult::Ok);
    EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 1),
              InventoryValidation::Valid);
  }
}

TEST(CompanionInventoryPublication, RollbackWrongGenerationAndReadFailurePreserveRecoveryArtifacts) {
  std::array<uint8_t, 8192> scratch;
  auto storage = mixedSnapshots(scratch, true);
  storage.failAt = 3;
  RealValidator validator(storage, scratch);
  InventoryRollback rollback(storage, validator, scratch);
  ASSERT_EQ(rollback.begin(generation(), 2), InventoryPublicationResult::IoError);
  storage.failAt = 0;
  const auto original = storage.files;
  auto wrong = generation();
  wrong[0] = 9;
  EXPECT_EQ(rollback.recover(wrong), InventoryPublicationResult::WrongStorage);
  EXPECT_EQ(storage.files, original);
  storage.failRead = true;
  EXPECT_EQ(rollback.recover(generation()), InventoryPublicationResult::IoError);
  EXPECT_EQ(storage.files, original);
  storage.failRead = false;
  EXPECT_EQ(rollback.recover(generation()), InventoryPublicationResult::Ok);
}

namespace {
Storage initialMixed(std::span<uint8_t> scratch) {
  auto storage = realPrepared();
  storage.files.erase(InventoryPublication::INDEX);
  storage.files.erase(InventoryPublication::PATHS);
  storage.failAt = 4;
  RealValidator validator(storage, scratch);
  InventoryPublication publication(storage, validator, scratch);
  EXPECT_EQ(publication.publish(generation(), 2), InventoryPublicationResult::IoError);
  storage.files.at(InventoryPublication::PATHS_NEXT).back() ^= 1;
  storage.failAt = 0;
  storage.mutation = 0;
  return storage;
}
}  // namespace
TEST(CompanionInventoryPublication, EmptyRollbackRecoversEveryInterruptedMutation) {
  std::array<uint8_t, 8192> scratch;
  auto success = initialMixed(scratch);
  RealValidator successValidator(success, scratch);
  InventoryRollback complete(success, successValidator, scratch);
  ASSERT_EQ(complete.beginEmpty(generation(), 2), InventoryPublicationResult::Ok);
  for (bool after : {false, true})
    for (unsigned failure = 1; failure <= success.mutation; ++failure) {
      auto storage = initialMixed(scratch);
      storage.failAt = failure;
      storage.after = after;
      RealValidator validator(storage, scratch);
      InventoryRollback rollback(storage, validator, scratch);
      EXPECT_NE(rollback.beginEmpty(generation(), 2), InventoryPublicationResult::Ok);
      storage.failAt = 0;
      InventoryRollback restarted(storage, validator, scratch);
      bool pending = false;
      ASSERT_TRUE(restarted.pending(pending));
      if (pending) {
        ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
      } else if (storage.files.contains(InventoryPublication::INTENTS[0])) {
        ASSERT_EQ(restarted.beginEmpty(generation(), 2), InventoryPublicationResult::Ok);
      }
      EXPECT_FALSE(storage.files.contains(InventoryPublication::INDEX));
      EXPECT_FALSE(storage.files.contains(InventoryPublication::PATHS));
      EXPECT_FALSE(storage.files.contains(InventoryPublication::INTENTS[0]));
      EXPECT_FALSE(storage.files.contains(InventoryPublication::PATHS_NEXT));
      ASSERT_TRUE(restarted.pending(pending));
      EXPECT_FALSE(pending);
    }
}
TEST(CompanionInventoryPublication, EmptyRollbackRejectsBackupsWrongIntentAndUnrelatedActiveFiles) {
  std::array<uint8_t, 8192> scratch;
  for (unsigned failure = 0; failure < 4; ++failure) {
    auto storage = initialMixed(scratch);
    if (failure == 0) storage.files[InventoryPublication::INDEX_OLD] = realSnapshot(1, false);
    if (failure == 1) {
      storage.files.erase(InventoryPublication::INTENTS[0]);
      storage.files.erase(InventoryPublication::INTENTS[1]);
    }
    if (failure == 2) storage.files[InventoryPublication::INDEX] = realSnapshot(1, false);
    RealValidator validator(storage, scratch);
    InventoryRollback rollback(storage, validator, scratch);
    const auto original = storage.files;
    EXPECT_EQ(rollback.beginEmpty(generation(), failure == 3 ? 3 : 2), InventoryPublicationResult::Corrupt);
    EXPECT_EQ(storage.files, original);
    EXPECT_EQ(storage.mutation, 0u);
  }
}
TEST(CompanionInventoryPublication, EmptyRollbackTornCopiesPreserveResumableState) {
  std::array<uint8_t, 8192> scratch;
  for (unsigned failure : {1u, 2u}) {
    auto storage = initialMixed(scratch);
    storage.failAt = failure;
    storage.torn = true;
    RealValidator validator(storage, scratch);
    InventoryRollback rollback(storage, validator, scratch);
    EXPECT_EQ(rollback.beginEmpty(generation(), 2), InventoryPublicationResult::IoError);
    EXPECT_TRUE(storage.files.contains(InventoryPublication::INDEX));
    storage.failAt = 0;
    if (failure == 1) {
      EXPECT_EQ(rollback.recover(generation()), InventoryPublicationResult::Corrupt);
      ASSERT_EQ(rollback.beginEmpty(generation(), 2), InventoryPublicationResult::Ok);
    } else
      ASSERT_EQ(rollback.recover(generation()), InventoryPublicationResult::Ok);
    EXPECT_FALSE(storage.files.contains(InventoryPublication::INDEX));
    EXPECT_FALSE(storage.files.contains(InventoryPublication::PATHS));
  }
}

TEST(CompanionInventoryPublication, RecoveryAutomaticallySelectsOlderPairOrEmptyAndThenMakesNoWrites) {
  std::array<uint8_t, 8192> scratch;
  for (bool initial : {false, true}) {
    auto storage = initial ? initialMixed(scratch) : mixedSnapshots(scratch, true);
    RealValidator validator(storage, scratch);
    InventoryPublication publication(storage, validator, scratch);
    InventoryRollback rollback(storage, validator, scratch);
    InventoryRecovery recovery(storage, publication, rollback);
    ASSERT_EQ(recovery.recover(generation()), InventoryPublicationResult::Ok);
    if (initial) {
      EXPECT_FALSE(storage.files.contains(InventoryPublication::INDEX));
      EXPECT_FALSE(storage.files.contains(InventoryPublication::PATHS));
    } else
      EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 1),
                InventoryValidation::Valid);
    const auto mutations = storage.mutation;
    EXPECT_EQ(recovery.recover(generation()), InventoryPublicationResult::Ok);
    EXPECT_EQ(storage.mutation, mutations);
  }
}
TEST(CompanionInventoryPublication, RecoveryResumesTornInitialRollbackIntentInBothModes) {
  std::array<uint8_t, 8192> scratch;
  for (bool initial : {false, true})
    for (unsigned failure : {1u, 2u}) {
      auto storage = initial ? initialMixed(scratch) : mixedSnapshots(scratch, true);
      RealValidator validator(storage, scratch);
      InventoryPublication publication(storage, validator, scratch);
      InventoryRollback rollback(storage, validator, scratch);
      storage.failAt = failure;
      storage.torn = true;
      EXPECT_EQ(initial ? rollback.beginEmpty(generation(), 2) : rollback.begin(generation(), 2),
                InventoryPublicationResult::IoError);
      storage.failAt = 0;
      InventoryRecovery restarted(storage, publication, rollback);
      ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
      bool pending = true;
      ASSERT_TRUE(rollback.pending(pending));
      EXPECT_FALSE(pending);
    }
}
TEST(CompanionInventoryPublication, RecoveryPreservesConflictingValidRollbackIntents) {
  std::array<uint8_t, 8192> scratch;
  auto storage = mixedSnapshots(scratch, true);
  RealValidator validator(storage, scratch);
  InventoryPublication publication(storage, validator, scratch);
  InventoryRollback rollback(storage, validator, scratch);
  storage.failAt = 3;
  ASSERT_EQ(rollback.begin(generation(), 2), InventoryPublicationResult::IoError);
  storage.failAt = 0;
  auto& second = storage.files.at(InventoryRollback::INTENTS[1]);
  second[3] = 2;
  inventory_detail::write(second, 28, inventoryIndexCrc(std::span(second).first(28)), 4);
  const auto original = storage.files;
  InventoryRecovery recovery(storage, publication, rollback);
  EXPECT_EQ(recovery.recover(generation()), InventoryPublicationResult::Corrupt);
  EXPECT_EQ(storage.files, original);
}
