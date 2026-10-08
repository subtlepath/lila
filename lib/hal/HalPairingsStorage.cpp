#include "HalPairingsStorage.h"

#include <Logging.h>
#include <Preferences.h>

#include <algorithm>
namespace companion {
namespace {
constexpr char NVS_NAMESPACE[] = "lila-companion";
constexpr char NVS_KEY[] = "pairings";
}  // namespace
PairingsRead HalPairingsStorage::read(std::span<uint8_t> bytes) {
  if (bytes.size() != PAIRINGS_RECORD_SIZE) return PairingsRead::Error;
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("COMPANION", "Pairings NVS open failed");
    return PairingsRead::Error;
  }
  const bool exists = preferences.isKey(NVS_KEY);
  const size_t size = preferences.getBytesLength(NVS_KEY);
  if (!exists) {
    preferences.end();
    return PairingsRead::Missing;
  }
  const size_t count = size == bytes.size() ? preferences.getBytes(NVS_KEY, bytes.data(), bytes.size()) : 0;
  preferences.end();
  if (count != bytes.size()) {
    LOG_ERR("COMPANION", "Pairings NVS read/size failed");
    return PairingsRead::Error;
  }
  return PairingsRead::Present;
}
bool HalPairingsStorage::write(std::span<const uint8_t> bytes) {
  if (bytes.size() != PAIRINGS_RECORD_SIZE) {
    LOG_ERR("COMPANION", "Pairings record size invalid");
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("COMPANION", "Pairings NVS open failed");
    return false;
  }
  const size_t written = preferences.putBytes(NVS_KEY, bytes.data(), bytes.size());
  const size_t count = preferences.getBytes(NVS_KEY, verified.data(), verified.size());
  preferences.end();
  const bool committed =
      written == bytes.size() && count == bytes.size() && std::equal(bytes.begin(), bytes.end(), verified.begin());
  std::fill(verified.begin(), verified.end(), 0);
  if (!committed) LOG_ERR("COMPANION", "Pairings NVS commit/readback failed");
  return committed;
}
}  // namespace companion
