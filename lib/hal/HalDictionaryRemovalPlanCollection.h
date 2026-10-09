#pragma once

#include "HalDictionaryRemovalCohortPlanWriter.h"
#include "HalDictionaryRemovalPlanAssembly.h"

namespace companion {
// Off-stack fixed directory/name/plan state. Assembly, writer and authenticated
// card/revision guard are borrowed. Namespace writers remain excluded throughout.
// Repeated name scans sort folder prefixes; each folder's bytes are assembled once.
class HalDictionaryRemovalPlanCollection final {
 public:
  HalDictionaryRemovalPlanCollection(HalDictionaryRemovalPlanAssembly& assembly,
                                     HalDictionaryRemovalCohortPlanWriter& writer, InventoryHashProgress progress,
                                     void* context)
      : assembly(assembly), writer(writer), progress(progress), context(context) {}
  ~HalDictionaryRemovalPlanCollection() { close(); }
  bool closeReaders() { return close(); }
  bool collect(const ContentRemovalRequest& request, uint64_t revision) {
    lostOwnership = false;
    if (!progress || !guard() || !validContentRemovalRequest(request) ||
        request.manifest.kind != ContentKind::Dictionary || !revision || !close())
      return failure("request");
    if (!writer.begin(request, revision)) {
      writer.releaseHandles();
      return failure("stage claim");
    }
    previous[0] = 0;
    uint64_t count = 0;
    for (;;) {
      selected[0] = selectedAlias[0] = 0;
      found = false;
      if (!scan(false)) return abort("folder scan");
      if (!found) break;
      unique = 0;
      if (!scan(true) || unique != 1) return abort("folder alias collision");
      previous = selected;
      const auto length = std::strlen(selected.data());
      const auto result = assembly.assemble(std::string_view(selected.data(), length - 1), request, plan);
      if (!guard() || result == DictionaryRemovalAssemblyResult::Error) return abort("folder proof");
      if (result == DictionaryRemovalAssemblyResult::Match) {
        if (count == UINT64_MAX || !writer.append(plan) || !guard()) return abort("record write");
        ++count;
      }
    }
    if (!guard() || !count || !writer.finish(count) || !guard()) return abort("publication");
    return true;
  }

 private:
  struct Root {
    const char* name;
    const char* path;
  };
  static constexpr Root ROOTS[] = {{"dictionaries", "/dictionaries/"}, {".dictionaries", "/.dictionaries/"}};
  HalDictionaryRemovalPlanAssembly& assembly;
  HalDictionaryRemovalCohortPlanWriter& writer;
  InventoryHashProgress progress;
  void* context;
  HalFile rootDirectory, rootEntry, directory, entry;
  std::array<char, 256> name{};
  std::array<char, 13> alias{}, selectedAlias{};
  std::array<char, 128> prefix{}, previous{}, selected{};
  DictionaryRemovalPlan plan;
  unsigned selectedRoot = 0, unique = 0;
  bool found = false, lostOwnership = false;
  bool guard() {
    if (lostOwnership) return false;
    lostOwnership = !progress || !progress(context);
    return !lostOwnership;
  }
  bool closeHandle(HalFile& file) {
    if (!file.isOpen() || file.close()) return true;
    LOG_ERR("COMPANION", "Dictionary collection handle close failed");
    return false;
  }
  bool close() {
    const bool e = closeHandle(entry), d = closeHandle(directory), re = closeHandle(rootEntry),
               rd = closeHandle(rootDirectory);
    return e && d && re && rd;
  }
  bool folderEquivalent(std::string_view wanted, std::string_view current, bool& equal) {
    const auto compared = HalFileName::compare(wanted, current);
    if (compared == FileNameComparison::Invalid) return false;
    equal = compared == FileNameComparison::Equal;
    return true;
  }
  bool isSelected(unsigned root, size_t nameLength, bool& equal) {
    equal = false;
    if (root != selectedRoot) return true;
    const size_t rootLength = std::strlen(ROOTS[root].path);
    const std::string_view wanted(selected.data() + rootLength, std::strlen(selected.data()) - rootLength - 1);
    if (!folderEquivalent(wanted, {name.data(), nameLength}, equal)) return false;
    bool aliasMatch = false;
    if (alias[0] && !folderEquivalent(wanted, alias.data(), aliasMatch)) return false;
    equal = equal || aliasMatch;
    if (selectedAlias[0]) {
      if (!folderEquivalent(selectedAlias.data(), {name.data(), nameLength}, aliasMatch)) return false;
      equal = equal || aliasMatch;
      if (alias[0]) {
        if (!folderEquivalent(selectedAlias.data(), alias.data(), aliasMatch)) return false;
        equal = equal || aliasMatch;
      }
    }
    return true;
  }
  bool scan(bool verify) {
    if (!close() || !guard() || !Storage.openFileForReadReusing("COMPANION", "/", rootDirectory) ||
        !rootDirectory.isDirectory() || !rootEntry.prepareDirectoryEntry())
      return failure("root open");
    for (;;) {
      if (!guard() || !closeHandle(rootEntry)) return failure("root close/cancel");
      const auto next = rootDirectory.nextEntry(rootEntry);
      if (next == HalDirectoryResult::Error) return failure("root read");
      if (next == HalDirectoryResult::End) break;
      const auto length = rootEntry.getName(name.data(), name.size());
      if (!length || length >= name.size() || name[length]) return failure("root name");
      for (unsigned root = 0; root < 2; ++root) {
        const auto match = HalFileName::compare(ROOTS[root].name, {name.data(), length});
        if (match == FileNameComparison::Invalid) return failure("root UTF-8");
        if (match != FileNameComparison::Equal) continue;
        if (!rootEntry.isDirectory() || !closeHandle(directory)) return failure("root collision");
        const auto rootLength = std::strlen(ROOTS[root].path);
        std::copy_n(ROOTS[root].path, rootLength - 1, prefix.begin());
        prefix[rootLength - 1] = 0;
        if (!Storage.openFileForReadReusing("COMPANION", prefix.data(), directory) || !directory.isDirectory() ||
            !entry.prepareDirectoryEntry())
          return failure("folder parent open");
        for (;;) {
          if (!guard() || !closeHandle(entry)) return failure("folder close/cancel");
          const auto child = directory.nextEntry(entry);
          if (child == HalDirectoryResult::Error) return failure("folder read");
          if (child == HalDirectoryResult::End) break;
          if (!entry.isDirectory()) continue;
          const auto nameLength = entry.getName(name.data(), name.size());
          if (!nameLength || nameLength >= name.size() || name[nameLength]) return failure("folder name");
          if (name[0] == '.') continue;
          if (rootLength + nameLength + 1 >= prefix.size() ||
              HalFileName::compare({name.data(), nameLength}, {name.data(), nameLength}) != FileNameComparison::Equal ||
              !entry.getShortName(alias.data(), alias.size()) || strnlen(alias.data(), alias.size()) == alias.size())
            return failure("folder bounds/alias/UTF-8");
          if (verify) {
            bool equal = false;
            if (!isSelected(root, nameLength, equal) || (equal && ++unique > 1))
              return failure("folder alias collision");
          } else {
            std::copy_n(ROOTS[root].path, rootLength, prefix.begin());
            std::copy_n(name.begin(), nameLength, prefix.begin() + rootLength);
            prefix[rootLength + nameLength] = '/';
            prefix[rootLength + nameLength + 1] = 0;
            if (std::strcmp(prefix.data(), previous.data()) > 0 &&
                (!found || std::strcmp(prefix.data(), selected.data()) < 0)) {
              selected = prefix;
              selectedAlias = alias;
              selectedRoot = root;
              found = true;
            }
          }
          vTaskDelay(1);
        }
        if (!closeHandle(entry) || !closeHandle(directory)) return failure("folder parent close");
        break;
      }
    }
    return close() && guard();
  }
  bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary collection %s failed", reason);
    close();
    return false;
  }
  bool abort(const char* reason) {
    if (guard())
      writer.discard();
    else
      writer.releaseHandles();
    return failure(reason);
  }
};
}  // namespace companion
