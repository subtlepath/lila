#pragma once

#include "CompanionDictionaryFatCase.h"
#include "CompanionDictionaryInstallationPaths.h"
#include "CompanionUnicodeUtf8Nfc.h"
#include "HalCompanionFileLookup.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class DictionaryDestinationPresence { Missing, Present, Conflict, Error };
enum class DictionaryDestinationFolder { Empty, Occupied, Conflict, Error };
// Borrow disjoint NFC banks from the installation session. No allocation.
// Matches SdFat long-name case equivalence after NFC normalization.
class HalDictionaryDestinationLookup final {
 public:
  HalDictionaryDestinationLookup(std::span<uint32_t> decoded, std::span<uint32_t> normalized, std::span<uint8_t> wanted,
                                 std::span<uint8_t> found, InventoryHashProgress progress = nullptr,
                                 void* context = nullptr)
      : decoded(decoded), normalized(normalized), wanted(wanted), found(found), progress(progress), context(context) {}
  ~HalDictionaryDestinationLookup() { close(); }
  DictionaryDestinationPresence resolveReaderFolder(std::string_view child, std::span<char> output) {
    if (!output.empty()) output.front() = 0;
    if (child.empty() || child.size() > 31 || child.front() == '.' ||
        child.find_first_of("/\\") != std::string_view::npos)
      return error("reader folder arguments");
    static constexpr std::string_view ROOTS[] = {"/dictionaries", "/.dictionaries"};
    for (const auto root : ROOTS) {
      auto status = inspectChild("/", root.substr(1), true);
      if (status == DictionaryDestinationPresence::Missing) continue;
      if (status != DictionaryDestinationPresence::Present) return status;
      status = inspectChild(root, child, true);
      if (status == DictionaryDestinationPresence::Missing) continue;
      if (status != DictionaryDestinationPresence::Present) return status;
      if (output.size() <= root.size() + 1 + child.size()) return error("reader folder bounds");
      std::copy(root.begin(), root.end(), output.begin());
      output[root.size()] = '/';
      std::copy(child.begin(), child.end(), output.begin() + root.size() + 1);
      output[root.size() + 1 + child.size()] = 0;
      return DictionaryDestinationPresence::Present;
    }
    return DictionaryDestinationPresence::Missing;
  }
  DictionaryDestinationFolder prepareEmptyFolder(const DictionaryInstallationPlan& plan) {
    if (!validDictionaryInstallationPlan(plan)) return DictionaryDestinationFolder::Error;
    std::copy(plan.base.begin(), plan.base.end(), path.begin());
    const std::string_view full(path.data());
    const auto rootEnd = full.find('/', 1);
    const auto folderEnd = full.find_last_of('/');
    const auto root = full.substr(1, rootEnd - 1);
    auto status = ensureChild("/", root);
    if (status != DictionaryDestinationPresence::Present)
      return status == DictionaryDestinationPresence::Conflict ? DictionaryDestinationFolder::Conflict
                                                               : DictionaryDestinationFolder::Error;
    status = ensureChild(full.substr(0, rootEnd), full.substr(rootEnd + 1, folderEnd - rootEnd - 1));
    if (status != DictionaryDestinationPresence::Present)
      return status == DictionaryDestinationPresence::Conflict ? DictionaryDestinationFolder::Conflict
                                                               : DictionaryDestinationFolder::Error;
    return inspectEmptyFolder(plan);
  }
  DictionaryDestinationPresence inspect(const DictionaryInstallationPlan& plan, unsigned member) {
    if (!close() || !dictionaryInstallationMemberPath(plan, member, path)) return error("arguments or close");
    const std::string_view full(path.data());
    const auto slash = full.find_last_of('/');
    const auto filename = full.substr(slash + 1);
    return inspectChild(full.substr(0, slash), filename, false);
  }
  DictionaryDestinationPresence inspectAncestors(const DictionaryInstallationPlan& plan) {
    if (!close() || !validDictionaryInstallationPlan(plan)) return error("ancestor arguments or close");
    std::copy(plan.base.begin(), plan.base.end(), path.begin());
    const std::string_view full(path.data());
    const auto rootEnd = full.find('/', 1);
    const auto folderEnd = full.find_last_of('/');
    const auto root = full.substr(1, rootEnd - 1);
    const auto child = full.substr(rootEnd + 1, folderEnd - rootEnd - 1);
    const auto status = inspectChild("/", root, true);
    if (status != DictionaryDestinationPresence::Present) return status;
    return inspectChild(full.substr(0, rootEnd), child, true);
  }
  DictionaryDestinationFolder inspectEmptyFolder(const DictionaryInstallationPlan& plan) {
    const auto ancestors = inspectAncestors(plan);
    if (ancestors == DictionaryDestinationPresence::Conflict) return DictionaryDestinationFolder::Conflict;
    if (ancestors != DictionaryDestinationPresence::Present) return DictionaryDestinationFolder::Error;
    const std::string_view full(plan.base.data());
    const auto end = full.find_last_of('/');
    std::copy_n(full.begin(), end, folder.begin());
    folder[end] = 0;
    if (!Storage.openFileForReadReusing("COMPANION", folder.data(), directory) || !directory.isDirectory() ||
        !entry.prepareDirectoryEntry() || (progress && !progress(context))) {
      error("empty destination open or cancellation");
      return DictionaryDestinationFolder::Error;
    }
    const auto result = directory.nextEntry(entry);
    const bool closed = close();
    if (result == HalDirectoryResult::Error || !closed) {
      error("empty destination enumeration or close");
      return DictionaryDestinationFolder::Error;
    }
    return result == HalDirectoryResult::End ? DictionaryDestinationFolder::Empty
                                             : DictionaryDestinationFolder::Occupied;
  }

 private:
  DictionaryDestinationPresence ensureChild(std::string_view parent, std::string_view child) {
    const auto status = inspectChild(parent, child, true);
    if (status != DictionaryDestinationPresence::Missing) return status;
    const auto slash = parent == "/" ? 0u : 1u;
    if (parent.size() + slash + child.size() >= folder.size()) return error("directory path bounds");
    std::copy(parent.begin(), parent.end(), folder.begin());
    size_t at = parent.size();
    if (slash) folder[at++] = '/';
    std::copy(child.begin(), child.end(), folder.begin() + at);
    folder[at + child.size()] = 0;
    if ((progress && !progress(context)) || !Storage.ensureDirectoryExists(folder.data()))
      return error("directory creation");
    return inspectChild(parent, child, true);
  }
  DictionaryDestinationPresence inspectChild(std::string_view parent, std::string_view filename,
                                             bool expectedDirectory) {
    if (!close() || parent.size() >= folder.size() || (parent != "/" && !validInventoryPath(parent)))
      return error("parent arguments");
    size_t wantedBytes = 0;
    if (!normalize(filename, wanted, wantedBytes)) return error("destination normalization");
    std::copy(parent.begin(), parent.end(), folder.begin());
    folder[parent.size()] = 0;
    if (!Storage.openFileForReadReusing("COMPANION", folder.data(), directory) || !directory.isDirectory() ||
        !entry.prepareDirectoryEntry())
      return error("parent directory");
    bool matched = false;
    unsigned steps = 0;
    for (;;) {
      if ((progress && !progress(context)) || !closeEntry()) return error("cancelled or entry close");
      const auto status = directory.nextEntry(entry);
      if (status == HalDirectoryResult::Error) return error("enumeration");
      if (status == HalDirectoryResult::End) {
        if (!close()) return error("directory close");
        return matched ? DictionaryDestinationPresence::Present : DictionaryDestinationPresence::Missing;
      }
      const auto length = entry.getName(name.data(), name.size());
      size_t foundBytes = 0;
      if (!length || length >= name.size() || name[length] ||
          !normalize(std::string_view(name.data(), length), found, foundBytes))
        return error("entry name");
      const bool longMatch = equivalent(wanted.first(wantedBytes), found.first(foundBytes));
      if (!entry.getShortName(alias.data(), alias.size())) return error("entry alias");
      const auto aliasLength = strnlen(alias.data(), alias.size());
      if (aliasLength == alias.size()) return error("unterminated alias");
      bool aliasMatch = false;
      if (aliasLength) {
        if (!normalize(std::string_view(alias.data(), aliasLength), found, foundBytes)) return error("alias encoding");
        aliasMatch = equivalent(wanted.first(wantedBytes), found.first(foundBytes));
      }
      if (longMatch || aliasMatch) {
        if (matched || std::string_view(name.data(), length) != filename || entry.isDirectory() != expectedDirectory) {
          if (!close()) return error("ambiguous entry close");
          LOG_ERR("COMPANION", "Ambiguous dictionary destination member");
          return DictionaryDestinationPresence::Conflict;
        }
        matched = true;
      }
      if (++steps == 32) {
        steps = 0;
        vTaskDelay(1);
      }
    }
  }

 private:
  HalFile directory, entry;
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
  std::array<char, 128> folder{};
  std::array<char, 256> name{};
  std::array<char, 13> alias{};
  std::span<uint32_t> decoded, normalized;
  std::span<uint8_t> wanted, found;
  InventoryHashProgress progress;
  void* context;
  bool normalize(std::string_view input, std::span<uint8_t> output, size_t& bytes) {
    return UnicodeUtf8Nfc::normalize(std::span(reinterpret_cast<const uint8_t*>(input.data()), input.size()), decoded,
                                     normalized, output, bytes);
  }
  static bool equivalent(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    // Both byte streams have already passed strict UTF-8 normalization.
    const auto next = [](std::span<const uint8_t>& bytes) {
      const auto first = bytes.front();
      unsigned count = first < 128 ? 1 : first < 224 ? 2 : first < 240 ? 3 : 4;
      uint32_t scalar = count == 1 ? first : first & (0x7f >> count);
      for (unsigned at = 1; at < count; ++at) scalar = (scalar << 6) | (bytes[at] & 63);
      bytes = bytes.subspan(count);
      return dictionaryFatUpcase(scalar);
    };
    while (!a.empty() && !b.empty()) {
      if (next(a) != next(b)) return false;
    }
    return a.empty() && b.empty();
  }
  bool closeEntry() { return !entry.isOpen() || entry.close(); }
  bool close() {
    const bool a = closeEntry();
    const bool b = !directory.isOpen() || directory.close();
    return a && b;
  }
  DictionaryDestinationPresence error(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary destination lookup %s failed", reason);
    close();
    return DictionaryDestinationPresence::Error;
  }
};
}  // namespace companion
