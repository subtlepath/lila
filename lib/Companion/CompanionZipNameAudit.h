#pragma once

#include "CompanionZipEntryMetadataValidation.h"

namespace companion {
class ZipNameAudit {
 public:
  virtual ~ZipNameAudit() = default;
  virtual bool begin() = 0;
  virtual bool add(const ZipEntryMetadata& entry) = 0;
  virtual bool finish() = 0;
  virtual void abort() = 0;
};
}  // namespace companion
