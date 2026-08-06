/**
 * @file main.cpp
 * @brief OpenChessMate UI Slave — VIEWE 4.3" ESP32-S3 display (portrait 480×800)
 *
 * Platform layer: initialises the display panel (ESP32_Display_Panel),
 * sets up LVGL via lvgl_v8_port (sw_rotate for portrait), creates the
 * shared chess UI, and polls serial for protocol messages from the master.
 *
 * Hardware:
 *   Board:   VIEWE UEDX80480043E-WB-A (ESP32-S3-N16R8)
 *   Display: 800×480 RGB IPS (ST7262E43-G4) — rotated to 480×800 portrait
 *   Touch:   GT911 capacitive (I2C)
 *   Serial:  UART0 on IO43/IO44 (CH340C USB-UART + board UART header)
 *
 * This codebase was developed with the assistance of Claude Sonnet 4.6
 * (Anthropic) via GitHub Copilot.
 */

#include "chess_ui.h"
#include "lvgl_v8_port.h"
#include "wifi_manager_ui.h"
#include "wifi_ui.h"
#include "version.h"
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <lvgl.h>
#include <vector>

// ---------------------------------------------------------------------------
// Serial protocol — line-buffered reader
// ---------------------------------------------------------------------------
static String s_rx_buf;

/// Send a protocol message to the master board via Serial (UART0).
static void platformSend(const char* msg) {
  Serial.print(msg);
}

// ---------------------------------------------------------------------------
// Backlight brightness
// ---------------------------------------------------------------------------
static esp_panel::drivers::Backlight* s_backlight = nullptr;

/// Set display brightness (0–100 percent).
static void platformSetBrightness(int percent) {
  if (s_backlight) s_backlight->setBrightness(percent);
}

// ---------------------------------------------------------------------------
// WiFi + OTA (standalone — see wifi_manager_ui.{h,cpp})
// ---------------------------------------------------------------------------
static WiFiManagerUI s_wifi;

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  // UART0 on IO43(TX) / IO44(RX) — shared between CH340C USB-C and UART header
  Serial.begin(115200);
  delay(200);
  Serial.println("UI Slave (VIEWE 4.3\") starting...");

  // ---- Display panel init (LCD + touch + backlight) ----
  ESP_Panel* panel = new ESP_Panel();
  panel->init();
  panel->begin();
  s_backlight = panel->getBacklight();
  Serial.println("Display panel initialised");

  // ---- LVGL port (buffers, flush, touch, tick, FreeRTOS task) ----
  lvgl_port_init(panel->getLcd(), panel->getTouch());
  Serial.println("LVGL port initialised");

  // ---- Chess UI (portrait 480×800, runs under LVGL mutex) ----
  lvgl_port_lock(0);
  chess_ui_create(480, 800, &lv_font_montserrat_14, platformSend, platformSetBrightness);
  lvgl_port_unlock();

  // ---- WiFi + web server + OTA (standalone, separate from master) ----
  s_wifi.onStatusChanged([](const WiFiStatusUI& s) {
    lvgl_port_lock(0);
    chess_ui_set_wifi_status(s.connected || s.apMode, s.ssid.c_str(), s.ip.c_str());
    wifi_ui_set_status(s.connected, s.apMode, s.ssid.c_str(), s.ip.c_str());
    lvgl_port_unlock();
  });
  s_wifi.begin();
  Serial.println("WiFi + web server initialised");

  // ---- WiFi & OTA on-screen panel (LVGL) ----
  lvgl_port_lock(0);
  wifi_ui_hooks hooks{};
  hooks.user         = &s_wifi;
  hooks.scan_request = [](void* u) { static_cast<WiFiManagerUI*>(u)->requestScan(); };
  hooks.save_creds   = [](void* u, const char* ssid, const char* pass) {
    static_cast<WiFiManagerUI*>(u)->requestSaveCredentials(String(ssid), String(pass));
  };
  hooks.ota_check    = [](void* u) {
    auto* w = static_cast<WiFiManagerUI*>(u);
    auto info = w->checkOtaUpdate();
    lvgl_port_lock(0);
    wifi_ui_set_ota_status(FIRMWARE_VERSION, info.available, info.version.c_str(),
                           w->isAutoOtaEnabled());
    lvgl_port_unlock();
  };
  hooks.ota_apply    = [](void* u) { static_cast<WiFiManagerUI*>(u)->applyOtaUpdate(); };
  hooks.ota_set_auto = [](void* u, bool e) { static_cast<WiFiManagerUI*>(u)->setAutoOtaEnabled(e); };
  wifi_ui_init(lv_scr_act(), 480, 800, hooks);
  wifi_ui_set_ota_status(FIRMWARE_VERSION, false, "", s_wifi.isAutoOtaEnabled());
  chess_ui_set_wifi_button_handler([](void*) { wifi_ui_show(); }, nullptr);
  lvgl_port_unlock();

  Serial.println("UI ready");
}

void loop() {
  // Poll serial for newline-terminated messages from the master
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (s_rx_buf.length() > 0) {
        lvgl_port_lock(0);
        chess_ui_handle_message(s_rx_buf.c_str());
        lvgl_port_unlock();
        s_rx_buf = "";
      }
    } else {
      s_rx_buf += c;
      if (s_rx_buf.length() > 1024)
        s_rx_buf = s_rx_buf.substring(s_rx_buf.length() - 1024);
    }
  }
  s_wifi.update();

  // Push WiFi scan results to the on-screen panel when they refresh.
  static size_t s_last_scan_n = (size_t)-1;
  const auto& scans = s_wifi.getScanResults();
  if (scans.size() != s_last_scan_n) {
    s_last_scan_n = scans.size();
    std::vector<const char*> ssids;
    std::vector<int>         rssis;
    ssids.reserve(scans.size());
    rssis.reserve(scans.size());
    for (const auto& e : scans) { ssids.push_back(e.ssid.c_str()); rssis.push_back(e.rssi); }
    lvgl_port_lock(0);
    wifi_ui_set_scan_results(ssids.data(), rssis.data(), (int)ssids.size());
    lvgl_port_unlock();
  }

  delay(5);
}
