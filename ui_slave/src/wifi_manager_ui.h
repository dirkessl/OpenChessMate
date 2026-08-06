/**
 * @file wifi_manager_ui.h
 * @brief WiFi + web-server + OTA orchestration for the ui_slave.
 *
 * Mirrors the master's WiFiManagerESP32, but tailored to the ui_slave:
 *  - no LittleFS-served pages (single embedded HTML page in PROGMEM)
 *  - no game/board endpoints (those still live on the master)
 *  - firmware-only OTA via OtaUpdaterUI
 *
 * Behaviour:
 *  - On begin(): try NVS-saved creds, then SECRET_SSID/SECRET_PASS, else
 *    start SoftAP "OpenChessUI-XXXX" (last 4 of MAC).
 *  - In either mode, starts an ESPAsyncWebServer on port 80.
 *  - update() (call from loop()): runs deferred work — async scan completion,
 *    pending credential save (reconnect outside the request handler), periodic
 *    auto-OTA check.
 *
 * Status changes are reported via a callback so the LVGL layer can update
 * its on-screen badge without a hard dependency on chess_ui.
 *
 * Guarded by #ifndef SIMULATOR so the SDL build stays network-free.
 */

#pragma once

#ifndef SIMULATOR

#include "ota_updater_ui.h"
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>

struct WiFiStatusUI {
  bool   connected = false;     // STA associated with IP
  bool   apMode    = false;     // SoftAP active (no STA)
  String ssid;                  // currently active SSID (STA or AP)
  String ip;                    // dotted IPv4
  int    rssi = 0;              // STA RSSI in dBm (0 in AP mode)
};

class WiFiManagerUI {
 public:
  using StatusCb = std::function<void(const WiFiStatusUI&)>;

  WiFiManagerUI();

  /// Bring up WiFi + web server. Non-blocking — returns immediately even
  /// while the STA association is still being negotiated.
  void begin();

  /// Drives deferred work. Call from loop() at least every ~100 ms.
  void update();

  /// Register a UI callback fired on every status transition (and once
  /// from begin()). Called from the loop() task only — safe for LVGL.
  void onStatusChanged(StatusCb cb) { statusCb = std::move(cb); }

  WiFiStatusUI getStatus() const { return status; }

  /// Persist new credentials to NVS and trigger a reconnect on the next
  /// update() tick (so the request handler can return first).
  void requestSaveCredentials(const String& ssid, const String& pass);

  /// Manually re-trigger an STA connect attempt (e.g., after the user
  /// edited creds on the touchscreen).
  void requestReconnect();

  /// Async WiFi scan results (cached). Call requestScan() to refresh.
  void requestScan();
  struct ScanEntry { String ssid; int rssi; bool secure; };
  const std::vector<ScanEntry>& getScanResults() const { return scanResults; }

  /// OTA passthroughs (used by both web endpoints and on-screen UI).
  OtaUpdateInfoUI checkOtaUpdate();
  void            applyOtaUpdate();          // apply lastOtaInfo
  bool            isAutoOtaEnabled() const { return autoOta; }
  void            setAutoOtaEnabled(bool e);
  OtaUpdateInfoUI lastOtaInfo;

 private:
  void setupServer();
  void connectSta();           // blocking up to ~10s
  void startAp();
  void publishStatus();        // recomputes `status` and fires callback
  String makeApSsid() const;

  // Endpoint handlers
  void handleStatus  (AsyncWebServerRequest* req);
  void handleScan    (AsyncWebServerRequest* req);
  void handleSave    (AsyncWebServerRequest* req);
  void handleOtaStat (AsyncWebServerRequest* req);
  void handleOtaSet  (AsyncWebServerRequest* req);
  void handleOtaApply(AsyncWebServerRequest* req);
  void handleRoot    (AsyncWebServerRequest* req);
  void onFirmwareUploadBody(AsyncWebServerRequest* req,
                            uint8_t* data, size_t len, size_t index, size_t total);

  AsyncWebServer        server;
  OtaUpdaterUI          ota;
  WiFiStatusUI          status;
  StatusCb              statusCb;

  String                stagedSsid, stagedPass; // saved by requestSave...
  bool                  hasStagedCreds = false;
  bool                  reconnectPending = false;
  bool                  scanInFlight   = false;
  std::vector<ScanEntry> scanResults;

  bool                  autoOta = false;
  unsigned long         lastOtaCheckMs = 0;
};

#endif // !SIMULATOR
