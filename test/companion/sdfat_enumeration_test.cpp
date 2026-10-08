// Host-only sparse images exercise the patched upstream iterator implementation.
#include <FsLib/FsLib.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>

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

int main() {
  exercise(false);
  exercise(true);
  std::puts("FAT/exFAT: EOF, entries, missing named lookup, read failure and corrupt entry passed");
}
