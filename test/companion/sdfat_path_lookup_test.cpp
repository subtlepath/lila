#include <cassert>
#include <string>

#include "lib/hal/HalInventoryPathLookup.h"
using namespace companion;
int main() {
  assert(HalFileName::compare("CAF\xC3\x89.epub", "caf\xC3\xA9.EPUB") == FileNameComparison::Equal);
  assert(HalFileName::compare("I.epub", "\xC4\xB1.epub") == FileNameComparison::Different);
  assert(HalFileName::compare("caf\xC3\xA9.epub", "cafe\xCC\x81.epub") == FileNameComparison::Different);
  assert(HalFileName::compare("a", "b\xC0\xAF") == FileNameComparison::Invalid);
  assert(HalFileName::compare("a\xC0\xAF", "b") == FileNameComparison::Invalid);
  assert(HalFileName::compare("\xF0\x90\x90\x80.EPUB", "\xF0\x90\x90\x80.epub") == FileNameComparison::Equal);
  assert(HalFileName::compare("\xF0\x90\x90\x80.epub", "\xF0\x90\x90\xA8.epub") == FileNameComparison::Different);
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.falseExists = true;
  state.directories["/"] = {};
  state.directories["/Books"] = {};
  state.files["/root.epub"] = {1, 2};
  state.files["/Books/CAF\xC3\x89.epub"] = {1, 2, 3};
  HalInventoryPathLookup lookup;
  uint64_t size = 999;
  assert(lookup.stat("/root.epub", size) == FileStatus::Present && size == 2);
  assert(lookup.stat("/Books/caf\xC3\xA9.EPUB", size) == FileStatus::Present && size == 3);
  state.aliases["/Books/CAF\xC3\x89.epub"] = "CAFE~1.EPU";
  assert(lookup.stat("/Books/cafe~1.epu", size) == FileStatus::Present && size == 3);
  state.failShortName = true;
  assert(lookup.stat("/Books/missing.epub", size) == FileStatus::Error && size == 3);
  state.failShortName = false;
  const auto wrappers = state.preparations;
  for (unsigned at = 0; at < 4; ++at) {
    assert(lookup.stat("/Books/missing.epub", size) == FileStatus::Missing && size == 3);
    assert(state.preparations == wrappers);
  }
  state.directoryErrorPath = "/Books";
  assert(lookup.stat("/Books/missing.epub", size) == FileStatus::Error && size == 3);
  state.directoryErrorPath.clear();
  state.failClose = true;
  assert(lookup.stat("/Books/caf\xC3\xA9.EPUB", size) == FileStatus::Error && size == 3);
  state.failClose = false;
  state.directories["/Books/folder.epub"] = {};
  assert(lookup.stat("/Books/folder.epub", size) == FileStatus::Error);
  assert(lookup.stat("/Absent/missing.epub", size) == FileStatus::Error);
  for (const auto path : {"/Books/../root.epub",
                          "/Books/\xC0\xAF"
                          "book.epub",
                          "/", "relative.epub"})
    assert(lookup.stat(path, size) == FileStatus::Error);
  state.files
      ["/Books/\xC0\xAF"
       "bad.epub"] = {1};
  assert(lookup.stat("/Books/missing.epub", size) == FileStatus::Error);
  state.files.erase(
      "/Books/\xC0\xAF"
      "bad.epub");
  unsigned progressCalls = 0;
  HalInventoryPathLookup cancelled([](void* ctx) { return ++*static_cast<unsigned*>(ctx) < 2; }, &progressCalls);
  assert(cancelled.stat("/root.epub", size) == FileStatus::Error && size == 3);
  progressCalls = 0;
  state.directories["/Empty"] = {};
  assert(cancelled.stat("/Empty/missing.epub", size) == FileStatus::Error && size == 3);
  for (unsigned at = 0; at < 100; ++at) state.files["/Books/book" + std::to_string(at) + ".epub"] = {1};
  const auto yields = state.yields;
  assert(lookup.stat("/Books/missing.epub", size) == FileStatus::Missing);
  assert(state.yields > yields);
}
