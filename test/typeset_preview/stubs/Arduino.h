#pragma once

// Parser stubs (ESP, millis) plus the micros() FontDecompressor times itself with.
#include "../../chapter_html_slim_parser/stubs/Arduino.h"
#include "../../chapter_html_slim_parser/stubs/HalStorage.h"

inline uint32_t micros() { return 0; }
