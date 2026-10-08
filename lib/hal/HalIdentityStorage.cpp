#include "HalIdentityStorage.h"

#include <Esp.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <array>

namespace companion {
namespace {
constexpr char NVS_NAMESPACE[] = "lila-companion";
constexpr char NVS_KEY[] = "identity";
bool failed(const char* operation) {
  LOG_ERR("COMPANION", "Identity %s failed", operation);
  return false;
}
}  // namespace

bool HalIdentityStorage::hardwareIdentity(Identity& output) {
  const uint64_t mac = ESP.getEfuseMac();
  if (mac == 0) return failed("eFuse read");
  static constexpr uint8_t PREFIX[] = {'L', 'I', 'L', 'A', 'D', 'E', 'V', 1};
  std::copy(std::begin(PREFIX), std::end(PREFIX), output.begin());
  for (unsigned i = 0; i < 8; ++i) output[8 + i] = static_cast<uint8_t>(mac >> (8 * i));
  return true;
}
bool HalIdentityStorage::cardIdentity(Identity& output) {
  return Storage.cardIdentity(output.data()) || failed("CID read");
}
IdentityRead HalIdentityStorage::readBinding(std::span<uint8_t> bytes) {
  if (bytes.size() != IDENTITY_RECORD_SIZE) return IdentityRead::Corrupt;
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    failed("NVS open");
    return IdentityRead::Error;
  }
  const size_t size = preferences.getBytesLength(NVS_KEY);
  if (size == 0) {
    const bool exists = preferences.isKey(NVS_KEY);
    preferences.end();
    return exists ? IdentityRead::Corrupt : IdentityRead::Missing;
  }
  if (size != bytes.size()) {
    preferences.end();
    return IdentityRead::Corrupt;
  }
  const size_t read = preferences.getBytes(NVS_KEY, bytes.data(), bytes.size());
  preferences.end();
  if (read != bytes.size()) {
    failed("NVS read");
    return IdentityRead::Error;
  }
  return IdentityRead::Present;
}
bool HalIdentityStorage::writeBinding(std::span<const uint8_t> bytes) {
  if (bytes.size() != IDENTITY_RECORD_SIZE) return failed("NVS record size");
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return failed("NVS open");
  const size_t written = preferences.putBytes(NVS_KEY, bytes.data(), bytes.size());
  std::array<uint8_t, IDENTITY_RECORD_SIZE> verified{};
  const size_t read = preferences.getBytes(NVS_KEY, verified.data(), verified.size());
  preferences.end();
  if (written != bytes.size() || read != bytes.size() || !std::equal(bytes.begin(), bytes.end(), verified.begin())) {
    return failed("NVS commit/readback");
  }
  return true;
}
IdentityRead HalIdentityStorage::readMarker(Identity& output) {
  if (!Storage.ready()) {
    failed("SD unavailable");
    return IdentityRead::Error;
  }
  if (!Storage.exists(CARD_MARKER_PATH)) return IdentityRead::Missing;
  HalFile file;
  if (!Storage.openFileForRead("COMPANION", CARD_MARKER_PATH, file)) return IdentityRead::Error;
  if (file.fileSize64() != output.size()) return IdentityRead::Corrupt;
  if (file.read(output.data(), output.size()) != static_cast<int>(output.size())) {
    failed("marker read");
    return IdentityRead::Error;
  }
  return IdentityRead::Present;
}
bool HalIdentityStorage::createMarker(const Identity& value) {
  if (!Storage.ensureDirectoryExists("/.crosspoint/companion")) return failed("marker directory");
  auto file = Storage.open(CARD_MARKER_PATH, O_WRONLY | O_CREAT | O_EXCL);
  if (!file || file.write(value.data(), value.size()) != value.size() || !file.sync()) return failed("marker sync");
  return true;
}
bool HalIdentityStorage::randomIdentity(Identity& output) {
  // Non-secret uniqueness tokens. Session encryption keys require a separate
  // authenticated-radio entropy lifecycle, not this provisioning API.
  esp_fill_random(output.data(), output.size());
  return true;
}
}  // namespace companion
