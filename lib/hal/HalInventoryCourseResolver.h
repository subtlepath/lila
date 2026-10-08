#pragma once

#include <Logging.h>

#include <cstring>

#include "CompanionCourseBinding.h"
#include "HalCourseValidation.h"
#include "HalInventoryFileSource.h"

namespace companion {
// Adds active-course resolution; the delegate remains responsible for other content.
class HalInventoryCourseResolver final : public InventoryFileResolver {
 public:
#if LILA_TINTA
  // Pack stays session-owned outside the stack; scratch borrows the hash bank.
  HalInventoryCourseResolver(TransferStorage& storage, tinta::core::pack::Pack& pack, InventoryFileResolver& delegate,
                             std::span<uint8_t> scratch)
      : storage(storage), pack(pack), delegate(delegate), scratch(scratch) {}
#else
  explicit HalInventoryCourseResolver(InventoryFileResolver& delegate) : delegate(delegate) {}
#endif
  InventoryFileDecision resolve(const char* path, HalFile& file, ContentManifest& metadata) override {
    course = activeCoursePath(path);
    if (!course) return delegate.resolve(path, file, metadata);
#if LILA_TINTA
    if (!file || file.isDirectory() || scratch.size() < 512) return failure("Invalid active course scan");
    CourseCandidateDetails details;
    if (!validateStagedCourse(path, pack, scratch, details)) return failure("Active course validation failed");
    const auto binding = readCourseBinding(storage, COURSE_BINDING_PATH, scratch, expected, bound);
    if (binding != CourseBindingResult::Ok) return failure("Active course binding is unreadable");
    if (bound && expected.formatVersion != details.major) return failure("Active course binding format differs");
    metadata = {};
    metadata.kind = ContentKind::Course;
    metadata.formatVersion = details.major;
    // Legacy packs remain unbound until their exact bytes are explicitly associated.
    if (bound) metadata.logicalIdentity = expected.logicalIdentity;
    return InventoryFileDecision::Include;
#else
    return failure("Active course requires a Tinta build");
#endif
  }
  InventoryFileDecision resolveBundle(const char* path, ContentManifest& metadata, const char*& archivePath) override {
    return delegate.resolveBundle(path, metadata, archivePath);
  }
  bool verifyHashed(const char* path, const ContentManifest& manifest) override {
    if (!course) return delegate.verifyHashed(path, manifest);
#if LILA_TINTA
    if (!activeCoursePath(path) || manifest.kind != ContentKind::Course) return false;
    if (!bound) return manifest.logicalIdentity == Identity{};
    return manifest == expected;
#else
    return false;
#endif
  }

 private:
  static bool activeCoursePath(const char* path) {
    if (!path) return false;
    for (size_t at = 0;; ++at) {
      const unsigned char byte = path[at];
      const unsigned char lower = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
      if (lower != ACTIVE_COURSE_PATH[at]) return false;
      if (byte == 0) return true;
    }
  }
#if LILA_TINTA
  TransferStorage& storage;
  tinta::core::pack::Pack& pack;
#endif
  InventoryFileResolver& delegate;
#if LILA_TINTA
  std::span<uint8_t> scratch;
  ContentManifest expected;
  bool bound = false;
#endif
  bool course = false;
  InventoryFileDecision failure(const char* reason) {
    LOG_ERR("COMPANION", "%s", reason);
    return InventoryFileDecision::Error;
  }
};
}  // namespace companion
