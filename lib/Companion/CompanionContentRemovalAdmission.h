#pragma once

#include "CompanionContentRemoval.h"

namespace companion {
enum class EpubRemovalAdmissionResult {
  Ready,
  Retired,
  Invalid,
  WrongStorage,
  Busy,
  NotFound,
  Conflict,
  Corrupt,
  IoError
};
class ContentRemovalAdmission {
 public:
  virtual ~ContentRemovalAdmission() = default;
  virtual bool supports(ContentKind kind) const = 0;
  virtual EpubRemovalAdmissionResult admit(const ContentRemovalRequest&, uint64_t revision, ContentRemovalRecord&) = 0;
};
}  // namespace companion
