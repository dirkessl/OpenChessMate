/**
 * @file ota_updater_ui.h
 * @brief Firmware-only OTA updater for the ui_slave.
 *
 * Mirror of src/ota_updater.* on the master, simplified: no LittleFS web
 * assets, no tar parsing — the slave only flashes its own firmware partition
 * via Update.h. Both auto-update from a GitHub release and manual upload
 * (via wifi_manager_ui's web endpoint) funnel through this class.
 *
 * Guarded by #ifndef SIMULATOR — the desktop SDL build skips OTA entirely.
 */

#pragma once

#ifndef SIMULATOR

#include <Arduino.h>

class HTTPClient;
class Stream;

struct OtaUpdateInfoUI {
  bool   available    = false;
  String version;        // e.g. "1.2.3" (no leading 'v')
  String firmwareUrl;    // URL of OTA_FIRMWARE_ASSET_NAME in the release
};

class OtaUpdaterUI {
 public:
  OtaUpdaterUI() = default;

  /// Query GitHub for the latest release and populate an OtaUpdateInfoUI.
  /// Sets available=true only when the remote version is strictly newer
  /// than FIRMWARE_VERSION (or current is "dev").
  OtaUpdateInfoUI checkForUpdate();

  /// Download firmware from `url` and apply via Update.h. Reboots on success.
  bool applyFirmwareFromUrl(const String& url);

  /// Apply firmware bytes from an arbitrary stream of known length (used by
  /// the /ota/upload/firmware web endpoint). Reboots on success.
  bool applyFirmwareFromStream(Stream& stream, size_t contentLength);

  /// One-shot: check + (optionally) apply. Mirrors master OtaUpdater::autoUpdate.
  void autoUpdate(OtaUpdateInfoUI& info, bool apply);

  static const char* getCurrentVersion();

 private:
  static bool isNewerVersion(const String& current, const String& remote);
  static bool beginHttpGet(HTTPClient& http, const String& url, int timeoutMs = 10000);
};

#endif // !SIMULATOR
