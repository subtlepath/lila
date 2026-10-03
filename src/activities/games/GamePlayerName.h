#pragma once

#include <cstddef>

// The name this reader shows at a Games table: the one set on the Games
// screen, or "Reader XXXX" from the tail of the WiFi MAC (read from eFuse, so
// the radio does not need to be up). Always null-terminated and at most
// table::NAME_LEN bytes of whole UTF-8 characters.
void gamePlayerName(char* out, size_t size);

// Stores a new name (trimmed and cut to whole characters). Empty restores the
// default. Returns true when the setting changed and was saved.
bool setGamePlayerName(const char* name);
