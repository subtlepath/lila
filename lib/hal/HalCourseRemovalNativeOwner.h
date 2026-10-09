#pragma once

#include "HalCourseRemovalMetadata.h"
#include "HalCourseRemovalSession.h"
#include "HalInventoryPublicationValidator.h"

namespace companion {
// Allocate once after checked heap admission. Fixed decoder/handle owners and
// the reusable inventory/migration IO bank exceed task-stack limits.
class HalCourseRemovalNativeOwner final {
 public:
  using Callback = HalEpubRemovalBackend::Callback;
  // Validate the active pack and prepare scoped state without replacing its binding.
  using PrepareState = bool (*)(void*, const ContentManifest&, std::span<uint8_t>);
  // Inventory callback publishes a complete pair and returns its revision.
  HalCourseRemovalNativeOwner(const Identity& generation, Callback permitted, Callback refresh, void* context,
                              HalCourseRemovalAdmission::InventoryReady inventoryReady, PrepareState prepareState)
      : generation(generation),
        permitted(permitted),
        refresh(refresh),
        context(context),
        inventoryReady(inventoryReady),
        prepareState(prepareState),
        validator(scratch),
        paths(pathStorage, scratch),
        metadata(permission, this),
        session(generation, paths, revision, metadata, scratch, permission, refreshReader, this, inventory,
                prepareCourse) {}
  bool prepare() { return allowed() && session.prepare() && allowed(); }
  bool openInventory(uint64_t requestedRevision) {
    revision = 0;
    if (!allowed() || !requestedRevision ||
        validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, requestedRevision) !=
            InventoryValidation::Valid ||
        !allowed() || !pathStorage.open(InventoryPublication::PATHS) || !allowed())
      return fail("inventory pair");
    revision = requestedRevision;
    return true;
  }
  bool closeReaders() {
    const bool sessionClosed = session.closeReaders();
    const bool pathsClosed = pathStorage.close();
    const bool metadataClosed = metadata.closeReaders();
    return sessionClosed && pathsClosed && metadataClosed;
  }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return session.handle(authorized, owner, request, reply);
  }

 private:
  Identity generation;
  Callback permitted, refresh;
  void* context;
  HalCourseRemovalAdmission::InventoryReady inventoryReady;
  PrepareState prepareState;
  static constexpr size_t COVERAGE_BYTES = 64;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD + COVERAGE_BYTES> scratch{};
  HalInventoryPublicationValidator validator;
  HalInventoryIndexStorage pathStorage;
  InventoryPaths paths;
  HalCourseRemovalMetadata metadata;
  uint64_t revision = 0;
  ContentManifest binding;
  HalCourseRemovalSession session;
  bool allowed() const { return permitted && permitted(context); }
  static bool permission(void* context) { return static_cast<HalCourseRemovalNativeOwner*>(context)->allowed(); }
  static bool refreshReader(void* context) {
    auto& self = *static_cast<HalCourseRemovalNativeOwner*>(context);
    return self.allowed() && self.refresh && self.refresh(self.context) && self.allowed();
  }
  static bool inventory(void* context, uint64_t& revision) {
    auto& self = *static_cast<HalCourseRemovalNativeOwner*>(context);
    if (!self.allowed() || !self.inventoryReady || !self.inventoryReady(self.context, revision) || !self.allowed())
      return false;
    return self.openInventory(revision);
  }
  bool verifyBinding(const ContentManifest& expected) {
    bool present = false;
    if (!allowed() ||
        readCourseBinding(metadata, COURSE_BINDING_PATH, scratch, binding, present) != CourseBindingResult::Ok ||
        !allowed() || !present || binding != expected)
      return false;
    uint64_t length = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP})
      if (!allowed() || metadata.stat(path, length) != FileStatus::Missing || !allowed()) return false;
    return allowed();
  }
  static bool prepareCourse(void* context, const ContentManifest& expected) {
    auto& self = *static_cast<HalCourseRemovalNativeOwner*>(context);
    return self.verifyBinding(expected) && self.prepareState &&
           self.prepareState(self.context, expected, self.scratch) && self.allowed() && self.verifyBinding(expected);
  }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Native course removal owner %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion
