#pragma once

#include "CompanionDictionaryCachePublication.h"
#include "CompanionDictionaryJournalPaths.h"

namespace companion {
inline constexpr const char* DICTIONARY_INSTALLATION_TEMPORARIES[] = {
    DICTIONARY_ZIP_AUDIT_JOURNALS[0],
    DICTIONARY_ZIP_AUDIT_JOURNALS[1],
    DICTIONARY_EXTRACTION_JOURNALS[0],
    DICTIONARY_EXTRACTION_JOURNALS[1],
    DICTIONARY_INSTALLATION_JOURNALS[0],
    DICTIONARY_INSTALLATION_JOURNALS[1],
    TRANSFER_STAGE,
    TRANSFER_BACKUP,
    DICTIONARY_MEMBER_CANDIDATE,
    DICTIONARY_CACHE_CANDIDATE,
    "/.crosspoint/companion/zip-ranges-next",
    "/.crosspoint/companion/zip-names-index-next",
    "/.crosspoint/companion/zip-names-bytes-next",
    "/.crosspoint/companion/dictionary-definitions-next",
    "/.crosspoint/companion/dictionary-index-next",
    "/.crosspoint/companion/dictionary-info-next",
    "/.crosspoint/companion/dictionary-synonyms-next"};
}  // namespace companion
