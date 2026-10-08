#pragma once
#include <cstdint>
#include <span>
namespace companion {
// Run after mounting storage and before opening reading or study activities.
bool recoverAtStartup(bool (*firmwareValidator)(const char*) = nullptr,
                      bool (*runningDigest)(std::span<uint8_t>, std::span<uint8_t>) = nullptr);
}  // namespace companion
