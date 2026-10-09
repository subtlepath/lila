// Host-only sparse images exercise the patched upstream iterator implementation.
#include <FsLib/FsLib.h>
#include <common/FsUtf.h>
#include <common/upcase.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../lib/hal/HalFilenameCodec.h"

class Image final : public FsBlockDeviceInterface {
 public:
  FILE* file = std::tmpfile();
  bool failReads = false;
  Sector_t lastRead = 0;
  Image() {
    assert(file);
    assert(ftruncate(fileno(file), static_cast<off_t>(sectorCount()) * 512) == 0);
  }
  ~Image() override { std::fclose(file); }
  bool isBusy() override { return false; }
  Sector_t sectorCount() override { return 0x100000; }
  bool syncDevice() override { return std::fflush(file) == 0; }
  bool readSector(Sector_t sector, uint8_t* out) override {
    if (failReads || sector >= sectorCount()) return false;
    lastRead = sector;
    return fseeko(file, static_cast<off_t>(sector) * 512, SEEK_SET) == 0 && std::fread(out, 1, 512, file) == 512;
  }
  bool writeSector(Sector_t sector, const uint8_t* bytes) override {
    return sector < sectorCount() && fseeko(file, static_cast<off_t>(sector) * 512, SEEK_SET) == 0 &&
           std::fwrite(bytes, 1, 512, file) == 512;
  }
  bool readSectors(Sector_t sector, uint8_t* out, size_t count) override {
    for (size_t i = 0; i < count; ++i)
      if (!readSector(sector + i, out + 512 * i)) return false;
    return true;
  }
  bool writeSectors(Sector_t sector, const uint8_t* bytes, size_t count) override {
    for (size_t i = 0; i < count; ++i)
      if (!writeSector(sector + i, bytes + 512 * i)) return false;
    return true;
  }
};

static void exercise(bool exfat) {
  Image image;
  uint8_t block[512]{};
  if (exfat) {
    ExFatFormatter formatter;
    assert(formatter.format(&image, block));
  } else {
    FatFormatter formatter;
    assert(formatter.format(&image, block));
  }
  {
    FsVolume volume;
    assert(volume.begin(&image));
    FsFile directory;
    assert(directory.openRoot(&volume));
    FsFile entry;
    assert(!entry.openNext(&directory));
    assert(directory.getError() == 0);
    FsFile created;
    assert(created.open(&volume, "/a long course pack filename.pack", O_CREAT | O_WRONLY));
    assert(created.close());
    directory.rewind();
    assert(entry.openNext(&directory));
    char alias[13];
    assert(entry.getShortName(alias, sizeof alias));
    assert(exfat ? alias[0] == 0 : alias[0] != 0);
    assert(!entry.getShortName(alias, sizeof alias - 1));
    if (!exfat) {
      FsFile byAlias;
      assert(byAlias.open(&directory, alias, O_RDONLY));
      assert(byAlias.dirIndex() == entry.dirIndex());
    }
    assert(entry.close());
    assert(!entry.getShortName(alias, sizeof alias));
    assert(!entry.openNext(&directory));
    assert(directory.getError() == 0);
    FsFile missing;
    assert(!missing.open(&directory, "missing.pack", O_RDONLY));
    assert(directory.getError() == 0);
    assert(directory.seekSet(512));
    image.failReads = true;
    assert(!entry.openNext(&directory));
    assert((directory.getError() & 2) != 0);
    image.failReads = false;
  }
  {
    FsVolume volume;
    assert(volume.begin(&image));
    FsFile directory;
    assert(directory.openRoot(&volume));
    assert(directory.read(block, sizeof block) == sizeof block);
    const Sector_t sector = image.lastRead;
    bool corrupted = false;
    for (size_t offset = 0; offset < sizeof block; offset += 32) {
      if (exfat && block[offset] == 0xc0) {
        block[offset] = 0;  // End marker inside a file entry set.
        corrupted = true;
        break;
      }
      if (!exfat && block[offset + 11] == 0x0f) {
        block[offset + 13] ^= 1;  // Long-name checksum no longer matches.
        corrupted = true;
        break;
      }
    }
    assert(corrupted);
    assert(image.writeSector(sector, block));
    assert(image.syncDevice());
  }
  {
    FsVolume volume;
    assert(volume.begin(&image));
    FsFile directory;
    assert(directory.openRoot(&volume));
    FsFile entry;
    assert(!entry.openNext(&directory));
    assert((directory.getError() & 2) != 0);
  }
}

static void exerciseUnicode(bool exfat) {
  Image image;
  uint8_t block[512]{};
  if (exfat) {
    ExFatFormatter formatter;
    assert(formatter.format(&image, block));
  } else {
    FatFormatter formatter;
    assert(formatter.format(&image, block));
  }
  FsVolume volume;
  assert(volume.begin(&image));
  const char* const names[][2] = {
      {"/caf\xc3\xa9.json", "/CAF\xc3\x89.JSON"},
      {"/\xc3\xa4-book.json", "/\xc3\x84-BOOK.JSON"},
      {"/\xf0\x9f\x93\x96-book.json", "/\xf0\x9f\x93\x96-BOOK.JSON"},
  };
  for (const auto& namesForFile : names) {
    const auto fold = +[](uint32_t cp) -> uint32_t { return cp <= 0xffff ? toUpcase(cp) : cp; };
    assert(hal_filename::compare(namesForFile[0], namesForFile[1], fold) == hal_filename::Comparison::Equal);
    FsFile created;
    assert(created.open(&volume, namesForFile[0], O_CREAT | O_EXCL | O_WRONLY));
    const auto index = created.dirIndex();
    assert(created.write("retained", 8) == 8);
    assert(created.close());
    FsFile alternate;
    assert(alternate.open(&volume, namesForFile[1], O_RDONLY));
    assert(alternate.dirIndex() == index);
    assert(alternate.fileSize() == 8);
    assert(alternate.close());
    // Exclusive creation must reject the case variant rather than replace it.
    assert(!alternate.open(&volume, namesForFile[1], O_CREAT | O_EXCL | O_WRONLY));
  }
  // SdFat compares UTF-16 units; it does not normalize composed/decomposed text.
  FsFile decomposed;
  assert(decomposed.open(&volume, "/cafe\xcc\x81.json", O_CREAT | O_EXCL | O_WRONLY));
  assert(decomposed.close());
}

static void exerciseFilenameComparison() {
  const auto fold = +[](uint32_t cp) -> uint32_t { return cp <= 0xffff ? toUpcase(cp) : cp; };
  for (uint32_t unit = 0xd800; unit <= 0xdfff; ++unit) assert(toUpcase(unit) == unit);
  assert(hal_filename::compare("caf\xc3\xa9", "cafe\xcc\x81", fold) == hal_filename::Comparison::Different);
  assert(hal_filename::compare("\xc4\xb1", "I", fold) == hal_filename::Comparison::Different);
  assert(hal_filename::compare("\xc5\xbf", "S", fold) == hal_filename::Comparison::Different);
  assert(hal_filename::compare("\xc9\xab", "\xe2\xb1\xa2", fold) == hal_filename::Comparison::Equal);
  const auto folded = [&](std::string_view value) {
    std::string result;
    assert(hal_filename::writeFolded(
        value, fold,
        +[](void* context, const uint8_t* bytes, size_t size) {
          assert(size <= 4);
          static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(bytes), size);
          return true;
        },
        &result));
    return result;
  };
  assert(folded("/.crosspoint/bookmarks/BOOK.json") == "/.crosspoint/bookmarks/book.json");
  assert(folded("caf\xc3\xa9-\xf0\x9f\x93\x96.JSON") == folded("CAF\xc3\x89-\xf0\x9f\x93\x96.json"));
  assert(folded("caf\xc3\xa9") != folded("cafe\xcc\x81"));
  assert(folded("\xc9\xab") == folded("\xe2\xb1\xa2"));
  // Every valid BMP scalar follows the installed table, including length changes.
  for (uint32_t cp = 1; cp <= 0xffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) continue;
    char original[4], uppercase[4];
    const auto originalEnd = FsUtf::cpToMb(cp, original, original + sizeof(original));
    const auto uppercaseEnd = FsUtf::cpToMb(toUpcase(cp), uppercase, uppercase + sizeof(uppercase));
    assert(originalEnd && uppercaseEnd);
    const std::string_view a(original, originalEnd - original), b(uppercase, uppercaseEnd - uppercase);
    assert(hal_filename::compare(a, b, fold) == hal_filename::Comparison::Equal);
    assert(folded(a) == folded(b));
  }
  for (const char* invalid : {"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc3"}) {
    assert(hal_filename::compare(invalid, "a", fold) == hal_filename::Comparison::Invalid);
    assert(hal_filename::compare("a", invalid, fold) == hal_filename::Comparison::Invalid);
  }
  assert(hal_filename::compare(std::string_view("a\0b", 3), "a", fold) == hal_filename::Comparison::Invalid);
  assert(hal_filename::compare("x\xc3", "y", fold) == hal_filename::Comparison::Invalid);
  assert(hal_filename::compare("", "", fold) == hal_filename::Comparison::Equal);
  assert(hal_filename::compare("a", "", fold) == hal_filename::Comparison::Different);
}

int main() {
  exerciseFilenameComparison();
  exercise(false);
  exercise(true);
  exerciseUnicode(false);
  exerciseUnicode(true);
  std::puts("FAT/exFAT: enumeration failures, Unicode case aliases and distinct normalization passed");
}
