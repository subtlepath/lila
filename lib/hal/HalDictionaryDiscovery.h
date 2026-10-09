#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionInventoryPaths.h"

namespace companion {
enum class DictionaryDiscoveryResult { Found, Empty, Error };
struct DictionaryDiscoveryDetails {
  bool compressed = false, synonyms = false;
  bool operator==(const DictionaryDiscoveryDetails&) const = default;
};
// Session-owned fixed buffers and two retained HAL handles. The base path is
// borrowed until the next inspect call; discovery does not validate contents.
class HalDictionaryDiscovery final {
 public:
  using Progress = bool (*)(void*);
  explicit HalDictionaryDiscovery(Progress progress = nullptr, void* context = nullptr)
      : progress(progress), context(context) {}
  ~HalDictionaryDiscovery() { close(); }
  bool closeReaders() { return close(); }
  const char* basePath() const { return base.data(); }
  DictionaryDiscoveryResult inspect(std::string_view folder, DictionaryDiscoveryDetails& output) {
    if (!close()) return error("Closing previous dictionary discovery failed");
    operations = 0;
    if (folder.size() > base.size() - 10 || !validInventoryPath(folder)) return error("Invalid dictionary folder");
    if (folder.data() != base.data()) std::copy(folder.begin(), folder.end(), base.begin());
    base[folder.size()] = 0;
    if (!Storage.openFileForReadReusing("COMPANION", base.data(), directory) || !directory.isDirectory() ||
        !entry.prepareDirectoryEntry())
      return error("Opening dictionary discovery failed");
    stem[0] = 0;
    for (;;) {
      const auto result = next();
      if (result == HalDirectoryResult::Error) return error("Reading dictionary discovery failed");
      if (result == HalDirectoryResult::End) break;
      const std::string_view filename(name.data());
      if (entry.isDirectory() || filename.starts_with("._") || !filename.ends_with(".idx")) continue;
      if (filename.size() <= 4 || stem[0]) return error("Ambiguous dictionary index stem");
      std::copy_n(filename.begin(), filename.size() - 4, stem.begin());
      stem[filename.size() - 4] = 0;
    }
    if (!stem[0]) {
      if (!close()) return error("Closing empty dictionary discovery failed");
      base[0] = 0;
      return DictionaryDiscoveryResult::Empty;
    }
    directory.rewindDirectory();
    uint8_t found = 0;
    for (;;) {
      const auto result = next();
      if (result == HalDirectoryResult::Error) return error("Reading dictionary members failed");
      if (result == HalDirectoryResult::End) break;
      const std::string_view filename(name.data());
      if (entry.isDirectory() || filename.starts_with("._")) continue;
      const std::string_view selected(stem.data());
      if (filename.ends_with(".idx") && (filename.size() != selected.size() + 4 || !filename.starts_with(selected)))
        return error("Dictionary index changed during discovery");
      if (!filename.starts_with(selected)) continue;
      const auto suffix = filename.substr(selected.size());
      uint8_t flag = 0;
      if (suffix == ".idx") flag = 1;
      if (suffix == ".ifo") flag = 2;
      if (suffix == ".dict") flag = 4;
      if (suffix == ".dict.dz") flag = 8;
      if (suffix == ".syn") flag = 16;
      if (flag && (found & flag)) return error("Duplicate dictionary member");
      found |= flag;
    }
    const auto stemLength = std::strlen(stem.data());
    if ((found & 3) != 3 || !(found & 12) || folder.size() + 1 + stemLength > base.size() - 9)
      return error("Missing or overlong dictionary members");
    if (!close()) return error("Closing dictionary discovery failed");
    base[folder.size()] = '/';
    std::copy_n(stem.begin(), stemLength + 1, base.begin() + folder.size() + 1);
    if (!validInventoryPath(std::string_view(base.data(), folder.size() + 1 + stemLength)))
      return error("Invalid dictionary base path");
    output = {!(found & 4), bool(found & 16)};
    return DictionaryDiscoveryResult::Found;
  }

 private:
  HalFile directory, entry;
  std::array<char, 512> base{};
  std::array<char, 256> name{}, stem{};
  uint8_t operations = 0;
  Progress progress;
  void* context;
  bool close() {
    bool ok = true;
    if (entry.isOpen() && !entry.close()) ok = false;
    if (directory.isOpen() && !directory.close()) ok = false;
    if (!ok) LOG_ERR("COMPANION", "Closing dictionary discovery handles failed");
    return ok;
  }
  HalDirectoryResult next() {
    if (progress && !progress(context)) return HalDirectoryResult::Error;
    if (entry.isOpen() && !entry.close()) return HalDirectoryResult::Error;
    const auto result = directory.nextEntry(entry);
    if (result != HalDirectoryResult::Entry) return result;
    const auto length = entry.getName(name.data(), name.size());
    if (!length || length >= name.size() || name[length] != 0) return HalDirectoryResult::Error;
    for (size_t at = 0; at < length; ++at) {
      const auto byte = static_cast<unsigned char>(name[at]);
      if (byte < 32 || byte == 127 || byte == '/' || byte == '\\' || byte == ':') return HalDirectoryResult::Error;
    }
    if (++operations == 32) {
      operations = 0;
      vTaskDelay(1);
    }
    return result;
  }
  DictionaryDiscoveryResult error(const char* reason) {
    LOG_ERR("COMPANION", "%s", reason);
    base[0] = 0;
    close();
    return DictionaryDiscoveryResult::Error;
  }
};
}  // namespace companion
