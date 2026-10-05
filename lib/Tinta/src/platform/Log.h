#pragma once

namespace tinta::platform {

// One line on the serial console (USB CDC on the device), prefixed "[tinta] ".
// The simulator tests match these lines, so their wording is an interface.
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// True in simulator bundles (tools/sim/build.sh defines TINTA_SIM), where the
// UI also logs what flows need to drive it, such as touch targets.
#if TINTA_SIM
inline constexpr bool kSimulator = true;
#else
inline constexpr bool kSimulator = false;
#endif

}  // namespace tinta::platform
