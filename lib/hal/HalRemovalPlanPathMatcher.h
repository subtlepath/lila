#pragma once

#include "HalMultiPathRemovalPlanStorage.h"
#include "HalRemovalReferencePath.h"

namespace companion {
// Off-stack owner; the reader and transaction guard outlive this adapter.
class HalRemovalPlanPathMatcher final {
 public:
  using Guard = bool (*)(void*);
  HalRemovalPlanPathMatcher(HalMultiPathRemovalPlanStorage& reader, Guard guard, void* context)
      : reader(reader), guard(guard), context(context) {}
  static bool callback(void* opaque, std::string_view candidate, bool& matched) {
    if (!opaque) return false;
    return static_cast<HalRemovalPlanPathMatcher*>(opaque)->match(candidate, matched);
  }
  bool match(std::string_view candidate, bool& matched) {
    if (!permitted() || !reader.rewind() || !permitted()) return failure();
    bool found = false;
    for (;;) {
      if (!permitted()) return failure();
      const auto result = reader.next(path);
      if (!permitted() || result == InventoryPathRecordResult::Error) return failure();
      if (result == InventoryPathRecordResult::End) {
        matched = found;
        return true;
      }
      if (removalReferencePathEqual(nullptr, candidate, path.data())) found = true;
    }
  }

 private:
  HalMultiPathRemovalPlanStorage& reader;
  Guard guard;
  void* context;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  bool permitted() const { return guard && guard(context); }
  static bool failure() {
    LOG_ERR("COMPANION", "Removal plan reference matching failed");
    return false;
  }
};
}  // namespace companion
