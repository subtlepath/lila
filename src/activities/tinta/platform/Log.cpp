#if LILA_TINTA

#include "platform/Log.h"

#include <Logging.h>
#include <stdarg.h>
#include <stdio.h>

namespace tinta::platform {

void log(const char* format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  LOG_INF("TNT", "%s", line);
}

}  // namespace tinta::platform

#endif  // LILA_TINTA
