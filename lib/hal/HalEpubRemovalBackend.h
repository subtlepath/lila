#pragma once

#include "CompanionContentRemovalHandler.h"
#include "HalContentRemovalTransactions.h"
#include "HalEpubRemovalAdmission.h"

namespace companion {
// Serialized session owner. The caller suspends transfer, reader and metadata writers.
class HalEpubRemovalBackend final : public ContentRemovalBackend {
 public:
  using Callback = bool (*)(void*);
  HalEpubRemovalBackend(HalEpubRemovalAdmission& admission, HalContentRemovalTransactions& transactions,
                        const uint64_t& revision, Callback permitted, Callback refresh, void* context)
      : admission(admission),
        transactions(transactions),
        revision(revision),
        permitted(permitted),
        refresh(refresh),
        context(context) {}
  ContentRemovalResult completed(const ContentRemovalRequest& request) override {
    if (!allowed()) return ContentRemovalResult::Busy;
    const auto result = transactions.lookup(request, record);
    switch (result) {
      case CompletedRemovalResult::Ok:
        return refreshed();
      case CompletedRemovalResult::Missing:
        return ContentRemovalResult::NotFound;
      case CompletedRemovalResult::Invalid:
        return ContentRemovalResult::Invalid;
      case CompletedRemovalResult::Conflict:
        return ContentRemovalResult::Conflict;
      case CompletedRemovalResult::Corrupt:
        return ContentRemovalResult::Corrupt;
      case CompletedRemovalResult::IoError:
        return ContentRemovalResult::IoError;
    }
    return ContentRemovalResult::IoError;
  }
  ContentRemovalResult remove(const ContentRemovalRequest& request) override {
    if (!allowed()) return ContentRemovalResult::Busy;
    if (request.manifest.kind != ContentKind::Epub) return ContentRemovalResult::Unsupported;
    const auto prepared = transactions.prepare(request);
    if (prepared != ContentRemovalJournalResult::Ok) {
      if (prepared == ContentRemovalJournalResult::Conflict) return ContentRemovalResult::Busy;
      if (prepared == ContentRemovalJournalResult::Corrupt) return ContentRemovalResult::Corrupt;
      if (prepared == ContentRemovalJournalResult::Invalid) return ContentRemovalResult::Invalid;
      return ContentRemovalResult::IoError;
    }
    const auto admitted = admission.admit(request, revision, record);
    switch (admitted) {
      case EpubRemovalAdmissionResult::Ready:
      case EpubRemovalAdmissionResult::Retired:
        break;
      case EpubRemovalAdmissionResult::Invalid:
        return ContentRemovalResult::Invalid;
      case EpubRemovalAdmissionResult::WrongStorage:
        return ContentRemovalResult::WrongStorage;
      case EpubRemovalAdmissionResult::Busy:
        return ContentRemovalResult::Busy;
      case EpubRemovalAdmissionResult::NotFound:
        return ContentRemovalResult::NotFound;
      case EpubRemovalAdmissionResult::Conflict:
        return ContentRemovalResult::Conflict;
      case EpubRemovalAdmissionResult::Corrupt:
        return ContentRemovalResult::Corrupt;
      case EpubRemovalAdmissionResult::IoError:
        return ContentRemovalResult::IoError;
    }
    if (!allowed()) return ContentRemovalResult::Busy;
    switch (transactions.remove(record)) {
      case ContentRemovalJournalResult::Ok:
        return refreshed();
      case ContentRemovalJournalResult::Invalid:
        return ContentRemovalResult::Invalid;
      case ContentRemovalJournalResult::Missing:
        return ContentRemovalResult::NotFound;
      case ContentRemovalJournalResult::Conflict:
        return ContentRemovalResult::Conflict;
      case ContentRemovalJournalResult::Corrupt:
        return ContentRemovalResult::Corrupt;
      case ContentRemovalJournalResult::IoError:
        return ContentRemovalResult::IoError;
    }
    return ContentRemovalResult::IoError;
  }

 private:
  HalEpubRemovalAdmission& admission;
  HalContentRemovalTransactions& transactions;
  const uint64_t& revision;
  Callback permitted, refresh;
  void* context;
  ContentRemovalRecord record;
  bool allowed() const { return permitted && permitted(context); }
  ContentRemovalResult refreshed() {
    if (allowed() && refresh && refresh(context) && allowed()) return ContentRemovalResult::Ok;
    LOG_ERR("COMPANION", "Removal completed but reader/inventory refresh failed");
    return ContentRemovalResult::IoError;
  }
};
}  // namespace companion
