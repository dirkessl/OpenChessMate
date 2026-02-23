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

/// Create the chess UI widgets on the current LVGL screen.
/// @param screen_w   display width in pixels
/// @param screen_h   display height in pixels
/// @param piece_font font used for piece labels (e.g., &OpenChessFont_32
///                   on ESP32, &lv_font_montserrat_14 on sim)
/// @param send_fn    callback to send messages (Serial2.print / stdout)
void chess_ui_create(int screen_w, int screen_h,
                     const lv_font_t* piece_font,
                     chess_ui_send_fn_t send_fn);

/// Parse and handle an incoming protocol line (e.g., "STATE|fen=...",
/// "HINT|move=e2e4", "MODE|value=1").  Accepts a plain C string.
void chess_ui_handle_message(const char* line);

/// Render a FEN position string on the board (board part only, e.g.,
/// "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR").
void chess_ui_render_fen(const char* fen);

/// Highlight a move on the board and update the status label.
void chess_ui_set_move(int fr, int fc, int tr, int tc, const char* text);

/// Reset move highlighting back to normal square colors.
void chess_ui_reset_highlight();

/// Show the welcome / mode selection screen.
void chess_ui_show_welcome();

/// Switch to the game screen (board, clock, buttons).
/// @param mode_name  Short description shown on the status label (e.g.,
///                   "Human vs Human").  May be NULL.
void chess_ui_show_game(const char* mode_name);
