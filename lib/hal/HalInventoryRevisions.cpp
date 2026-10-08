#include "HalInventoryRevisions.h"

#include <Logging.h>
#include <Preferences.h>

#include <algorithm>
#include <array>

namespace companion {
namespace {
constexpr char NVS_NAMESPACE[] = "lila-companion";
constexpr char NVS_KEY[] = "inventory-rev";
bool failure(const char* operation) {
  LOG_ERR("COMPANION", "Inventory revision %s failed", operation);
  return false;
}
}  // namespace
bool HalInventoryRevisions::reserve(uint64_t after, uint64_t& revision) {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return failure("open");
  struct Close {
    Preferences& preferences;
    ~Close() { preferences.end(); }
  } close{preferences};
  std::array<uint8_t, 8> bytes{};
  const size_t length = preferences.getBytesLength(NVS_KEY);
  uint64_t previous = 0;
  if (length == 0) {
    if (preferences.isKey(NVS_KEY)) return failure("invalid counter");
  } else {
    if (length != bytes.size() || preferences.getBytes(NVS_KEY, bytes.data(), bytes.size()) != bytes.size())
      return failure("read");
    for (unsigned i = 0; i < 8; ++i) previous |= uint64_t(bytes[i]) << (8 * i);
    if (previous == 0) return failure("zero counter");
  }
  const uint64_t latest = std::max(previous, after);
  if (latest == UINT64_MAX) return failure("exhausted");
  const uint64_t next = latest + 1;
  for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(next >> (8 * i));
  if (preferences.putBytes(NVS_KEY, bytes.data(), bytes.size()) != bytes.size()) return failure("write");
  std::array<uint8_t, 8> verified{};
  if (preferences.getBytes(NVS_KEY, verified.data(), verified.size()) != verified.size() || verified != bytes)
    return failure("readback");
  revision = next;
  return true;
}
}  // namespace companion
