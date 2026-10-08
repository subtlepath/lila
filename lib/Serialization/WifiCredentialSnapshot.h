#pragma once

#include <algorithm>
#include <span>
#include <string_view>

// Sources must be disjoint from both output buffers.
inline bool copyWifiCredentialSnapshot(std::string_view sourceSsid, std::string_view sourcePassword,
                                       std::span<char> ssid, std::span<char> password) {
  std::fill(ssid.begin(), ssid.end(), 0);
  volatile char* secret = password.data();
  for (size_t at = 0; at < password.size(); ++at) secret[at] = 0;
  if (sourceSsid.empty() || sourceSsid.size() > 32 || sourcePassword.size() > 64 || sourceSsid.size() >= ssid.size() ||
      sourcePassword.size() >= password.size() || sourceSsid.find('\0') != std::string_view::npos ||
      sourcePassword.find('\0') != std::string_view::npos)
    return false;
  std::copy(sourceSsid.begin(), sourceSsid.end(), ssid.begin());
  std::copy(sourcePassword.begin(), sourcePassword.end(), password.begin());
  return true;
}
