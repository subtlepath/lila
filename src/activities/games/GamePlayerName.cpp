#include "GamePlayerName.h"

#include <Logging.h>
#include <TableWire.h>
#include <esp_mac.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"

void gamePlayerName(char* out, const size_t size) {
  if (!out || size == 0) return;
  if (SETTINGS.gamesPlayerName[0] != '\0') {
    snprintf(out, size, "%s", SETTINGS.gamesPlayerName);
    return;
  }
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) LOG_ERR("GAME", "cannot read MAC for player name");
  snprintf(out, size, "Reader %02X%02X", mac[4], mac[5]);
}

bool setGamePlayerName(const char* name) {
  char trimmed[sizeof(SETTINGS.gamesPlayerName)] = {};
  if (name) {
    while (*name == ' ') name++;
    const size_t n = table::utf8Prefix(name, table::NAME_LEN);
    memcpy(trimmed, name, n);
    for (size_t end = n; end > 0 && trimmed[end - 1] == ' '; end--) trimmed[end - 1] = '\0';
  }
  if (strcmp(trimmed, SETTINGS.gamesPlayerName) == 0) return false;
  memcpy(SETTINGS.gamesPlayerName, trimmed, sizeof(trimmed));
  SETTINGS.saveToFile();
  return true;
}
