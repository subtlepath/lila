#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "CompanionCourseRemovalPlan.h"
#include "HalRemovalStageClaimStorage.h"

namespace companion {
enum class CourseRemovalPlanStorageResult { Ok, Missing, Invalid, Corrupt, Conflict, IoError };
// Session-owned. Input/retained-plan bytes and comparison scratch must be disjoint.
// The serialized owner excludes concurrent plan and content changes.
class HalCourseRemovalPlanStorage final {
 public:
  explicit HalCourseRemovalPlanStorage(std::span<uint8_t> comparison) : comparison(comparison), claims(comparison) {
    mbedtls_sha256_init(&hashContext);
  }
  ~HalCourseRemovalPlanStorage() {
    close();
    mbedtls_sha256_free(&hashContext);
  }
  HalCourseRemovalPlanStorage(const HalCourseRemovalPlanStorage&) = delete;
  HalCourseRemovalPlanStorage& operator=(const HalCourseRemovalPlanStorage&) = delete;
  const char* publishedPath() const { return ready ? target.data() : nullptr; }
  CourseRemovalPlanStorageResult publish(std::span<const uint8_t> bytes, uint64_t inventoryRevision, Digest& output) {
    ready = false;
    if (comparison.size() < REMOVAL_STAGE_CLAIM_SIZE || !inventoryRevision || !disjoint(bytes, comparison) ||
        !codec.decode(bytes, parsed))
      return CourseRemovalPlanStorageResult::Invalid;
    if (!close() || !prepare() || !digest(bytes) || !paths(actual)) return io("publish preparation");
    const auto existing = inspect(target.data(), bytes);
    if (existing == CourseRemovalPlanStorageResult::Ok) {
      output = actual;
      ready = true;
      return existing;
    }
    if (existing != CourseRemovalPlanStorageResult::Missing) return existing;
    claim.request = parsed.request;
    claim.inventoryRevision = inventoryRevision;
    if (claims.persist(claim) != RemovalStageClaimResult::Ok) return io("stage claim");
    std::copy_n(claims.planStagePath(), strlen(claims.planStagePath()) + 1, stage.begin());
    const auto stagedResult = inspectStage();
    if (stagedResult != CourseRemovalPlanStorageResult::Ok && stagedResult != CourseRemovalPlanStorageResult::Missing)
      return stagedResult;
    if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file)) return io("stage write open");
    const bool written = !file.isDirectory() && file.write(bytes.data(), bytes.size()) == bytes.size() &&
                         file.truncate(bytes.size()) && file.sync();
    const bool closed = close();
    if (!written || !closed) return io("stage write/sync");
    const auto sealed = inspect(stage.data(), bytes);
    if (sealed != CourseRemovalPlanStorageResult::Ok) return sealed;
    if (claims.load(claim) != RemovalStageClaimResult::Ok ||
        lookup.inspect(target.data()) != CompanionFilePresence::Missing)
      return io("publication ownership or target");
    if (!Storage.rename(stage.data(), target.data())) return io("publication rename");
    const auto published = inspect(target.data(), bytes);
    if (published != CourseRemovalPlanStorageResult::Ok) return published;
    output = actual;
    ready = true;
    return published;
  }
  // Decoded output changes only after SHA, format and synchronized-file checks.
  // Reusing planBuffer replaces retained bytes even when loading fails.
  CourseRemovalPlanStorageResult load(const Digest& expected, std::span<uint8_t> planBuffer,
                                      CourseRemovalPlan& output) {
    ready = false;
    if (planBuffer.size() < COURSE_REMOVAL_PLAN_SIZE || !disjoint(planBuffer, comparison) ||
        !std::any_of(expected.begin(), expected.end(), [](uint8_t byte) { return byte != 0; }))
      return CourseRemovalPlanStorageResult::Invalid;
    if (!close() || !prepare() || !paths(expected)) return io("load preparation");
    const auto presence = lookup.inspect(target.data());
    if (presence == CompanionFilePresence::Missing) return CourseRemovalPlanStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", target.data(), file))
      return io("load open");
    if (file.isDirectory()) {
      close();
      return io("load directory");
    }
    const auto length = file.fileSize64();
    if (length != COURSE_REMOVAL_PLAN_SIZE) {
      if (!close()) return io("load close");
      return CourseRemovalPlanStorageResult::Corrupt;
    }
    auto bytes = planBuffer.first(static_cast<size_t>(length));
    if (file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) {
      close();
      return io("load read");
    }
    if (!digest(bytes)) {
      close();
      return io("load SHA");
    }
    const bool valid = actual == expected && codec.decode(bytes, parsed);
    const bool synced = valid && file.sync();
    const bool closed = close();
    if (!closed || (valid && !synced)) return io("load sync/close");
    if (!valid) return CourseRemovalPlanStorageResult::Corrupt;
    output = parsed;
    ready = true;
    return CourseRemovalPlanStorageResult::Ok;
  }

 private:
  std::span<uint8_t> comparison;
  HalFile file;
  HalCompanionFileLookup lookup;
  mbedtls_sha256_context hashContext;
  Digest actual{};
  std::array<char, 112> target{}, stage{};
  CourseRemovalPlan parsed, staged;
  CourseRemovalPlanCodec codec;
  RemovalStageClaim claim;
  HalRemovalStageClaimStorage claims;
  bool ready = false;
  static bool disjoint(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const auto first = reinterpret_cast<uintptr_t>(a.data());
    const auto second = reinterpret_cast<uintptr_t>(b.data());
    return first <= second ? a.size() <= second - first : b.size() <= first - second;
  }
  bool prepare() { return Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY); }
  bool close() { return !file.isOpen() || file.close() || failure("close"); }
  bool digest(std::span<const uint8_t> bytes) {
    return mbedtls_sha256_starts(&hashContext, 0) == 0 &&
           mbedtls_sha256_update(&hashContext, bytes.data(), bytes.size()) == 0 &&
           mbedtls_sha256_finish(&hashContext, actual.data()) == 0;
  }
  bool paths(const Digest& hash) {
    static constexpr char PREFIX[] = "/.crosspoint/companion/removal-course-plan-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(sizeof(PREFIX) + 64 + 4 <= 112);
    std::copy_n(PREFIX, sizeof(PREFIX) - 1, target.begin());
    size_t at = sizeof(PREFIX) - 1;
    for (const auto byte : hash) {
      target[at++] = HEX_DIGITS[byte >> 4];
      target[at++] = HEX_DIGITS[byte & 15];
    }
    target[at] = 0;
    std::copy_n(target.begin(), at, stage.begin());
    std::copy_n(".tmp", 5, stage.begin() + at);
    return true;
  }
  CourseRemovalPlanStorageResult inspectStage() {
    if (!close()) return io("stage close");
    const auto presence = lookup.inspect(stage.data());
    if (presence == CompanionFilePresence::Missing) return CourseRemovalPlanStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", stage.data(), file))
      return io("stage open");
    const auto size = file.fileSize64();
    if (file.isDirectory() || size > COURSE_REMOVAL_PLAN_SIZE) {
      close();
      return io("stage collision");
    }
    bool foreign = false;
    if (size == COURSE_REMOVAL_PLAN_SIZE) {
      if (file.read(comparison.data(), size) != static_cast<int>(size)) {
        close();
        return io("stage read");
      }
      foreign = codec.decode(comparison.first(COURSE_REMOVAL_PLAN_SIZE), staged) && staged != parsed;
    }
    const bool closed = close();
    if (!closed) return io("stage close");
    return foreign ? CourseRemovalPlanStorageResult::Conflict : CourseRemovalPlanStorageResult::Ok;
  }
  CourseRemovalPlanStorageResult inspect(const char* path, std::span<const uint8_t> expected) {
    if (!close()) return io("inspection close");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return CourseRemovalPlanStorageResult::Missing;
    if (presence != CompanionFilePresence::Present || !Storage.openFileForReadReusing("COMPANION", path, file))
      return io("inspection open");
    if (file.isDirectory()) {
      close();
      return io("inspection directory");
    }
    bool equal = file.fileSize64() == expected.size();
    size_t offset = 0;
    while (equal && offset < expected.size()) {
      auto part = comparison.first(std::min(comparison.size(), expected.size() - offset));
      if (file.read(part.data(), part.size()) != static_cast<int>(part.size())) {
        close();
        return io("inspection read");
      }
      equal = std::equal(part.begin(), part.end(), expected.begin() + offset);
      offset += part.size();
    }
    const bool synced = equal && file.sync();
    const bool closed = close();
    if (!closed || (equal && !synced)) return io("inspection sync/close");
    return equal ? CourseRemovalPlanStorageResult::Ok : CourseRemovalPlanStorageResult::Corrupt;
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Course removal plan %s failed", operation);
    return false;
  }
  static CourseRemovalPlanStorageResult io(const char* operation) {
    failure(operation);
    return CourseRemovalPlanStorageResult::IoError;
  }
};
}  // namespace companion
