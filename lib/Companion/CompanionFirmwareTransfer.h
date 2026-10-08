#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {
inline constexpr char FIRMWARE_STAGE_DESTINATION[] = "/Companion/firmware.bin";
inline constexpr uint32_t FIRMWARE_STAGE_FORMAT = 1;
inline bool validFirmwareStageManifest(const ContentManifest& manifest) {
  return manifest.kind == ContentKind::Firmware && manifest.formatVersion == FIRMWARE_STAGE_FORMAT &&
         manifest.length >= 65536 && manifest.length <= UINT32_MAX &&
         std::all_of(manifest.logicalIdentity.begin(), manifest.logicalIdentity.end(),
                     [](uint8_t byte) { return byte == 0; });
}
}  // namespace companion
