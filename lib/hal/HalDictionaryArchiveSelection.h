#pragma once

#include "CompanionDictionaryZipSelection.h"
#include "CompanionZipArchiveValidation.h"
#include "CompanionZipNormalizedNamesValidation.h"
#include "HalZipNameBytesStorage.h"
#include "HalZipRangeStorage.h"

namespace companion {
// Session-owned outside the task stack. The source is exclusively owned; scratch
// banks are disjoint. Name workspace may borrow the decoder window between reads.
class HalDictionaryArchiveSelection final {
 public:
  HalDictionaryArchiveSelection(InventoryIndexStorage& source, std::span<uint8_t> archiveScratch,
                                std::span<uint8_t> comparisonScratch, tinfl_decompressor* decoder,
                                std::span<uint8_t> decoderWindow, ZipNameWorkspace nameWorkspace,
                                ZipArchiveValidation::Progress progress = nullptr, void* context = nullptr)
      : ranges(rangeStorage, progress, context),
        duplicates(nameIndex, nameBytes, comparisonScratch, progress, context),
        names(source, duplicates, nameWorkspace, progress, context, visit, &selection),
        archive(source, archiveScratch, decoder, decoderWindow, progress, context) {}
  bool select(DictionaryZipMembers& output) {
    selection.begin();
    if (!archive.validate(layout, &ranges, &names) || !selection.finishHeaders() ||
        !archive.validate(layout, &ranges, &names) || !selection.finishMembers(members)) {
      LOG_ERR("COMPANION", "Dictionary archive selection failed");
      return false;
    }
    // Scratch indexes are disposable, but cleanup failure retains ownership.
    if (!rangeStorage.discard() || !nameIndex.discard() || !nameBytes.discard()) {
      LOG_ERR("COMPANION", "Dictionary archive selection cleanup failed");
      return false;
    }
    output = members;
    return true;
  }

 private:
  HalZipRangeStorage rangeStorage;
  HalZipRangeStorage nameIndex{HalZipRangeStorage::Purpose::NameIndex};
  HalZipNameBytesStorage nameBytes;
  ZipRangeValidation ranges;
  ZipNameDuplicateValidation duplicates;
  DictionaryZipSelection selection;
  ZipNormalizedNamesValidation names;
  ZipArchiveValidation archive;
  ZipDirectoryLayout layout;
  DictionaryZipMembers members;
  static bool visit(void* context, std::span<const uint8_t> name, const ZipEntryMetadata& entry) {
    return static_cast<DictionaryZipSelection*>(context)->add(name, entry);
  }
};
}  // namespace companion
