#pragma once

#include <ArduinoJson.h>
#include <HalStorage.h>

namespace bookmark_test {
inline bool failRead = false, failWrite = false;
inline unsigned writes = 0;
}  // namespace bookmark_test
class PersistableStoreBase {
 public:
  static bool readDocFromFile(const char* path, JsonDocument& doc) {
    const auto entry = inventory_hal_test::state.files.find(path);
    if (bookmark_test::failRead || entry == inventory_hal_test::state.files.end()) return false;
    return !deserializeJson(doc, entry->second.data(), entry->second.size());
  }
  static bool writeDocToFile(const char* path, const JsonDocument& doc) {
    if (bookmark_test::failWrite) return false;
    std::string text;
    serializeJson(doc, text);
    inventory_hal_test::state.files[path] = std::vector<uint8_t>(text.begin(), text.end());
    ++bookmark_test::writes;
    return true;
  }
};
