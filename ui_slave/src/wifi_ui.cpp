/**
 * @file wifi_ui.cpp
 * @brief Full-screen LVGL panel for WiFi credentials + OTA controls.
 *
 * Layout (480 × 800 portrait):
 *   ┌─────────────────────────────┐
 *   │ ← Back     WiFi & Updates   │   header
 *   │ Status: connected, IP …     │
 *   │ ┌─────────────────────────┐ │
 *   │ │ SSID (dropdown)  [Scan] │ │
 *   │ │ Password (textarea)     │ │
 *   │ │ [ Save & Connect ]      │ │
 *   │ └─────────────────────────┘ │
 *   │ ┌─────────────────────────┐ │
 *   │ │ Firmware: vX.Y.Z        │ │
 *   │ │ [Check] [Apply update]  │ │
 *   │ │ □ Auto-update on boot   │ │
 *   │ └─────────────────────────┘ │
 *   │           keyboard          │   appears when password focused
 *   └─────────────────────────────┘
 */

#include "wifi_ui.h"
#include <Arduino.h>
#include <string.h>
#include <vector>

static wifi_ui_hooks s_hooks = {};

static lv_obj_t* s_screen        = nullptr;
static lv_obj_t* s_status_lbl    = nullptr;
static lv_obj_t* s_ssid_dd       = nullptr;
static lv_obj_t* s_pass_ta       = nullptr;
static lv_obj_t* s_keyboard      = nullptr;
static lv_obj_t* s_ota_lbl       = nullptr;
static lv_obj_t* s_auto_sw       = nullptr;

// Cached SSID list so lv_dropdown options string stays valid
static String s_dd_options;
// Raw SSIDs in dropdown order — selected via index, NOT the displayed text
// (which carries an "(-XX dBm)" suffix that would corrupt WiFi.begin()).
static std::vector<String> s_dd_ssids;

// ---------- event callbacks ----------
static void on_back(lv_event_t*)        { wifi_ui_hide(); }
static void on_scan(lv_event_t*)        { if (s_hooks.scan_request) s_hooks.scan_request(s_hooks.user); }
static void on_check(lv_event_t*)       { if (s_hooks.ota_check)    s_hooks.ota_check(s_hooks.user); }
static void on_apply(lv_event_t*)       { if (s_hooks.ota_apply)    s_hooks.ota_apply(s_hooks.user); }
static void on_auto(lv_event_t* e) {
  if (!s_hooks.ota_set_auto) return;
  bool en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  s_hooks.ota_set_auto(s_hooks.user, en);
}
static void on_save(lv_event_t*) {
  if (!s_hooks.save_creds) return;
  uint16_t idx = lv_dropdown_get_selected(s_ssid_dd);
  String ssid;
  if (idx < s_dd_ssids.size()) {
    ssid = s_dd_ssids[idx];
  } else {
    // No scan results yet — fall back to displayed text (user typed it via
    // a future hidden hidden-network entry; today we just take the label).
    char buf[64] = {0};
    lv_dropdown_get_selected_str(s_ssid_dd, buf, sizeof(buf));
    ssid = buf;
  }
  const char* pass = lv_textarea_get_text(s_pass_ta);
  s_hooks.save_creds(s_hooks.user, ssid.c_str(), pass);
}
static void on_pass_focus(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (!s_keyboard) return;
  if (code == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(s_keyboard, s_pass_ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  } else if (code == LV_EVENT_DEFOCUSED) {
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---------- helpers ----------
static lv_obj_t* card(lv_obj_t* parent, int w, int h) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x262626), 0);
  lv_obj_set_style_radius(c, 8, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
static lv_obj_t* primary_btn(lv_obj_t* parent, const char* text, int w, int h, lv_event_cb_t cb) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x37474F), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x546E7A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(l);
  return b;
}

// ---------- public API ----------

void wifi_ui_init(lv_obj_t* parent, int screen_w, int screen_h, const wifi_ui_hooks& hooks) {
  s_hooks  = hooks;
  s_screen = lv_obj_create(parent);
  lv_obj_set_size(s_screen, screen_w, screen_h);
  lv_obj_set_pos(s_screen, 0, 0);
  lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(s_screen, 0, 0);
  lv_obj_set_style_radius(s_screen, 0, 0);
  lv_obj_set_style_pad_all(s_screen, 0, 0);
  lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

  // Header — title + back button
  lv_obj_t* title = lv_label_create(s_screen);
  lv_label_set_text(title, "WiFi & Updates");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t* back = primary_btn(s_screen, LV_SYMBOL_LEFT " Back", 90, 36, on_back);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);

  s_status_lbl = lv_label_create(s_screen);
  lv_label_set_text(s_status_lbl, "Status: …");
  lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 56);

  int pad = 16;
  int w   = screen_w - 2 * pad;

  // ---- WiFi card ----
  lv_obj_t* wcard = card(s_screen, w, 220);
  lv_obj_align(wcard, LV_ALIGN_TOP_MID, 0, 90);

  lv_obj_t* sl = lv_label_create(wcard);
  lv_label_set_text(sl, "Network");
  lv_obj_set_style_text_color(sl, lv_color_hex(0x9cf), 0);
  lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 0, 0);

  s_ssid_dd = lv_dropdown_create(wcard);
  lv_dropdown_set_options(s_ssid_dd, "(scan first)");
  lv_obj_set_size(s_ssid_dd, w - 80 - 30, 40);
  lv_obj_align(s_ssid_dd, LV_ALIGN_TOP_LEFT, 0, 24);

  lv_obj_t* scan_btn = primary_btn(wcard, "Scan", 70, 40, on_scan);
  lv_obj_align(scan_btn, LV_ALIGN_TOP_RIGHT, 0, 24);

  s_pass_ta = lv_textarea_create(wcard);
  lv_textarea_set_one_line(s_pass_ta, true);
  lv_textarea_set_password_mode(s_pass_ta, true);
  lv_textarea_set_placeholder_text(s_pass_ta, "Password");
  lv_obj_set_size(s_pass_ta, w - 30, 40);
  lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 0, 76);
  lv_obj_add_event_cb(s_pass_ta, on_pass_focus, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(s_pass_ta, on_pass_focus, LV_EVENT_DEFOCUSED, nullptr);

  lv_obj_t* save_btn = primary_btn(wcard, "Save & Connect", w - 30, 44, on_save);
  lv_obj_align(save_btn, LV_ALIGN_TOP_LEFT, 0, 130);

  // ---- OTA card ----
  lv_obj_t* ocard = card(s_screen, w, 170);
  lv_obj_align(ocard, LV_ALIGN_TOP_MID, 0, 320);

  lv_obj_t* ol = lv_label_create(ocard);
  lv_label_set_text(ol, "Firmware");
  lv_obj_set_style_text_color(ol, lv_color_hex(0x9cf), 0);
  lv_obj_align(ol, LV_ALIGN_TOP_LEFT, 0, 0);

  s_ota_lbl = lv_label_create(ocard);
  lv_label_set_text(s_ota_lbl, "v?  · status unknown");
  lv_obj_set_style_text_color(s_ota_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(s_ota_lbl, LV_ALIGN_TOP_LEFT, 0, 22);

  lv_obj_t* check_btn = primary_btn(ocard, "Check", (w - 50) / 2, 40, on_check);
  lv_obj_align(check_btn, LV_ALIGN_TOP_LEFT, 0, 50);
  lv_obj_t* apply_btn = primary_btn(ocard, "Apply update", (w - 50) / 2, 40, on_apply);
  lv_obj_align(apply_btn, LV_ALIGN_TOP_RIGHT, 0, 50);

  lv_obj_t* sw_lbl = lv_label_create(ocard);
  lv_label_set_text(sw_lbl, "Auto-update on boot");
  lv_obj_set_style_text_color(sw_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(sw_lbl, LV_ALIGN_TOP_LEFT, 0, 100);

  s_auto_sw = lv_switch_create(ocard);
  lv_obj_align(s_auto_sw, LV_ALIGN_TOP_RIGHT, 0, 96);
  lv_obj_add_event_cb(s_auto_sw, on_auto, LV_EVENT_VALUE_CHANGED, nullptr);

  // ---- On-screen keyboard (hidden until password focused) ----
  s_keyboard = lv_keyboard_create(s_screen);
  lv_obj_set_size(s_keyboard, screen_w, 240);
  lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void wifi_ui_show(void) {
  if (!s_screen) return;
  lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_screen);
}
void wifi_ui_hide(void) {
  if (!s_screen) return;
  lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void wifi_ui_set_status(bool online, bool ap_mode, const char* ssid, const char* ip) {
  if (!s_status_lbl) return;
  char buf[160];
  if (ap_mode) {
    snprintf(buf, sizeof(buf), "AP mode · %s · %s", ssid ? ssid : "?", ip ? ip : "?");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFFB74D), 0);
  } else if (online) {
    snprintf(buf, sizeof(buf), "Connected · %s · %s", ssid ? ssid : "?", ip ? ip : "?");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x77DD77), 0);
  } else {
    snprintf(buf, sizeof(buf), "Disconnected");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xE57373), 0);
  }
  lv_label_set_text(s_status_lbl, buf);
}

void wifi_ui_set_scan_results(const char* const* ssids, const int* rssis, int count) {
  if (!s_ssid_dd) return;
  s_dd_options = "";
  s_dd_ssids.clear();
  for (int i = 0; i < count; ++i) {
    if (i > 0) s_dd_options += "\n";
    s_dd_options += ssids[i];
    s_dd_ssids.emplace_back(ssids[i]);
    if (rssis) {
      char rb[16]; snprintf(rb, sizeof(rb), " (%d dBm)", rssis[i]);
      s_dd_options += rb;
    }
  }
  if (count == 0) s_dd_options = "(no networks found)";
  lv_dropdown_set_options(s_ssid_dd, s_dd_options.c_str());
}

void wifi_ui_set_ota_status(const char* current_version, bool update_available,
                            const char* latest_version, bool auto_update) {
  if (s_ota_lbl) {
    char buf[128];
    if (update_available && latest_version && latest_version[0]) {
      snprintf(buf, sizeof(buf), "v%s · update available: v%s",
               current_version ? current_version : "?", latest_version);
      lv_obj_set_style_text_color(s_ota_lbl, lv_color_hex(0x77DD77), 0);
    } else {
      snprintf(buf, sizeof(buf), "v%s · up to date", current_version ? current_version : "?");
      lv_obj_set_style_text_color(s_ota_lbl, lv_color_hex(0xCCCCCC), 0);
    }
    lv_label_set_text(s_ota_lbl, buf);
  }
  if (s_auto_sw) {
    if (auto_update) lv_obj_add_state(s_auto_sw, LV_STATE_CHECKED);
    else             lv_obj_clear_state(s_auto_sw, LV_STATE_CHECKED);
  }
}
