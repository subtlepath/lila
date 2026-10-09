#!/usr/bin/env python3
"""Compile the native font installer against deterministic persistence faults."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "Logging.h": '#pragma once\n#define LOG_ERR(...) ((void)0)\n#define LOG_DBG(...) ((void)0)\n',
    "CrossPointSettings.h": r'''#pragma once
#include <cstring>
struct Settings {
  char sdFontFamilyName[32] = "Family";
  bool succeeds = true;
  unsigned saves = 0;
  bool saveToFile() { ++saves; return succeeds; }
  bool clearSdFontFamily() { sdFontFamilyName[0] = 0; return saveToFile(); }
};
inline Settings settings;
#define SETTINGS settings
''',
    "SdCardFontRegistry.h": r'''#pragma once
#include <cstdint>
inline constexpr unsigned CPFONT_MAGIC_LEN = 8;
struct SdCardFontRegistry {
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";
  static const char* findFamilyRoot(const char*) { return FONTS_DIR_HIDDEN; }
  static const char* defaultWriteRoot() { return FONTS_DIR_HIDDEN; }
  void discover() {}
  const void* findFamily(const char*) const { return nullptr; }
};
''',
    "HalStorage.h": r'''#pragma once
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
struct HalFile {
  size_t read(uint8_t*, size_t) { return 0; }
  bool close() { return true; }
};
struct FakeStorage {
  std::set<std::string> directories {"/.fonts/Family", "/fonts/Family"};
  unsigned removals = 0;
  unsigned failAt = 0;
  bool failAfterEffect = false;
  bool exists(const char* path) { return directories.contains(path); }
  bool mkdir(const char* path) { directories.insert(path); return true; }
  bool openFileForRead(const char*, const char*, HalFile&) { return false; }
  bool removeDir(const char* path) {
    ++removals;
    if (removals == failAt && !failAfterEffect) return false;
    directories.erase(path); return removals != failAt;
  }
};
inline FakeStorage storage;
#define Storage storage
''',
    "main.cpp": r'''#include "FontInstaller.h"
#include "CrossPointSettings.h"
#include "HalStorage.h"
#include <cassert>
int main() {
  SdCardFontRegistry registry;
  FontInstaller installer(registry);
  using Error = FontInstaller::Error;
  settings.succeeds = false;
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(settings.sdFontFamilyName[0] == 0);
  assert(storage.removals == 0 && storage.directories.size() == 2);
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(settings.saves == 2 && storage.removals == 0);
  settings.succeeds = true;
  storage.failAt = 2;
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(settings.saves == 3 && storage.directories.size() == 1);
  settings.succeeds = false;
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(storage.removals == 2 && storage.directories.size() == 1);
  settings.succeeds = true;
  assert(installer.deleteFamily("Family") == Error::OK);
  assert(storage.directories.empty());
  assert(installer.deleteFamily("Family") == Error::OK);
  const auto saves = settings.saves;
  assert(installer.deleteFamily("../Family") == Error::INVALID_FAMILY_NAME);
  assert(settings.saves == saves);
  std::string oversized(160, 'A');
  assert(installer.deleteFamily(oversized.c_str()) == Error::INVALID_FAMILY_NAME);
  assert(settings.saves == saves);
  storage = FakeStorage{};
  settings = Settings{};
  assert(installer.deleteFamily(settings.sdFontFamilyName) == Error::OK);
  assert(settings.sdFontFamilyName[0] == 0 && storage.directories.empty());
  assert(storage.removals == 2);
  storage = FakeStorage{};
  settings = Settings{};
  storage.failAt = 1; storage.failAfterEffect = true;
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(!storage.directories.contains("/.fonts/Family"));
  assert(storage.directories.contains("/fonts/Family"));
  settings.succeeds = false;
  assert(installer.deleteFamily("Family") == Error::SD_WRITE_ERROR);
  assert(storage.removals == 1);
  settings.succeeds = true;
  assert(installer.deleteFamily("Family") == Error::OK);
  assert(storage.directories.empty());
  storage = FakeStorage{};
  settings = Settings{};
  std::strcpy(settings.sdFontFamilyName, "Other");
  assert(installer.deleteFamily("Family") == Error::OK);
  assert(std::strcmp(settings.sdFontFamilyName, "Other") == 0);
  const std::string longest(151, 'A');
  storage.directories = {"/.fonts/" + longest, "/fonts/" + longest, "/fonts/Unrelated"};
  assert(installer.deleteFamily(longest.c_str()) == Error::OK);
  assert(storage.directories == std::set<std::string>{"/fonts/Unrelated"});
  const auto boundarySaves = settings.saves;
  const auto boundaryRemovals = storage.removals;
  assert(installer.deleteFamily((longest + "A").c_str()) == Error::INVALID_FAMILY_NAME);
  assert(settings.saves == boundarySaves && storage.removals == boundaryRemovals);
}
''',
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", default="c++")
arguments = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="lila-font-installer-") as temporary:
    directory = Path(temporary)
    # Preserve the implementation verbatim; quoted includes resolve to fake peers.
    for name in ["FontInstaller.cpp", "FontInstaller.h"]:
        shutil.copy2(ROOT / "src" / name, directory / name)
    for name, contents in STUBS.items():
        (directory / name).write_text(contents)
    binary = directory / "fault-check"
    subprocess.run([arguments.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(directory), str(directory / "FontInstaller.cpp"),
                    str(directory / "main.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Font deletion persistence, retry, partial deletion, and invalid-path checks pass.")
