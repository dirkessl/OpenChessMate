#pragma once
#include <lvgl.h>

/*
 * Shared chess UI module — used by both the ESP32 target and the
 * desktop SDL simulator.  All LVGL widget creation, clock logic,
 * board rendering, and message handling lives here.
 *
 * Platform-specific code (display driver, input driver, comms)
 * stays in each platform's main.cpp.
 */

// Callback type for sending protocol messages to the master
typedef void (*chess_ui_send_fn_t)(const char* msg);

// Callback type for setting display brightness (0–100)
typedef void (*chess_ui_brightness_fn_t)(int percent);

/// Create the chess UI widgets on the current LVGL screen.
/// @param screen_w      display width in pixels
/// @param screen_h      display height in pixels
/// @param piece_font    font used for piece labels (e.g., &OpenChessFont_32
///                      on ESP32, &lv_font_montserrat_14 on sim)
/// @param send_fn       callback to send messages (Serial2.print / stdout)
/// @param brightness_fn callback to set backlight brightness (may be NULL)
void chess_ui_create(int screen_w, int screen_h,
                     const lv_font_t* piece_font,
                     chess_ui_send_fn_t send_fn,
                     chess_ui_brightness_fn_t brightness_fn);

/// Parse and handle an incoming protocol line (e.g., "STATE|fen=...",
/// "HINT|move=e2e4", "MODE|value=1").  Accepts a plain C string.
void chess_ui_handle_message(const char* line);

/// Render a FEN position string on the board (board part only, e.g.,
/// "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR").
void chess_ui_render_fen(const char* fen);

/// Highlight a move on the board and update the status label.
void chess_ui_set_move(int fr, int fc, int tr, int tc, const char* text);

// ---------------------------------------------------------------------------
// External integration hooks (used by wifi_ui / wifi_manager_ui)
// ---------------------------------------------------------------------------

/// Callback invoked when the user taps the "WiFi & Updates" button on the
/// settings screen. The button is always present; if no handler is set
/// (e.g. in the simulator) the tap is ignored.
typedef void (*chess_ui_btn_cb_t)(void* user_data);
void chess_ui_set_wifi_button_handler(chess_ui_btn_cb_t cb, void* user_data);

/// Update the WiFi status badge shown on the welcome screen.
/// `online` true = STA connected or AP mode active. `ssid`/`ip` may be NULL.
void chess_ui_set_wifi_status(bool online, const char* ssid, const char* ip);

/// Reset move highlighting back to normal square colors.
void chess_ui_reset_highlight();

/// Show the welcome / mode selection screen.
void chess_ui_show_welcome();

/// Switch to the game screen (board, clock, buttons).
/// @param mode_name  Short description shown on the status label (e.g.,
///                   "Human vs Human").  May be NULL.
void chess_ui_show_game(const char* mode_name);
