/**
 * @file wifi_ui.h
 * @brief LVGL screen for WiFi credentials entry + OTA controls.
 *
 * Full-screen overlay, hidden by default. Toggled visible by tapping the
 * "WiFi & Updates" button on the chess_ui settings screen (see
 * chess_ui_set_wifi_button_handler()).
 *
 * The screen has zero direct dependency on `wifi_manager_ui` so it stays
 * buildable from the SDL simulator. Callbacks for scan / save / OTA are
 * passed in via wifi_ui_init() — main.cpp wires them on the device build.
 */

#pragma once
#include <lvgl.h>

struct wifi_ui_hooks {
  void (*scan_request)(void* user);
  void (*save_creds)(void* user, const char* ssid, const char* pass);
  void (*ota_check)(void* user);
  void (*ota_apply)(void* user);
  void (*ota_set_auto)(void* user, bool enabled);
  void* user;
};

/// Build the full-screen LVGL panel as a child of `parent`. Hooks may be NULL
/// (sim build) — buttons that have no hook just log a debug message.
void wifi_ui_init(lv_obj_t* parent, int screen_w, int screen_h,
                  const wifi_ui_hooks& hooks);

/// Bring the panel to the front and show it. Idempotent.
void wifi_ui_show(void);

/// Hide the panel (returns to whatever was previously visible).
void wifi_ui_hide(void);

/// Refresh the connection state displayed in the panel header.
/// Called by the WiFi manager from main.cpp on every status transition.
void wifi_ui_set_status(bool online, bool ap_mode,
                        const char* ssid, const char* ip);

/// Replace the contents of the SSID dropdown with new scan results.
void wifi_ui_set_scan_results(const char* const* ssids, const int* rssis,
                              int count);

/// Update the OTA panel labels.
void wifi_ui_set_ota_status(const char* current_version,
                            bool update_available,
                            const char* latest_version,
                            bool auto_update);
