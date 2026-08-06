/*
 * chess_ui.cpp — Shared chess UI implementation
 *
 * Pure LVGL code, no platform dependencies.
 * Works on both ESP32 (Arduino) and desktop (SDL simulator).
 */
#include "chess_ui.h"
#include "micro_engine.h"
#include "pieces/pieces.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIMULATOR
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
static Preferences s_prefs;
// Engine task: dedicated 40KB stack on core 0 (internal SRAM).
// Do NOT use PSRAM for task stacks — requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY.
#define ENGINE_TASK_STACK (40 * 1024)
static TaskHandle_t s_engine_task_handle = nullptr;
static SemaphoreHandle_t s_engine_trigger = nullptr; // given to start search
static SemaphoreHandle_t s_engine_done = nullptr;    // given when search done
static me_state_t s_engine_input;
static me_move_t s_engine_result;
static int s_engine_depth_arg = 3;
// True while the engine task is running a search — used to gate UI interaction.
static volatile bool s_engine_thinking = false;
// LVGL timer handle for polling engine completion (defined here; initialized in chess_ui_create)
static lv_timer_t* s_engine_poll_timer = nullptr;

static void engine_task_fn(void* arg) {
  (void)arg;
  while (true) {
    xSemaphoreTake(s_engine_trigger, portMAX_DELAY);
    s_engine_result = me_search(&s_engine_input, s_engine_depth_arg);
    xSemaphoreGive(s_engine_done);
  }
}

// Fire-and-forget: start the engine on its own task and return immediately.
// Must NOT be called while a search is already running — call abort_engine_search() first.
static void start_engine_search(const me_state_t* st, int depth) {
  if (!s_engine_task_handle) return;
  if (s_engine_thinking) return; // guard: never double-trigger
  s_engine_input = *st;
  s_engine_depth_arg = depth;
  s_engine_thinking = true;
  if (s_engine_poll_timer) lv_timer_resume(s_engine_poll_timer);
  xSemaphoreGive(s_engine_trigger);
}

// Non-blocking check: returns true (and fills result) once done, false if still running.
static bool poll_engine_result(me_move_t* out) {
  if (!s_engine_thinking) return false;
  if (xSemaphoreTake(s_engine_done, 0) == pdTRUE) {
    s_engine_thinking = false;
    if (s_engine_poll_timer) lv_timer_pause(s_engine_poll_timer);
    *out = s_engine_result;
    return true;
  }
  return false;
}

// Abort any running search and wait for it to finish (max 500 ms).
// Safe to call from the LVGL task — the engine aborts within microseconds
// of s_search_aborted being set, so the wait is almost always instant.
static void abort_engine_search() {
  if (!s_engine_thinking) return;
  me_abort_search();
  // Drain the done semaphore (engine signals it even after abort)
  for (int i = 0; i < 10; i++) {
    if (xSemaphoreTake(s_engine_done, pdMS_TO_TICKS(50)) == pdTRUE) break;
  }
  s_engine_thinking = false;
  if (s_engine_poll_timer) lv_timer_pause(s_engine_poll_timer);
}
#else
// Simulator: synchronous search (SDL main thread has plenty of stack)
static volatile bool s_engine_thinking = false;
static void start_engine_search(const me_state_t* st, int depth) {
  (void)st; (void)depth;
}
static bool poll_engine_result(me_move_t* out) {
  (void)out; return false;
}
static void abort_engine_search() {}
#endif

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static chess_ui_send_fn_t s_send_fn = nullptr;
static chess_ui_brightness_fn_t s_brightness_fn = nullptr;
static const lv_font_t* s_piece_font = nullptr;

// LVGL widget pointers
static lv_obj_t* s_welcome_screen = nullptr; // Welcome / mode selection panel
static lv_obj_t* s_wifi_badge     = nullptr; // Tiny WiFi status label on welcome screen

// Registered by chess_ui_set_wifi_button_handler(); invoked from the
// "WiFi & Updates" button on the settings screen. NULL by default
// (e.g. in the simulator) — taps become no-ops.
static chess_ui_btn_cb_t s_wifi_btn_cb        = nullptr;
static void*             s_wifi_btn_user_data = nullptr;
static void wifi_settings_btn_cb(lv_event_t*) {
  if (s_wifi_btn_cb) s_wifi_btn_cb(s_wifi_btn_user_data);
}
static lv_obj_t* s_game_screen = nullptr;    // Game board + clock + buttons panel
static lv_obj_t* s_status_label = nullptr;
static lv_obj_t* s_btns[8][8];   // button per cell (holds bg color)
static lv_obj_t* s_labels[8][8]; // label inside each button (piece glyph)

// Chess clock
static lv_obj_t* s_clock_white_panel = nullptr;
static lv_obj_t* s_clock_black_panel = nullptr;
static lv_obj_t* s_clock_play_btn = nullptr;
static lv_obj_t* s_clock_play_lbl = nullptr;
static int s_white_time_sec = 10 * 60;
static int s_black_time_sec = 10 * 60;
static bool s_white_active = true;
static bool s_clock_running = false;

// Canvas-based rotated clock (source drawn horizontally, then rotated 90° CW)
// Portrait 480×800: panel_w=108, panel_h=162
// After 90° CW rotation: src(162×108) → dst(108×162)
#define CLK_SRC_W 162
#define CLK_SRC_H 108
#define CLK_DSP_W CLK_SRC_H // 108
#define CLK_DSP_H CLK_SRC_W // 162

// DSEG7 Classic Bold 7-segment display fonts
LV_FONT_DECLARE(dseg7_classic_bold_46);
LV_FONT_DECLARE(dseg7_classic_bold_100);
static lv_color_t* s_clk_src_buf = nullptr;
static lv_color_t* s_clk_w_buf = nullptr;
static lv_color_t* s_clk_b_buf = nullptr;
static lv_obj_t* s_clk_src_canvas = nullptr;
static lv_obj_t* s_clk_w_canvas = nullptr;
static lv_obj_t* s_clk_b_canvas = nullptr;

// Canvas buffers for 180°-rotated black buttons
#define BLK_BTN_CVS_W 100
#define BLK_BTN_CVS_H 24
#define BLK_BTN_COUNT 3
static lv_color_t* s_blk_btn_scratch = nullptr;
static lv_color_t* s_blk_btn_buf[BLK_BTN_COUNT] = {};

// (Black moves canvas removed — replaced by combined move list box)

// Black (player 2) move list — canvas rotated 180° (transform_angle doesn't
// work on lv_obj containers in LVGL v8, so we render to canvas + rotateBuf180)
static int s_blk_ml_w = 0;
static int s_blk_ml_h = 0;
static lv_color_t* s_blk_ml_scratch = nullptr;
static lv_color_t* s_blk_ml_buf = nullptr;
static lv_obj_t* s_blk_ml_src_canvas = nullptr;
static lv_obj_t* s_blk_ml_canvas = nullptr;
static lv_obj_t* s_blk_ml_box = nullptr;

// Highlight tracking
static lv_color_t s_highlight_light;
static lv_color_t s_highlight_dark;
static lv_color_t s_light_sq;
static lv_color_t s_dark_sq;
static int s_hl_from_r = -1, s_hl_from_c = -1;
static int s_hl_to_r = -1, s_hl_to_c = -1;

// Screen dimensions (cached for welcome screen layout)
static int s_screen_w = 480;
static int s_screen_h = 800;

// Piece image zoom (LVGL units: 256 = 100%).  Piece PNGs are 30×30;
// zoom = cell_size * 256 / 30 so they fill the square.
static uint16_t s_piece_zoom = 256;   // default = no scaling
static const int PIECE_IMG_SIZE = 30; // source image dimension (px)

// Mode names for display
static const char* MODE_NAMES[] = {
    "Select Mode",        // 0 = selection
    "Human vs Human",     // 1
    "Human vs Stockfish", // 2
    "Online (Lichess)",   // 3
    "Sensor Test",        // 4
    "Practice"            // 5 = vs built-in engine
};

// Current game mode (0=select, 1=HvH, 2=Stockfish, 3=Lichess, 4=SensorTest, 5=Practice)
static int s_current_mode = 0;

// Clock configuration
static int s_clock_initial_sec = 10 * 60; // default 10 minutes per side
static int s_clock_increment_sec = 0;     // Fischer increment per move
static bool s_clock_started = false;      // set true on first move
static bool s_no_clock = false;           // unlimited / no clock mode
static bool s_board_flipped = false;      // true = black at bottom
static char s_last_fen[128] = "";         // last rendered FEN for re-render on swap

// Confirmation dialog
static lv_obj_t* s_confirm_overlay = nullptr;
enum ConfirmAction { CONFIRM_NONE,
                     CONFIRM_HOME,
                     CONFIRM_NEW };
static ConfirmAction s_confirm_action = CONFIRM_NONE;

// Clock setup screen
static lv_obj_t* s_clock_screen = nullptr;
static lv_obj_t* s_custom_min_label = nullptr;
static lv_obj_t* s_custom_inc_label = nullptr;
static int s_custom_minutes = 10;
static int s_custom_increment = 0;

// Settings screen
static lv_obj_t* s_settings_screen = nullptr;
static bool s_show_clock = true;
static bool s_show_captures = true;
static bool s_show_movelist = true;
static int  s_brightness = 100; // 0–100 percent
static bool s_clock_from_settings = false; // track where clock screen was opened from
static bool s_clock_from_clockonly = false; // track if clock screen was opened from clock-only
static bool s_settings_from_game = false;  // track if settings opened from game screen

// ---------------------------------------------------------------------------
// Practice mode state (built-in engine, touch gameplay)
// ---------------------------------------------------------------------------
static me_state_t s_practice_state;          // engine game state
static me_state_t s_practice_undo_stack[60]; // undo history (position before each pair of moves)
static int s_practice_undo_count = 0;
static int s_practice_selected_sq = -1;      // selected piece square (-1 = none)
static me_move_t s_practice_legal[ME_MAX_MOVES]; // legal moves for selected piece
static int s_practice_legal_count = 0;
static int s_practice_depth = 3;             // engine search depth (difficulty)
static bool s_practice_game_over = false;
static bool s_practice_player_white = true;  // player plays white
static lv_obj_t* s_practice_ctrl_area = nullptr;
static lv_obj_t* s_practice_status_lbl = nullptr;
static lv_obj_t* s_practice_diff_lbl = nullptr;
// Colors for legal move indicators
static const uint32_t LEGAL_MOVE_DOT_CLR = 0x55555580;
static const uint32_t LEGAL_CAPTURE_CLR  = 0xFF4444;

// ---------------------------------------------------------------------------
// Opening picker — modal overlay shown when entering/restarting practice mode
// ---------------------------------------------------------------------------
struct OpeningEntry {
  const char* name;
  // Move sequence from the starting position in UCI notation, nullptr-terminated.
  // The trainer plays through these moves step by step. White moves at even
  // indices (0, 2, 4 …) are made by the human; black moves at odd indices are
  // auto-played by the trainer after a short delay (see trainer_advance).
  // "Play vs Engine" derives the position FEN from the same sequence.
  // First element nullptr means use the standard starting position (no guided moves).
  // Length 40 → up to 20 full moves, enough for canonical mainline depth.
  const char* moves[40];
};

// Canonical mainlines — depth chosen to match the standard opening "tabiya"
// (the position theory considers the basic stem of each variation), typically
// 12–20 plies. Sources: ECO classification + standard repertoire references
// (lichess.org/openings, Wikipedia mainlines, Modern Chess Openings).
static const OpeningEntry OPENINGS[] = {
  { "Starting Position", { nullptr } },
  // --- Open games (1.e4 e5) ---
  { "Italian Game", // Giuoco Pianissimo modern mainline
    { "e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "c2c3", "g8f6",
      "d2d3", "d7d6", "e1g1", "e8g8", "f1e1", "a7a6", "h2h3", "c5a7", nullptr } },
  { "Ruy Lopez (Spanish)", // Closed Morphy, Chigorin variation
    { "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6",
      "e1g1", "f8e7", "f1e1", "b7b5", "a4b3", "d7d6", "c2c3", "e8g8",
      "h2h3", "c6a5", "b3c2", "c7c5", nullptr } },
  { "King's Gambit", // Accepted, Modern Variation 5...d6
    { "e2e4", "e7e5", "f2f4", "e5f4", "g1f3", "g7g5", "h2h4", "g5g4",
      "f3e5", "g8f6", "d2d4", "d7d6", nullptr } },
  { "Vienna Game", // Falkbeer Variation
    { "e2e4", "e7e5", "b1c3", "g8f6", "f2f4", "d7d5", "f4e5", "f6e4",
      "g1f3", "f8e7", "d2d3", "e4c3", nullptr } },
  { "Petroff Defence", // Classical Cozio Attack, mainline
    { "e2e4", "e7e5", "g1f3", "g8f6", "f3e5", "d7d6", "e5f3", "f6e4",
      "d2d4", "d6d5", "f1d3", "f8d6", "e1g1", "e8g8", nullptr } },
  // --- Sicilian Defence ---
  { "Sicilian Defence", // Open Sicilian, Najdorf precursor with 6.Be2
    { "e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6",
      "b1c3", "a7a6", "f1e2", "e7e5", "d4b3", "c8e6", nullptr } },
  { "Sicilian Najdorf", // English Attack 6.Be3
    { "e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6",
      "b1c3", "a7a6", "c1e3", "e7e5", "d4b3", "c8e6", "f2f3", "f8e7",
      "d1d2", "e8g8", "e1c1", "b8d7", nullptr } },
  { "Sicilian Dragon", // Yugoslav Attack 9.Bc4
    { "e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6",
      "b1c3", "g7g6", "c1e3", "f8g7", "f2f3", "e8g8", "d1d2", "b8c6",
      "f1c4", "c8d7", "e1c1", "a8c8", nullptr } },
  // --- Semi-open games ---
  { "French Defence", // Winawer 4.e5
    { "e2e4", "e7e6", "d2d4", "d7d5", "b1c3", "f8b4", "e4e5", "c7c5",
      "a2a3", "b4c3", "b2c3", "g8e7", "d1g4", "d8c7", nullptr } },
  { "Caro-Kann Defence", // Classical Capablanca 4...Bf5
    { "e2e4", "c7c6", "d2d4", "d7d5", "b1c3", "d5e4", "c3e4", "c8f5",
      "e4g3", "f5g6", "h2h4", "h7h6", "g1f3", "b8d7", nullptr } },
  { "Scandinavian Defence", // 3...Qa5 Mieses-Kotroc
    { "e2e4", "d7d5", "e4d5", "d8d5", "b1c3", "d5a5", "d2d4", "g8f6",
      "g1f3", "c7c6", "f1c4", "c8f5", "c1d2", "e7e6", nullptr } },
  { "Pirc Defence", // Classical Two Knights with 7.a4
    { "e2e4", "d7d6", "d2d4", "g8f6", "b1c3", "g7g6", "g1f3", "f8g7",
      "f1e2", "e8g8", "e1g1", "c7c6", "a2a4", "a7a5", "h2h3", "b8d7", nullptr } },
  // --- Closed games (1.d4) ---
  { "Queen's Gambit", // Orthodox Exchange precursor
    { "d2d4", "d7d5", "c2c4", "e7e6", "b1c3", "g8f6", "c1g5", "f8e7",
      "e2e3", "e8g8", "g1f3", "b8d7", "a1c1", "c7c6", "f1d3", "d5c4", nullptr } },
  { "Queen's Gambit Declined", // Lasker Defence 7...Ne4
    { "d2d4", "d7d5", "c2c4", "e7e6", "b1c3", "g8f6", "c1g5", "f8e7",
      "e2e3", "h7h6", "g5h4", "e8g8", "g1f3", "f6e4", nullptr } },
  { "King's Indian Defence", // Classical Mar del Plata, 9.Ne1
    { "d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "f8g7", "e2e4", "d7d6",
      "g1f3", "e8g8", "f1e2", "e7e5", "e1g1", "b8c6", "d4d5", "c6e7",
      "f3e1", "f6d7", nullptr } },
  { "Nimzo-Indian Defence", // Rubinstein 4.e3, ...d5 ...c5
    { "d2d4", "g8f6", "c2c4", "e7e6", "b1c3", "f8b4", "e2e3", "e8g8",
      "f1d3", "d7d5", "g1f3", "c7c5", "e1g1", "b8c6", "a2a3", "b4c3", nullptr } },
  { "Dutch Defence", // Stonewall structure
    { "d2d4", "f7f5", "g1f3", "g8f6", "g2g3", "e7e6", "f1g2", "d7d5",
      "e1g1", "f8d6", "c2c4", "c7c6", "b2b3", "d8e7", "c1b2", "b7b6", nullptr } },
  // --- Flank openings ---
  { "English Opening", // Reversed Sicilian, 4.cxd5
    { "c2c4", "e7e5", "b1c3", "g8f6", "g2g3", "d7d5", "c4d5", "f6d5",
      "f1g2", "d5b6", "g1f3", "b8c6", "e1g1", "f8e7", nullptr } },
  { "King's Indian Attack", // vs ...d5 ...e6 setup
    { "g1f3", "d7d5", "g2g3", "g8f6", "f1g2", "e7e6", "e1g1", "f8e7",
      "d2d3", "e8g8", "b1d2", "c7c5", "e2e4", "b8c6", "f1e1", "b7b6", nullptr } },
  { "London System", // Modern with 3.Bf4
    { "d2d4", "d7d5", "g1f3", "g8f6", "c1f4", "e7e6", "e2e3", "f8d6",
      "f4g3", "e8g8", "b1d2", "c7c5", "c2c3", "b8c6", "f1d3", "b7b6", nullptr } },
  { "Reti Opening", // 1.Nf3 d5 2.c4 e6 mainline
    { "g1f3", "d7d5", "c2c4", "e7e6", "g2g3", "g8f6", "f1g2", "f8e7",
      "e1g1", "e8g8", "b2b3", "c7c5", "c1b2", "b8c6", "c4d5", "e6d5", nullptr } },
};
static const int OPENING_COUNT = (int)(sizeof(OPENINGS) / sizeof(OPENINGS[0]));

// Currently selected opening index
static int s_selected_opening = 0;
// Opening picker overlay widget
static lv_obj_t* s_opening_picker = nullptr;

// ---------------------------------------------------------------------------
// Guided opening trainer state
// ---------------------------------------------------------------------------
static bool      s_trainer_active      = false;  // true = trainer controls tap input
static int       s_trainer_idx         = 0;       // which opening is being trained
static int       s_trainer_step        = 0;       // index of the next expected move
static int       s_trainer_total       = 0;       // total moves in this opening
static bool      s_trainer_guided      = true;    // show hints vs quiz mode
static int       s_trainer_sel_sq      = -1;      // currently selected source square
static me_move_t s_trainer_moves[40];             // pre-parsed moves for current opening
// Display-coord squares currently coloured as hints (or -1 if none).
static int       s_trainer_hint_from   = -1;      // display row*8+col
static int       s_trainer_hint_to     = -1;
// Trainer UI widgets (in s_practice_ctrl_area)
static lv_obj_t* s_trainer_progress_lbl = nullptr;
static lv_obj_t* s_trainer_hint_btn     = nullptr;
// Practice-mode widgets we need to show/hide when switching modes
static lv_obj_t* s_practice_undo_btn   = nullptr;
static lv_obj_t* s_practice_swap_btn   = nullptr;
static lv_obj_t* s_practice_diff_minus = nullptr;
static lv_obj_t* s_practice_diff_plus  = nullptr;

// ---------------------------------------------------------------------------
// Color theme system
// ---------------------------------------------------------------------------
struct ColorTheme {
  const char* name;
  // Board
  uint32_t light_sq;
  uint32_t dark_sq;
  uint32_t hl_light;      // highlighted light square
  uint32_t hl_dark;        // highlighted dark square
  // Game clock (canvas-rendered)
  uint32_t clk_active_bg;
  uint32_t clk_active_fg;
  uint32_t clk_inactive_bg;
  uint32_t clk_inactive_fg;
  // Clock play button
  uint32_t clk_btn_bg;
  uint32_t clk_btn_pressed;
  // Clock-only LED display
  uint32_t led_bg;
  uint32_t led_on;
  uint32_t led_dim;
  uint32_t led_ghost_active;
  uint32_t led_ghost_inactive;
  uint32_t led_panel_glow;
  // Preview swatch (dark sq color used as swatch)
};

static const ColorTheme THEMES[] = {
    {"Classic",
     0xF0D9B5, 0xB58863, 0xAAD751, 0x6B8E23,
     0x6D3B2A, 0xFFFFFF, 0x3E2117, 0x999999,
     0x4E2A1A, 0x6D3B2A,
     0x0A0A0A, 0xDD0000, 0x550000, 0x1A0808, 0x100303, 0x140404},

    {"Blue",
     0xDEE3E6, 0x8CA2AD, 0x829DC8, 0x4A7BAF,
     0x2C5F8A, 0xFFFFFF, 0x1A3A52, 0x8899AA,
     0x1A3A52, 0x2C5F8A,
     0x050A10, 0x0088FF, 0x003366, 0x081828, 0x060E18, 0x0A1A2A},

    {"Green",
     0xEEEED2, 0x769656, 0xF6F669, 0xBBCA2A,
     0x2E6B30, 0xFFFFFF, 0x1A3E1B, 0x88AA88,
     0x1A3E1B, 0x2E6B30,
     0x040A04, 0x00DD00, 0x005500, 0x081808, 0x060E06, 0x0A1A0A},

    {"Coral",
     0xF5E6D3, 0xD4886B, 0xE8C170, 0xC9943A,
     0xA34A28, 0xFFFFFF, 0x5C2A16, 0xBB9988,
     0x5C2A16, 0xA34A28,
     0x0A0605, 0xFF6644, 0x662200, 0x1A0A05, 0x100603, 0x140A06},

    {"Midnight",
     0xB0B7C3, 0x5C6378, 0x7B92B8, 0x4A6490,
     0x3A4660, 0xE0E8FF, 0x1E2636, 0x7788AA,
     0x1E2636, 0x3A4660,
     0x060810, 0x6688FF, 0x223366, 0x0A1020, 0x080C18, 0x0C1428},

    {"Walnut",
     0xD8C4A0, 0x7A5C3C, 0xC4A84C, 0x8E7428,
     0x5A3820, 0xF0E0C0, 0x321E10, 0x8A7A60,
     0x321E10, 0x5A3820,
     0x080604, 0xCC8800, 0x553300, 0x181008, 0x100A04, 0x1A1208},

    {"Light Grey",
     0xF0F0F0, 0xA0A0A0, 0xC8E06A, 0x8AAE30,
     0x606060, 0xFFFFFF, 0x404040, 0xAAAAAA,
     0x484848, 0x606060,
     0xE0E0E0, 0x333333, 0x999999, 0xD0D0D0, 0xE8E8E8, 0xC8C8C8},

    {"High Contrast",
     0xFFFFFF, 0x000000, 0xFFFF00, 0x0000FF,
     0x000000, 0xFFFF00, 0x333333, 0xCCCCCC,
     0x222222, 0x000000,
     0x000000, 0x00FF00, 0x006600, 0x0A0A0A, 0x050505, 0x0C0C0C},
};
static const int NUM_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);
static int s_theme_index = 0;  // current theme
static lv_obj_t* s_theme_label = nullptr;  // theme name label in settings

// Theme preview widgets
static lv_obj_t* s_preview_sq[4][4] = {};
static lv_obj_t* s_preview_clk_active = nullptr;
static lv_obj_t* s_preview_clk_inactive = nullptr;
static lv_obj_t* s_preview_clk_active_lbl = nullptr;
static lv_obj_t* s_preview_clk_inactive_lbl = nullptr;
static lv_obj_t* s_preview_led_box = nullptr;
static lv_obj_t* s_preview_led_lbl = nullptr;

// Clock-only screen
static lv_obj_t* s_clockonly_screen = nullptr;
static lv_obj_t* s_co_white_panel = nullptr;
static lv_obj_t* s_co_black_panel = nullptr;
static lv_obj_t* s_co_play_lbl = nullptr;
static lv_obj_t* s_co_tc_label = nullptr;

// 7-segment canvas buffers for clock-only mode
// Rendered horizontally into scratch (CVS_W × CVS_H), then rotated 90° CW
// into display buffers (DSP_W × DSP_H) so both clocks read sideways.
// Clock-only mode canvas dimensions
// Source rendered horizontally, then rotated 90° CW for display
#define SEG7_CVS_W      360
#define SEG7_CVS_H      156
#define SEG7_DSP_W      SEG7_CVS_H   // after 90° CW rotation
#define SEG7_DSP_H      SEG7_CVS_W
static lv_color_t* s_co_scratch_buf = nullptr;
static lv_color_t* s_co_white_buf = nullptr;
static lv_color_t* s_co_black_buf = nullptr;
static lv_obj_t* s_co_scratch_canvas = nullptr;
static lv_obj_t* s_co_white_canvas = nullptr;
static lv_obj_t* s_co_black_canvas = nullptr;

// Settings persistence
#ifdef SIMULATOR
static const char* SETTINGS_FILE = "chess_settings.dat";
#endif

static void saveSettings() {
#ifdef SIMULATOR
  FILE* f = fopen(SETTINGS_FILE, "wb");
  if (!f) return;
  uint8_t data[5] = {
      (uint8_t)(s_show_clock ? 1 : 0),
      (uint8_t)(s_show_captures ? 1 : 0),
      (uint8_t)(s_show_movelist ? 1 : 0),
      (uint8_t)s_brightness,
      (uint8_t)s_theme_index,
  };
  fwrite(data, 1, sizeof(data), f);
  fclose(f);
#else
  s_prefs.begin("chess", false);
  s_prefs.putBool("clock", s_show_clock);
  s_prefs.putBool("captures", s_show_captures);
  s_prefs.putBool("movelist", s_show_movelist);
  s_prefs.putInt("bright", s_brightness);
  s_prefs.putInt("theme", s_theme_index);
  s_prefs.end();
#endif
}

static void loadSettings() {
#ifdef SIMULATOR
  FILE* f = fopen(SETTINGS_FILE, "rb");
  if (!f) return;
  uint8_t data[5] = {1, 1, 1, 100, 0};
  size_t n = fread(data, 1, sizeof(data), f);
  if (n >= 3) {
    s_show_clock = data[0] != 0;
    s_show_captures = data[1] != 0;
    s_show_movelist = data[2] != 0;
  }
  if (n >= 4) {
    s_brightness = data[3];
    if (s_brightness < 1) s_brightness = 1;
    if (s_brightness > 100) s_brightness = 100;
  }
  if (n >= 5) {
    s_theme_index = data[4];
    if (s_theme_index < 0 || s_theme_index >= NUM_THEMES) s_theme_index = 0;
  }
  fclose(f);
#else
  s_prefs.begin("chess", true);
  s_show_clock = s_prefs.getBool("clock", true);
  s_show_captures = s_prefs.getBool("captures", true);
  s_show_movelist = s_prefs.getBool("movelist", true);
  s_brightness = s_prefs.getInt("bright", 100);
  if (s_brightness < 1) s_brightness = 1;
  if (s_brightness > 100) s_brightness = 100;
  s_theme_index = s_prefs.getInt("theme", 0);
  if (s_theme_index < 0 || s_theme_index >= NUM_THEMES) s_theme_index = 0;
  s_prefs.end();
#endif
}

// HvH-specific widgets
static lv_obj_t* s_white_area = nullptr; // bottom area (white buttons + moves)
static lv_obj_t* s_black_area = nullptr; // top area (black buttons + moves, rotated)
static lv_obj_t* s_white_moves_label = nullptr;
static lv_obj_t* s_black_moves_label = nullptr;
static lv_obj_t* s_swap_btn = nullptr;          // swap sides (disappears after 1st move)
static lv_obj_t* s_generic_ctrl_area = nullptr; // non-HvH: status + buttons

// Captured pieces display
#define MAX_CAPTURES 15
#define CAP_PIECE_SIZE 18
#define CAP_IMG_ZOOM 154                    // CAP_PIECE_SIZE * 256 / PIECE_IMG_SIZE
static lv_obj_t* s_white_cap_bar = nullptr; // below board
static lv_obj_t* s_black_cap_bar = nullptr; // above board
static lv_obj_t* s_wcap_imgs[MAX_CAPTURES] = {};
static lv_obj_t* s_bcap_imgs[MAX_CAPTURES] = {};

// Combined move list box
static lv_obj_t* s_movelist_box = nullptr;
static lv_obj_t* s_movelist_table = nullptr;

// Move history tracking
#define MAX_MOVE_HISTORY 30
static char s_move_list[MAX_MOVE_HISTORY][8];
static int s_move_count = 0;

// Forward declarations
static void applyVisibilitySettings();

// Clock presets — most popular tournament & online time controls
struct ClockPreset {
  const char* label;
  const char* category;
  int time_sec;
  int increment_sec;
};
static const ClockPreset CLOCK_PRESETS[] = {
    {"1+0", "Bullet", 60, 0},
    {"1+1", "Bullet", 60, 1},
    {"2+1", "Bullet", 120, 1},
    {"3+0", "Blitz", 180, 0},
    {"3+2", "Blitz", 180, 2},
    {"5+0", "Blitz", 300, 0},
    {"5+3", "Blitz", 300, 3},
    {"10+0", "Rapid", 600, 0},
    {"10+5", "Rapid", 600, 5},
    {"15+10", "Rapid", 900, 10},
    {"30+0", "Rapid", 1800, 0},
    {"60+30", "Classical", 3600, 30},
    {"90+30", "Classical", 5400, 30},
};
static const int NUM_PRESETS = (int)(sizeof(CLOCK_PRESETS) / sizeof(CLOCK_PRESETS[0]));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static lv_color_t squareColor(int r, int c) {
  return ((r + c) % 2 == 0) ? s_light_sq : s_dark_sq;
}

// Forward declarations for theme application
static void updateClockDisplay();
static void updateClockOnlyDisplay();

static void applyTheme() {
  const ColorTheme& t = THEMES[s_theme_index];
  s_light_sq = lv_color_hex(t.light_sq);
  s_dark_sq = lv_color_hex(t.dark_sq);
  s_highlight_light = lv_color_hex(t.hl_light);
  s_highlight_dark = lv_color_hex(t.hl_dark);
  // Repaint board squares (only if board exists)
  if (s_btns[0][0]) {
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        lv_obj_set_style_bg_color(s_btns[r][c], squareColor(r, c), 0);
    // Reapply highlights if active
    if (s_hl_from_r >= 0) {
      bool dark = ((s_hl_from_r + s_hl_from_c) % 2 != 0);
      lv_obj_set_style_bg_color(s_btns[s_hl_from_r][s_hl_from_c],
                                dark ? s_highlight_dark : s_highlight_light, 0);
    }
    if (s_hl_to_r >= 0) {
      bool dark = ((s_hl_to_r + s_hl_to_c) % 2 != 0);
      lv_obj_set_style_bg_color(s_btns[s_hl_to_r][s_hl_to_c],
                                dark ? s_highlight_dark : s_highlight_light, 0);
    }
  }
  // Update clock panels
  updateClockDisplay();
  updateClockOnlyDisplay();

  // Update settings preview
  const ColorTheme& tp = THEMES[s_theme_index];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!s_preview_sq[r][c]) continue;
      bool is_hl = (r == 1 && c == 2) || (r == 2 && c == 1);
      bool dark = (r + c) % 2 != 0;
      uint32_t clr;
      if (is_hl) clr = dark ? tp.hl_dark : tp.hl_light;
      else clr = dark ? tp.dark_sq : tp.light_sq;
      lv_obj_set_style_bg_color(s_preview_sq[r][c], lv_color_hex(clr), 0);
    }
  if (s_preview_clk_active) {
    lv_obj_set_style_bg_color(s_preview_clk_active, lv_color_hex(tp.clk_active_bg), 0);
    if (s_preview_clk_active_lbl)
      lv_obj_set_style_text_color(s_preview_clk_active_lbl, lv_color_hex(tp.clk_active_fg), 0);
  }
  if (s_preview_clk_inactive) {
    lv_obj_set_style_bg_color(s_preview_clk_inactive, lv_color_hex(tp.clk_inactive_bg), 0);
    if (s_preview_clk_inactive_lbl)
      lv_obj_set_style_text_color(s_preview_clk_inactive_lbl, lv_color_hex(tp.clk_inactive_fg), 0);
  }
  if (s_preview_led_box) {
    lv_obj_set_style_bg_color(s_preview_led_box, lv_color_hex(tp.led_bg), 0);
    if (s_preview_led_lbl)
      lv_obj_set_style_text_color(s_preview_led_lbl, lv_color_hex(tp.led_on), 0);
  }
}

static void formatTime(int total_sec, char* buf, int buf_size) {
  if (s_no_clock) {
    snprintf(buf, buf_size, "--:--");
    return;
  }
  int mins = total_sec / 60;
  int secs = total_sec % 60;
  snprintf(buf, buf_size, "%d:%02d", mins, secs);
}

// Rotate pixel buffer 90° clockwise: src(sw×sh) → dst(sh×sw)
static void rotateBuf90CW(const lv_color_t* src, int sw, int sh,
                          lv_color_t* dst) {
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++)
      dst[x * sh + (sh - 1 - y)] = src[y * sw + x];
}

// Rotate pixel buffer 180°: same dimensions, reverse pixel order
static void rotateBuf180(const lv_color_t* src, int w, int h,
                         lv_color_t* dst) {
  int total = w * h;
  for (int i = 0; i < total; i++)
    dst[total - 1 - i] = src[i];
}

// ---------------------------------------------------------------------------
// Clock display update
// ---------------------------------------------------------------------------

static void updateClockOnlyDisplay() {
  if (!s_co_scratch_canvas || !s_co_white_canvas || !s_co_black_canvas) return;
  const ColorTheme& t = THEMES[s_theme_index];
  lv_color_t bg = lv_color_hex(t.led_bg);
  lv_color_t bright_on = lv_color_hex(t.led_on);
  lv_color_t dim_on = lv_color_hex(t.led_dim);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &dseg7_classic_bold_100;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  char tbuf[16];
  lv_point_t sz;
  int ty;

  // White clock (bottom, rotated 90° CW)
  lv_canvas_fill_bg(s_co_scratch_canvas, bg, LV_OPA_COVER);
  dsc.color = lv_color_hex(s_white_active ? t.led_ghost_active : t.led_ghost_inactive);
  lv_txt_get_size(&sz, "88:88", dsc.font, 0, 0, SEG7_CVS_W, LV_TEXT_FLAG_NONE);
  ty = (SEG7_CVS_H - sz.y) / 2;
  lv_canvas_draw_text(s_co_scratch_canvas, 0, ty, SEG7_CVS_W, &dsc, "88:88");
  formatTime(s_white_time_sec, tbuf, sizeof(tbuf));
  dsc.color = s_white_active ? bright_on : dim_on;
  lv_canvas_draw_text(s_co_scratch_canvas, 0, ty, SEG7_CVS_W, &dsc, tbuf);
  rotateBuf90CW(s_co_scratch_buf, SEG7_CVS_W, SEG7_CVS_H, s_co_white_buf);
  lv_obj_invalidate(s_co_white_canvas);

  // Black clock (top, rotated 90° CW)
  lv_canvas_fill_bg(s_co_scratch_canvas, bg, LV_OPA_COVER);
  dsc.color = lv_color_hex(!s_white_active ? t.led_ghost_active : t.led_ghost_inactive);
  lv_canvas_draw_text(s_co_scratch_canvas, 0, ty, SEG7_CVS_W, &dsc, "88:88");
  formatTime(s_black_time_sec, tbuf, sizeof(tbuf));
  dsc.color = !s_white_active ? bright_on : dim_on;
  lv_canvas_draw_text(s_co_scratch_canvas, 0, ty, SEG7_CVS_W, &dsc, tbuf);
  rotateBuf90CW(s_co_scratch_buf, SEG7_CVS_W, SEG7_CVS_H, s_co_black_buf);
  lv_obj_invalidate(s_co_black_canvas);
  // Panel backgrounds — subtle glow for active side
  if (s_co_white_panel)
    lv_obj_set_style_bg_color(s_co_white_panel,
        s_white_active ? lv_color_hex(THEMES[s_theme_index].led_panel_glow) : bg, 0);
  if (s_co_black_panel)
    lv_obj_set_style_bg_color(s_co_black_panel,
        !s_white_active ? lv_color_hex(THEMES[s_theme_index].led_panel_glow) : bg, 0);
  // Play/pause icon
  if (s_co_play_lbl) {
    if (s_no_clock)
      lv_label_set_text(s_co_play_lbl, LV_SYMBOL_PLAY);
    else if (s_clock_running)
      lv_label_set_text(s_co_play_lbl, LV_SYMBOL_PAUSE);
    else
      lv_label_set_text(s_co_play_lbl, LV_SYMBOL_PLAY);
  }
}

static void updateClockDisplay() {
  if (!s_clk_src_canvas || !s_clk_w_canvas || !s_clk_b_canvas) return;
  const ColorTheme& t = THEMES[s_theme_index];

  lv_color_t w_bg, w_fg, b_bg, b_fg;
  if (s_white_active) {
    w_bg = lv_color_hex(t.clk_active_bg);
    w_fg = lv_color_hex(t.clk_active_fg);
    b_bg = lv_color_hex(t.clk_inactive_bg);
    b_fg = lv_color_hex(t.clk_inactive_fg);
  } else {
    w_bg = lv_color_hex(t.clk_inactive_bg);
    w_fg = lv_color_hex(t.clk_inactive_fg);
    b_bg = lv_color_hex(t.clk_active_bg);
    b_fg = lv_color_hex(t.clk_active_fg);
  }
  lv_obj_set_style_bg_color(s_clock_white_panel, w_bg, 0);
  lv_obj_set_style_bg_color(s_clock_black_panel, b_bg, 0);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &dseg7_classic_bold_46;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  char tbuf[16];
  lv_point_t sz;
  int ty;

  // White clock: draw horizontal → rotate → display
  lv_canvas_fill_bg(s_clk_src_canvas, w_bg, LV_OPA_COVER);
  formatTime(s_white_time_sec, tbuf, sizeof(tbuf));
  dsc.color = w_fg;
  lv_txt_get_size(&sz, tbuf, dsc.font, 0, 0, CLK_SRC_W, LV_TEXT_FLAG_NONE);
  ty = (CLK_SRC_H - sz.y) / 2;
  lv_canvas_draw_text(s_clk_src_canvas, 0, ty, CLK_SRC_W, &dsc, tbuf);
  rotateBuf90CW(s_clk_src_buf, CLK_SRC_W, CLK_SRC_H, s_clk_w_buf);
  lv_obj_invalidate(s_clk_w_canvas);

  // Black clock: draw horizontal → rotate → display
  lv_canvas_fill_bg(s_clk_src_canvas, b_bg, LV_OPA_COVER);
  formatTime(s_black_time_sec, tbuf, sizeof(tbuf));
  dsc.color = b_fg;
  lv_txt_get_size(&sz, tbuf, dsc.font, 0, 0, CLK_SRC_W, LV_TEXT_FLAG_NONE);
  ty = (CLK_SRC_H - sz.y) / 2;
  lv_canvas_draw_text(s_clk_src_canvas, 0, ty, CLK_SRC_W, &dsc, tbuf);
  rotateBuf90CW(s_clk_src_buf, CLK_SRC_W, CLK_SRC_H, s_clk_b_buf);
  lv_obj_invalidate(s_clk_b_canvas);

  // Update play/pause icon
  if (s_clock_play_lbl) {
    if (s_no_clock)
      lv_label_set_text(s_clock_play_lbl, LV_SYMBOL_PLAY);
    else if (s_clock_running)
      lv_label_set_text(s_clock_play_lbl, LV_SYMBOL_PAUSE);
    else
      lv_label_set_text(s_clock_play_lbl, LV_SYMBOL_PLAY);
  }
}

static void clockTimerCb(lv_timer_t* t) {
  (void)t;
  if (!s_clock_running || s_no_clock) return;
  if (s_white_active) {
    if (s_white_time_sec > 0) s_white_time_sec--;
  } else {
    if (s_black_time_sec > 0) s_black_time_sec--;
  }
  updateClockDisplay();
  updateClockOnlyDisplay();
}

// ---------------------------------------------------------------------------
// Move history (HvH)
// ---------------------------------------------------------------------------

static void updateMoveList() {
  if (!s_movelist_table) return;
  int rows = (s_move_count + 1) / 2;
  if (rows == 0) rows = 1;
  lv_table_set_row_cnt(s_movelist_table, rows);
  lv_table_set_col_cnt(s_movelist_table, 3);
  for (int i = 0; i < s_move_count; i += 2) {
    int row = i / 2;
    char num_str[8];
    snprintf(num_str, sizeof(num_str), "%d.", row + 1);
    lv_table_set_cell_value(s_movelist_table, row, 0, num_str);
    lv_table_set_cell_value(s_movelist_table, row, 1, s_move_list[i]);
    if (i + 1 < s_move_count)
      lv_table_set_cell_value(s_movelist_table, row, 2, s_move_list[i + 1]);
    else
      lv_table_set_cell_value(s_movelist_table, row, 2, "");
  }
  if (s_movelist_box)
    lv_obj_scroll_to_y(s_movelist_box, LV_COORD_MAX, LV_ANIM_ON);

  // Render 180°-rotated black move list canvas (latest moves, 3-column table format)
  if (s_blk_ml_src_canvas && s_blk_ml_canvas && s_blk_ml_scratch && s_blk_ml_buf &&
      s_blk_ml_w > 0 && s_blk_ml_h > 0) {
    lv_color_t bg = lv_color_hex(0x262421);
    lv_canvas_fill_bg(s_blk_ml_src_canvas, bg, LV_OPA_COVER);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_montserrat_14;
    dsc.color = lv_color_hex(0x999999);
    dsc.align = LV_TEXT_ALIGN_LEFT;
    // 3-column layout matching player 1's table proportions
    int pad = 4;
    int usable_w = s_blk_ml_w - 2 * pad;
    int col0_w = usable_w * 2 / 10; // move number
    int col1_w = usable_w * 4 / 10; // white move
    int col2_w = usable_w * 4 / 10; // black move
    int col0_x = pad;
    int col1_x = col0_x + col0_w;
    int col2_x = col1_x + col1_w;
    // Row height ~18px (montserrat_14)
    int row_h = 18;
    int usable_h = s_blk_ml_h - 2 * pad;
    int max_lines = usable_h / row_h;
    if (max_lines < 1) max_lines = 1;
    int total_rows = (s_move_count + 1) / 2;
    int start_row = total_rows - max_lines;
    if (start_row < 0) start_row = 0;
    // Draw each row with 3 columns
    for (int r = start_row; r < total_rows; r++) {
      int y = pad + (r - start_row) * row_h;
      int i = r * 2;
      char num_str[8];
      snprintf(num_str, sizeof(num_str), "%d.", r + 1);
      lv_canvas_draw_text(s_blk_ml_src_canvas, col0_x, y, col0_w, &dsc, num_str);
      lv_canvas_draw_text(s_blk_ml_src_canvas, col1_x, y, col1_w, &dsc, s_move_list[i]);
      if (i + 1 < s_move_count)
        lv_canvas_draw_text(s_blk_ml_src_canvas, col2_x, y, col2_w, &dsc, s_move_list[i + 1]);
    }
    rotateBuf180(s_blk_ml_scratch, s_blk_ml_w, s_blk_ml_h, s_blk_ml_buf);
    lv_obj_invalidate(s_blk_ml_canvas);
  }
}

static void updateCapturedPieces(const char* fen) {
  if (!fen || !s_white_cap_bar) return;
  // Count current pieces in FEN
  // Index: 0=Q, 1=R, 2=B, 3=N, 4=P (most valuable first)
  int w_cur[] = {0, 0, 0, 0, 0};
  int b_cur[] = {0, 0, 0, 0, 0};
  for (const char* p = fen; *p && *p != ' '; p++) {
    switch (*p) {
      case 'Q':
        w_cur[0]++;
        break;
      case 'R':
        w_cur[1]++;
        break;
      case 'B':
        w_cur[2]++;
        break;
      case 'N':
        w_cur[3]++;
        break;
      case 'P':
        w_cur[4]++;
        break;
      case 'q':
        b_cur[0]++;
        break;
      case 'r':
        b_cur[1]++;
        break;
      case 'b':
        b_cur[2]++;
        break;
      case 'n':
        b_cur[3]++;
        break;
      case 'p':
        b_cur[4]++;
        break;
    }
  }

  static const int START[] = {1, 2, 2, 2, 8}; // Q R B N P
  static const int VALS[] = {9, 5, 3, 3, 1};
  // Black piece images (shown when white captures from black)
  static const lv_img_dsc_t* B_IMGS[] = {&bQ, &bR, &bB, &bN, &bP};
  // White piece images (shown when black captures from white)
  static const lv_img_dsc_t* W_IMGS[] = {&wQ, &wR, &wB, &wN, &wP};

  int w_idx = 0, b_idx = 0;
  int w_val = 0, b_val = 0;

  for (int i = 0; i < 5; i++) {
    int w_cap = START[i] - b_cur[i]; // white captured from black
    int b_cap = START[i] - w_cur[i]; // black captured from white
    if (w_cap < 0) w_cap = 0;
    if (b_cap < 0) b_cap = 0;
    w_val += w_cap * VALS[i];
    b_val += b_cap * VALS[i];

    for (int j = 0; j < w_cap && w_idx < MAX_CAPTURES; j++) {
      lv_img_set_src(s_wcap_imgs[w_idx], B_IMGS[i]);
      lv_img_set_zoom(s_wcap_imgs[w_idx], CAP_IMG_ZOOM);
      lv_obj_set_pos(s_wcap_imgs[w_idx], w_idx * CAP_PIECE_SIZE, 1);
      lv_obj_clear_flag(s_wcap_imgs[w_idx], LV_OBJ_FLAG_HIDDEN);
      w_idx++;
    }
    for (int j = 0; j < b_cap && b_idx < MAX_CAPTURES; j++) {
      lv_img_set_src(s_bcap_imgs[b_idx], W_IMGS[i]);
      lv_img_set_zoom(s_bcap_imgs[b_idx], CAP_IMG_ZOOM);
      lv_obj_set_pos(s_bcap_imgs[b_idx], b_idx * CAP_PIECE_SIZE, 1);
      lv_img_set_angle(s_bcap_imgs[b_idx], 1800); // rotate for black player
      lv_obj_clear_flag(s_bcap_imgs[b_idx], LV_OBJ_FLAG_HIDDEN);
      b_idx++;
    }
  }

  // Hide remaining slots
  for (int i = w_idx; i < MAX_CAPTURES; i++)
    lv_obj_add_flag(s_wcap_imgs[i], LV_OBJ_FLAG_HIDDEN);
  for (int i = b_idx; i < MAX_CAPTURES; i++)
    lv_obj_add_flag(s_bcap_imgs[i], LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Highlight
// ---------------------------------------------------------------------------

void chess_ui_reset_highlight() {
  if (s_hl_from_r >= 0) {
    lv_obj_set_style_bg_color(s_btns[s_hl_from_r][s_hl_from_c],
                              squareColor(s_hl_from_r, s_hl_from_c), 0);
    s_hl_from_r = -1;
  }
  if (s_hl_to_r >= 0) {
    lv_obj_set_style_bg_color(s_btns[s_hl_to_r][s_hl_to_c],
                              squareColor(s_hl_to_r, s_hl_to_c), 0);
    s_hl_to_r = -1;
  }
}

static void highlightSquare(int r, int c) {
  bool dark = ((r + c) % 2 != 0);
  lv_obj_set_style_bg_color(s_btns[r][c],
                            dark ? s_highlight_dark : s_highlight_light, 0);
}

void chess_ui_set_move(int fr, int fc, int tr, int tc, const char* text) {
  chess_ui_reset_highlight();
  // Map logical coords to display coords when board is flipped
  if (s_board_flipped) {
    fr = 7 - fr; fc = 7 - fc;
    tr = 7 - tr; tc = 7 - tc;
  }
  s_hl_from_r = fr;
  s_hl_from_c = fc;
  s_hl_to_r = tr;
  s_hl_to_c = tc;
  highlightSquare(fr, fc);
  highlightSquare(tr, tc);
  if (text) lv_label_set_text(s_status_label, text);
}

// ---------------------------------------------------------------------------
// FEN rendering — piece images
// ---------------------------------------------------------------------------

static const lv_img_dsc_t* fen_char_to_img(char ch) {
  switch (ch) {
    case 'K':
      return &wK;
    case 'Q':
      return &wQ;
    case 'R':
      return &wR;
    case 'B':
      return &wB;
    case 'N':
      return &wN;
    case 'P':
      return &wP;
    case 'k':
      return &bK;
    case 'q':
      return &bQ;
    case 'r':
      return &bR;
    case 'b':
      return &bB;
    case 'n':
      return &bN;
    case 'p':
      return &bP;
    default:
      return nullptr;
  }
}

void chess_ui_render_fen(const char* fen) {
  if (!fen) return;
  // Store FEN for re-render on swap
  strncpy(s_last_fen, fen, sizeof(s_last_fen) - 1);
  s_last_fen[sizeof(s_last_fen) - 1] = '\0';

  int r = 0, c = 0;
  for (const char* p = fen; *p && r < 8; p++) {
    char ch = *p;
    if (ch == '/') {
      r++;
      c = 0;
      continue;
    }
    if (ch == ' ') break; // end of board part
    // Map FEN row/col to display row/col (flip if board is flipped)
    if (ch >= '1' && ch <= '8') {
      int empties = ch - '0';
      for (int e = 0; e < empties && c < 8; e++) {
        int dr = s_board_flipped ? 7 - r : r;
        int dc = s_board_flipped ? 7 - c : c;
        lv_obj_add_flag(s_labels[dr][dc], LV_OBJ_FLAG_HIDDEN);
        c++;
      }
    } else {
      int dr = s_board_flipped ? 7 - r : r;
      int dc = s_board_flipped ? 7 - c : c;
      const lv_img_dsc_t* img = fen_char_to_img(ch);
      if (img) {
        lv_img_set_src(s_labels[dr][dc], img);
        lv_img_set_zoom(s_labels[dr][dc], s_piece_zoom);
        // In HvH mode, rotate opponent's pieces 180° to face them
        bool is_black_piece = (ch >= 'a' && ch <= 'z');
        bool rotate = false;
        if (s_current_mode == 1) {
          rotate = s_board_flipped ? !is_black_piece : is_black_piece;
        }
        lv_img_set_angle(s_labels[dr][dc], rotate ? 1800 : 0);
        lv_obj_clear_flag(s_labels[dr][dc], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_labels[dr][dc], LV_OBJ_FLAG_HIDDEN);
      }
      c++;
    }
  }
  updateCapturedPieces(fen);
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

// Parse a UCI move string (e.g., "e2e4") into row/col coords.
// Returns true on success.
static bool parseUci(const char* uci, int* fr, int* fc, int* tr, int* tc) {
  if (!uci || strlen(uci) < 4) return false;
  char ff = uci[0], fk = uci[1], tf = uci[2], tk = uci[3];
  if (ff < 'a' || ff > 'h' || tf < 'a' || tf > 'h') return false;
  if (fk < '1' || fk > '8' || tk < '1' || tk > '8') return false;
  *fc = ff - 'a';
  *tc = tf - 'a';
  *fr = 8 - (fk - '0');
  *tr = 8 - (tk - '0');
  return true;
}

void chess_ui_handle_message(const char* line) {
  if (!line) return;

  // Find the '|' separator between type and payload
  const char* pipe = strchr(line, '|');
  int type_len = pipe ? (int)(pipe - line) : (int)strlen(line);
  const char* payload = pipe ? pipe + 1 : "";

  if (type_len == 5 && strncmp(line, "STATE", 5) == 0) {
    // Look for fen=...
    const char* fen_key = strstr(payload, "fen=");
    if (fen_key) {
      const char* fen_start = fen_key + 4;
      // FEN ends at ';' or end of string
      const char* fen_end = strchr(fen_start, ';');
      int fen_len = fen_end ? (int)(fen_end - fen_start) : (int)strlen(fen_start);
      char fen_buf[128];
      if (fen_len > 0 && fen_len < (int)sizeof(fen_buf)) {
        strncpy(fen_buf, fen_start, fen_len);
        fen_buf[fen_len] = '\0';
        chess_ui_render_fen(fen_buf);
      }
    }
    // Look for move=...
    const char* move_key = strstr(payload, "move=");
    if (move_key) {
      const char* move_start = move_key + 5;
      const char* move_end = strchr(move_start, ';');
      int move_len = move_end ? (int)(move_end - move_start)
                              : (int)strlen(move_start);
      char move_buf[16];
      if (move_len > 0 && move_len < (int)sizeof(move_buf)) {
        strncpy(move_buf, move_start, move_len);
        move_buf[move_len] = '\0';
        int fr, fc, tr, tc;
        if (parseUci(move_buf, &fr, &fc, &tr, &tc)) {
          chess_ui_set_move(fr, fc, tr, tc, move_buf);
          // Track move in history (HvH)
          if (s_move_count < MAX_MOVE_HISTORY) {
            strncpy(s_move_list[s_move_count], move_buf, 7);
            s_move_list[s_move_count][7] = '\0';
            s_move_count++;
            updateMoveList();
          }
          // Hide swap button after first move
          if (s_swap_btn && s_move_count == 1 && s_current_mode == 1)
            lv_obj_add_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);
          // Clock: start on first move, add increment, switch sides
          if (!s_no_clock) {
            if (!s_clock_started) {
              s_clock_started = true;
              s_clock_running = true;
            } else if (s_clock_increment_sec > 0) {
              // Add increment to the player who just moved
              if (s_white_active)
                s_white_time_sec += s_clock_increment_sec;
              else
                s_black_time_sec += s_clock_increment_sec;
            }
            s_white_active = !s_white_active;
            updateClockDisplay();
          }
        }
      }
    }
  } else if (type_len == 4 && strncmp(line, "HINT", 4) == 0) {
    const char* move_key = strstr(payload, "move=");
    if (move_key) {
      const char* move_start = move_key + 5;
      const char* move_end = strchr(move_start, ';');
      int move_len = move_end ? (int)(move_end - move_start)
                              : (int)strlen(move_start);
      char move_buf[16];
      if (move_len > 0 && move_len < (int)sizeof(move_buf)) {
        strncpy(move_buf, move_start, move_len);
        move_buf[move_len] = '\0';
        int fr, fc, tr, tc;
        if (parseUci(move_buf, &fr, &fc, &tr, &tc)) {
          char hint_text[32];
          snprintf(hint_text, sizeof(hint_text), "Hint: %s", move_buf);
          chess_ui_set_move(fr, fc, tr, tc, hint_text);
        }
      }
    }
  } else if (type_len == 5 && strncmp(line, "ERROR", 5) == 0) {
    lv_label_set_text(s_status_label, "Error");
  } else if (type_len == 4 && strncmp(line, "MODE", 4) == 0) {
    // MODE|value=N — master tells us which mode was selected
    const char* val_key = strstr(payload, "value=");
    if (val_key) {
      int mode = val_key[6] - '0';
      s_current_mode = mode;
      if (mode == 0) {
        // Back to selection screen
        chess_ui_show_welcome();
      } else if (mode >= 1 && mode <= 5) {
        chess_ui_show_game(MODE_NAMES[mode]);
      }
    }
  } else if (type_len == 5 && strncmp(line, "CLOCK", 5) == 0) {
    // CLOCK — show the clock setup screen
    if (s_game_screen) lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_clock_screen) lv_obj_clear_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---------------------------------------------------------------------------
// Practice mode — built-in engine with touch gameplay
// ---------------------------------------------------------------------------

// Sync engine state → LVGL board
static void practice_render_board() {
    char fen[80];
    me_board_fen(&s_practice_state, fen, sizeof(fen));
    chess_ui_render_fen(fen);
}

// Clear legal move indicators (restore square colors)
static void practice_clear_indicators() {
    if (s_practice_selected_sq < 0 && s_practice_legal_count == 0) return;
    // Restore selected square
    if (s_practice_selected_sq >= 0) {
        int sr = s_practice_selected_sq / 8;
        int sc = s_practice_selected_sq % 8;
        int dr = s_board_flipped ? 7 - sr : sr;
        int dc = s_board_flipped ? 7 - sc : sc;
        lv_obj_set_style_bg_color(s_btns[dr][dc], squareColor(dr, dc), 0);
    }
    // Restore legal move squares
    for (int i = 0; i < s_practice_legal_count; i++) {
        int tr = s_practice_legal[i].to / 8;
        int tc = s_practice_legal[i].to % 8;
        int dr = s_board_flipped ? 7 - tr : tr;
        int dc = s_board_flipped ? 7 - tc : tc;
        lv_obj_set_style_bg_color(s_btns[dr][dc], squareColor(dr, dc), 0);
    }
    s_practice_selected_sq = -1;
    s_practice_legal_count = 0;
}

// Show legal move indicators for a selected piece
static void practice_show_legal_moves(int sq) {
    practice_clear_indicators();
    s_practice_selected_sq = sq;

    // Highlight selected square with cyan tint
    int sr = sq / 8, sc = sq % 8;
    int dsr = s_board_flipped ? 7 - sr : sr;
    int dsc = s_board_flipped ? 7 - sc : sc;
    lv_obj_set_style_bg_color(s_btns[dsr][dsc], lv_color_hex(0x44CCCC), 0);

    // Get all legal moves, filter those from this square
    me_move_t all_moves[ME_MAX_MOVES];
    int n = me_generate_moves(&s_practice_state, all_moves);
    s_practice_legal_count = 0;
    for (int i = 0; i < n; i++) {
        if (all_moves[i].from == (uint8_t)sq) {
            s_practice_legal[s_practice_legal_count++] = all_moves[i];
        }
    }

    // Show indicators on destination squares
    for (int i = 0; i < s_practice_legal_count; i++) {
        int tr = s_practice_legal[i].to / 8;
        int tc = s_practice_legal[i].to % 8;
        int dr = s_board_flipped ? 7 - tr : tr;
        int dc = s_board_flipped ? 7 - tc : tc;
        if (s_practice_legal[i].flags & ME_FLAG_CAPTURE) {
            lv_obj_set_style_bg_color(s_btns[dr][dc], lv_color_hex(LEGAL_CAPTURE_CLR), 0);
        } else {
            // Blend a dot color with the square
            bool dark = (dr + dc) % 2 != 0;
            lv_obj_set_style_bg_color(s_btns[dr][dc],
                dark ? lv_color_hex(0x6B9E23) : lv_color_hex(0xAAD751), 0);
        }
    }
}

// Update practice status label
static void practice_update_status() {
    if (!s_practice_status_lbl) return;
    if (s_practice_game_over) return; // keep game-over message

    bool in_check = me_in_check(&s_practice_state);
    bool is_player_turn = (s_practice_state.side == ME_WHITE) == s_practice_player_white;
    if (is_player_turn) {
        if (in_check)
            lv_label_set_text(s_practice_status_lbl, "Your turn — Check!");
        else
            lv_label_set_text(s_practice_status_lbl, "Your turn");
    } else {
        lv_label_set_text(s_practice_status_lbl, "Engine thinking...");
    }
}

// Check for game end
static bool practice_check_game_end() {
    if (me_is_checkmate(&s_practice_state)) {
        s_practice_game_over = true;
        s_clock_running = false;
        bool winner_is_white = (s_practice_state.side == ME_BLACK);
        if (winner_is_white == s_practice_player_white) {
            if (s_practice_status_lbl)
                lv_label_set_text(s_practice_status_lbl, "Checkmate — You win!");
        } else {
            if (s_practice_status_lbl)
                lv_label_set_text(s_practice_status_lbl, "Checkmate — Engine wins!");
        }
        return true;
    }
    if (me_is_stalemate(&s_practice_state)) {
        s_practice_game_over = true;
        s_clock_running = false;
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "Stalemate — Draw");
        return true;
    }
    if (s_practice_state.halfmove >= 100) {
        s_practice_game_over = true;
        s_clock_running = false;
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "50-move rule — Draw");
        return true;
    }
    return false;
}

// Apply a completed engine move to the board (called when search result is ready)
static void practice_apply_engine_move(me_move_t best) {
    if (best.from == 0 && best.to == 0 && best.promotion == 0 && best.flags == 0) {
        practice_check_game_end();
        return;
    }

    char uci[6];
    me_move_to_uci(best, uci, sizeof(uci));
    if (s_move_count < MAX_MOVE_HISTORY) {
        strncpy(s_move_list[s_move_count], uci, 7);
        s_move_list[s_move_count][7] = '\0';
        s_move_count++;
        updateMoveList();
    }

    me_make_move(&s_practice_state, best);

    int fr = best.from / 8, fc = best.from % 8;
    int tr = best.to / 8, tc = best.to % 8;
    practice_render_board();
    chess_ui_set_move(fr, fc, tr, tc, uci);

    if (!s_no_clock) {
        s_white_active = (s_practice_state.side == ME_WHITE);
        if (s_clock_increment_sec > 0) {
            if (s_practice_state.side == ME_WHITE)
                s_black_time_sec += s_clock_increment_sec;
            else
                s_white_time_sec += s_clock_increment_sec;
        }
        updateClockDisplay();
    }

    practice_check_game_end();
    if (!s_practice_game_over) practice_update_status();
}

// LVGL timer callback: polls for a completed engine search result (runs in LVGL task)
static void practice_engine_poll_timer_cb(lv_timer_t* t) {
    (void)t;
#ifndef SIMULATOR
    me_move_t result;
    if (poll_engine_result(&result)) {
        practice_apply_engine_move(result);
    }
#endif
}

// Trigger the engine to respond after player's move.
// Returns immediately — result is applied asynchronously via the poll timer.
static void practice_engine_respond() {
    if (s_practice_game_over) return;
    practice_update_status(); // shows "Thinking..."
#ifdef SIMULATOR
    // Simulator: synchronous (no FreeRTOS)
    me_move_t best = me_search(&s_practice_state, s_practice_depth);
    practice_apply_engine_move(best);
#else
    start_engine_search(&s_practice_state, s_practice_depth);
    // Poll timer will fire every 50ms to check for result
#endif
}

// ===========================================================================
// Guided opening trainer
// ===========================================================================

// Show/hide trainer-specific vs practice-specific control widgets.
static void trainer_set_controls(bool trainer_mode) {
    auto vis = [](lv_obj_t* o, bool show) {
        if (!o) return;
        if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    vis(s_practice_undo_btn,   !trainer_mode);
    vis(s_practice_swap_btn,   !trainer_mode);
    vis(s_practice_diff_minus, !trainer_mode);
    vis(s_practice_diff_plus,  !trainer_mode);
    vis(s_practice_diff_lbl,   !trainer_mode);
    vis(s_trainer_progress_lbl, trainer_mode);
    // Hint button is only shown in quiz mode — handled separately
    if (s_trainer_hint_btn) lv_obj_add_flag(s_trainer_hint_btn, LV_OBJ_FLAG_HIDDEN);
}

// Restore the hint squares to their default board colour.
static void trainer_clear_hint_squares() {
    if (s_trainer_hint_from >= 0) {
        int r = s_trainer_hint_from / 8, c = s_trainer_hint_from % 8;
        lv_obj_set_style_bg_color(s_btns[r][c], squareColor(r, c), 0);
        s_trainer_hint_from = -1;
    }
    if (s_trainer_hint_to >= 0) {
        int r = s_trainer_hint_to / 8, c = s_trainer_hint_to % 8;
        lv_obj_set_style_bg_color(s_btns[r][c], squareColor(r, c), 0);
        s_trainer_hint_to = -1;
    }
}

// Colour from-square cyan and to-square green for the current step.
static void trainer_show_hint_squares() {
    trainer_clear_hint_squares();
    if (s_trainer_step >= s_trainer_total) return;
    me_move_t& m = s_trainer_moves[s_trainer_step];
    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to   / 8, tc = m.to   % 8;
    int dfr = s_board_flipped ? 7 - fr : fr, dfc = s_board_flipped ? 7 - fc : fc;
    int dtr = s_board_flipped ? 7 - tr : tr, dtc = s_board_flipped ? 7 - tc : tc;
    lv_obj_set_style_bg_color(s_btns[dfr][dfc], lv_color_hex(0x00BBBB), 0); // cyan
    lv_obj_set_style_bg_color(s_btns[dtr][dtc], lv_color_hex(0x22BB44), 0); // green
    s_trainer_hint_from = dfr * 8 + dfc;
    s_trainer_hint_to   = dtr * 8 + dtc;
}

// Update the progress / mode label.
static void trainer_update_progress() {
    if (!s_trainer_progress_lbl) return;
    if (s_trainer_total == 0) {
        lv_label_set_text(s_trainer_progress_lbl, "Starting position");
        return;
    }
    char buf[56];
    const char* side = (s_practice_state.side == ME_WHITE) ? "White" : "Black";
    snprintf(buf, sizeof(buf), "Move %d/%d  %s  [%s]",
             s_trainer_step + 1, s_trainer_total,
             side,
             s_trainer_guided ? "Guided" : "Quiz");
    lv_label_set_text(s_trainer_progress_lbl, buf);
}

// Timer: restore board after a wrong-move flash, then re-show guided hints.
static void trainer_restore_after_error_cb(lv_timer_t* t) {
    lv_timer_del(t);
    practice_render_board();
    if (s_trainer_guided) {
        trainer_show_hint_squares();
    }
    s_trainer_sel_sq = -1;
    if (s_practice_status_lbl) {
        trainer_update_progress(); // resets label text via progress
        // brief re-draw handled above
    }
}

// Timer: remove temporary hint after 2 s (quiz-mode hint button).
static void trainer_hide_hint_cb(lv_timer_t* t) {
    lv_timer_del(t);
    if (s_trainer_active && !s_trainer_guided) {
        trainer_clear_hint_squares();
    }
}

// Timer: restart the opening in quiz mode after completion.
static void trainer_restart_quiz_cb(lv_timer_t* t) {
    lv_timer_del(t);
    s_trainer_guided = false;
    s_trainer_step   = 0;
    s_trainer_sel_sq = -1;
    me_init(&s_practice_state);
    s_board_flipped = false;
    practice_render_board();
    practice_clear_indicators();
    trainer_clear_hint_squares();
    trainer_update_progress();
    // Show hint button in quiz mode
    if (s_trainer_hint_btn) lv_obj_clear_flag(s_trainer_hint_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_practice_status_lbl) lv_label_set_text(s_practice_status_lbl, "Find the move!");
}

// Called when all moves in the opening have been played correctly.
static void trainer_on_complete() {
    trainer_clear_hint_squares();
    if (s_practice_status_lbl)
        lv_label_set_text(s_practice_status_lbl,
            LV_SYMBOL_OK " Complete! Restarting in quiz mode...");
    if (s_trainer_progress_lbl)
        lv_label_set_text(s_trainer_progress_lbl, "Opening complete!");
    lv_timer_create(trainer_restart_quiz_cb, 2500, nullptr);
}

// Auto-play timer callback for black moves (declared before trainer_advance
// so the timer can recurse via lv_timer_create from inside trainer_advance).
static void trainer_autoplay_cb(lv_timer_t* t);

// Apply a confirmed correct move and advance the step counter.
static void trainer_advance(me_move_t move) {
    trainer_clear_hint_squares();
    me_make_move(&s_practice_state, move);
    s_trainer_sel_sq = -1;
    s_trainer_step++;
    practice_render_board();
    // Show last-move highlight (yellow tint) for the move just played
    chess_ui_set_move(move.from / 8, move.from % 8,
                      move.to   / 8, move.to   % 8, nullptr);

    if (s_trainer_step >= s_trainer_total) {
        trainer_on_complete();
        return;
    }
    trainer_update_progress();

    // Auto-play black: if the next move belongs to black, schedule it.
    // The user only ever plays the white side in trainer mode.
    if (s_practice_state.side == ME_BLACK) {
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "Opponent thinking...");
        lv_timer_t* tmr = lv_timer_create(trainer_autoplay_cb, 700, nullptr);
        lv_timer_set_repeat_count(tmr, 1);
        return;
    }

    // White to move — re-show hints / status for the user.
    if (s_trainer_guided) {
        trainer_show_hint_squares();
    }
    if (s_practice_status_lbl) {
        if (!s_trainer_guided)
            lv_label_set_text(s_practice_status_lbl, "Find the move!");
        // Guided mode — status is shown via trainer_update_progress
    }
}

static void trainer_autoplay_cb(lv_timer_t* t) {
    lv_timer_del(t);
    if (!s_trainer_active) return;
    if (s_trainer_step >= s_trainer_total) return;
    if (s_practice_state.side != ME_BLACK) return;  // safety
    trainer_advance(s_trainer_moves[s_trainer_step]);
}

// Parse the UCI move sequence for opening [idx] into s_trainer_moves[].
// Returns the number of moves parsed.
static int trainer_parse_opening(int idx) {
    me_state_t st;
    me_init(&st);
    int count = 0;
    for (int i = 0; i < 39 && OPENINGS[idx].moves[i] != nullptr; i++) {
        me_move_t m = {};
        if (!me_parse_uci(&st, OPENINGS[idx].moves[i], &m)) break;
        s_trainer_moves[count++] = m;
        me_make_move(&st, m);
    }
    return count;
}

// Derive the FEN that results from playing through opening [idx] from start.
// Used by "Play vs Engine" button.
static void opening_derive_fen(int idx, char* buf, int buf_size) {
    me_state_t st;
    me_init(&st);
    for (int i = 0; i < 39 && OPENINGS[idx].moves[i] != nullptr; i++) {
        me_move_t m = {};
        if (!me_parse_uci(&st, OPENINGS[idx].moves[i], &m)) break;
        me_make_move(&st, m);
    }
    me_full_fen(&st, buf, buf_size);
}

// Enter guided/quiz trainer mode for opening [idx].
static void trainer_enter(int idx, bool guided) {
    abort_engine_search();
    s_trainer_active  = true;
    s_trainer_idx     = idx;
    s_trainer_guided  = guided;
    s_trainer_step    = 0;
    s_trainer_sel_sq  = -1;
    s_trainer_total   = trainer_parse_opening(idx);
    s_selected_opening = idx;

    me_init(&s_practice_state);
    s_practice_game_over = false;
    s_board_flipped = false;
    practice_render_board();
    chess_ui_reset_highlight();
    practice_clear_indicators();
    trainer_clear_hint_squares();

    trainer_set_controls(true);
    trainer_update_progress();

    if (s_trainer_total == 0) {
        // "Starting Position" — nothing to train, just drop into free play
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "No moves to train");
        return;
    }

    if (guided) {
        if (s_trainer_hint_btn) lv_obj_add_flag(s_trainer_hint_btn, LV_OBJ_FLAG_HIDDEN);
        trainer_show_hint_squares();
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl,
                "Follow the highlighted squares");
    } else {
        if (s_trainer_hint_btn) lv_obj_clear_flag(s_trainer_hint_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "Find the move!");
    }
}

// Hint button callback: temporarily show from/to squares for 2 s.
static void trainer_hint_btn_cb(lv_event_t* e) {
    (void)e;
    if (!s_trainer_active || s_trainer_step >= s_trainer_total) return;
    trainer_show_hint_squares();
    lv_timer_create(trainer_hide_hint_cb, 2000, nullptr);
}

// Handle a board tap in trainer mode.
static void trainer_cell_tap(int r, int c) {
    if (s_trainer_step >= s_trainer_total) return;
    // Black moves are auto-played — ignore taps while it's black's turn.
    if (s_practice_state.side != ME_WHITE) return;

    int sq = r * 8 + c;
    int dsq = (s_board_flipped ? (7 - r) : r) * 8 + (s_board_flipped ? (7 - c) : c);
    (void)dsq; // logical sq is used for move matching

    me_move_t expected = s_trainer_moves[s_trainer_step];

    // === No piece selected yet ===
    if (s_trainer_sel_sq < 0) {
        uint8_t piece = s_practice_state.board[sq];
        if (piece == ME_EMPTY) return;
        // Allow selecting any piece that belongs to the side to move
        bool white_piece = ME_IS_WHITE(piece);
        bool white_turn  = (s_practice_state.side == ME_WHITE);
        if (white_piece != white_turn) return;

        s_trainer_sel_sq = sq;
        // Highlight selected square cyan (on top of existing hints)
        int dr = s_board_flipped ? 7 - r : r;
        int dc = s_board_flipped ? 7 - c : c;
        lv_obj_set_style_bg_color(s_btns[dr][dc], lv_color_hex(0x44CCCC), 0);
        return;
    }

    // === Piece already selected ===

    // Tapping the same square → deselect
    if (sq == s_trainer_sel_sq) {
        s_trainer_sel_sq = -1;
        practice_render_board();
        chess_ui_reset_highlight();
        if (s_trainer_guided) trainer_show_hint_squares();
        return;
    }

    // Tapping another own piece → reselect
    uint8_t piece = s_practice_state.board[sq];
    if (piece != ME_EMPTY) {
        bool white_piece = ME_IS_WHITE(piece);
        bool white_turn  = (s_practice_state.side == ME_WHITE);
        if (white_piece == white_turn) {
            s_trainer_sel_sq = sq;
            practice_render_board();
            chess_ui_reset_highlight();
            if (s_trainer_guided) trainer_show_hint_squares();
            int dr = s_board_flipped ? 7 - r : r;
            int dc = s_board_flipped ? 7 - c : c;
            lv_obj_set_style_bg_color(s_btns[dr][dc], lv_color_hex(0x44CCCC), 0);
            return;
        }
    }

    // Validate against expected move
    bool from_ok = ((uint8_t)s_trainer_sel_sq == expected.from);
    bool to_ok   = ((uint8_t)sq               == expected.to);

    if (from_ok && to_ok) {
        // Correct! Advance to next step.
        trainer_advance(expected);
    } else {
        // Wrong move — flash destination square red, then restore.
        int dr = s_board_flipped ? 7 - r : r;
        int dc = s_board_flipped ? 7 - c : c;
        lv_obj_set_style_bg_color(s_btns[dr][dc], lv_color_hex(0xCC2222), 0);
        s_trainer_sel_sq = -1;
        if (s_practice_status_lbl)
            lv_label_set_text(s_practice_status_lbl, "Wrong — try again");
        lv_timer_create(trainer_restore_after_error_cb, 700, nullptr);
    }
}

// ===========================================================================
// Handle cell tap in practice mode
static void practice_cell_tap(int r, int c) {
    // Route to trainer if active
    if (s_trainer_active) {
        trainer_cell_tap(r, c);
        return;
    }

    if (s_practice_game_over) return;
    if (s_engine_thinking) return; // ignore taps while engine is thinking

    // Only allow moves on player's turn
    bool is_player_turn = (s_practice_state.side == ME_WHITE) == s_practice_player_white;
    if (!is_player_turn) return;

    int sq = r * 8 + c;

    // If a piece is selected, check if this tap is a legal destination
    if (s_practice_selected_sq >= 0) {
        for (int i = 0; i < s_practice_legal_count; i++) {
            if (s_practice_legal[i].to == (uint8_t)sq) {
                // Legal move found — execute it
                me_move_t move = s_practice_legal[i];

                // Handle promotion: default to queen (TODO: prompt user)
                if (move.flags & ME_FLAG_PROMOTION) {
                    // Find the queen-promotion variant
                    for (int j = 0; j < s_practice_legal_count; j++) {
                        if (s_practice_legal[j].to == (uint8_t)sq &&
                            s_practice_legal[j].promotion == ME_QUEEN) {
                            move = s_practice_legal[j];
                            break;
                        }
                    }
                }

                // Save state for undo (save the position before player + engine pair)
                if (s_practice_undo_count < 60)
                    s_practice_undo_stack[s_practice_undo_count++] = s_practice_state;

                practice_clear_indicators();

                // Track player move in move list
                char uci[6];
                me_move_to_uci(move, uci, sizeof(uci));
                if (s_move_count < MAX_MOVE_HISTORY) {
                    strncpy(s_move_list[s_move_count], uci, 7);
                    s_move_list[s_move_count][7] = '\0';
                    s_move_count++;
                    updateMoveList();
                }

                me_make_move(&s_practice_state, move);
                practice_render_board();
                chess_ui_set_move(move.from / 8, move.from % 8,
                                 move.to / 8, move.to % 8, uci);

                // Start clock on first move
                if (!s_no_clock) {
                    if (!s_clock_started) {
                        s_clock_started = true;
                        s_clock_running = true;
                    } else if (s_clock_increment_sec > 0) {
                        if (s_practice_state.side == ME_WHITE)
                            s_black_time_sec += s_clock_increment_sec;
                        else
                            s_white_time_sec += s_clock_increment_sec;
                    }
                    s_white_active = (s_practice_state.side == ME_WHITE);
                    updateClockDisplay();
                }

                // Hide swap button after first move
                if (s_swap_btn && s_move_count == 1)
                    lv_obj_add_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);

                if (!practice_check_game_end()) {
                    practice_engine_respond();
                }
                return;
            }
        }
        // Tap was not a legal destination — deselect or select new piece
        practice_clear_indicators();
    }

    // Check if tapping own piece to select it
    uint8_t piece = s_practice_state.board[sq];
    if (piece != ME_EMPTY) {
        bool is_own = (s_practice_player_white && ME_IS_WHITE(piece)) ||
                      (!s_practice_player_white && ME_IS_BLACK(piece));
        if (is_own) {
            practice_show_legal_moves(sq);
            return;
        }
    }

    // Tap on empty or opponent piece with nothing selected — ignore
    practice_clear_indicators();
}

// Practice mode: start a new game from the given FEN (nullptr = starting position)
static void practice_new_game(const char* fen = nullptr) {
    // If the engine is mid-search, abort it cleanly before resetting state.
    // (abort_engine_search feeds the done semaphore so the engine task stays healthy)
    abort_engine_search();
    // Leaving trainer mode (this function is only called for play-vs-engine)
    if (s_trainer_active) {
        s_trainer_active = false;
        trainer_clear_hint_squares();
        trainer_set_controls(false);
    }
    if (fen && fen[0]) {
        if (!me_load_fen(&s_practice_state, fen))
            me_init(&s_practice_state); // fallback if FEN invalid
    } else {
        me_init(&s_practice_state);
    }
    // Determine who plays based on side to move in the FEN
    // (player plays the side that moves first in the loaded position)
    s_practice_player_white = (s_practice_state.side == ME_WHITE);
    s_practice_selected_sq = -1;
    s_practice_legal_count = 0;
    s_practice_game_over = false;
    s_practice_undo_count = 0;
    s_board_flipped = !s_practice_player_white;
    s_move_count = 0;
    memset(s_move_list, 0, sizeof(s_move_list));
    updateMoveList();
    practice_render_board();
    chess_ui_reset_highlight();
    s_white_time_sec = s_clock_initial_sec;
    s_black_time_sec = s_clock_initial_sec;
    s_white_active = true;
    s_clock_running = false;
    s_clock_started = false;
    updateClockDisplay();
    practice_update_status();

    // If engine moves first (player plays black from starting side = black), let it go
    if (!s_practice_player_white) {
        if (s_practice_undo_count < 60)
            s_practice_undo_stack[s_practice_undo_count++] = s_practice_state;
        practice_engine_respond();
    }
}

// Practice mode: undo last move pair (player + engine)
static void practice_undo() {
    if (s_practice_undo_count <= 0 || s_practice_game_over) return;
    s_practice_undo_count--;
    s_practice_state = s_practice_undo_stack[s_practice_undo_count];
    practice_clear_indicators();
    practice_render_board();
    chess_ui_reset_highlight();
    // Remove last 2 moves from move list (player + engine)
    if (s_move_count > 0) s_move_count--;
    if (s_move_count > 0) s_move_count--;
    updateMoveList();
    practice_update_status();
}

// Practice-specific button callbacks
// ---------------------------------------------------------------------------
// Opening picker modal
// ---------------------------------------------------------------------------

// Forward declaration
static void show_opening_picker();

// Encode (index << 1 | action) as user_data: bit0=0 → Train, bit0=1 → Play
static void opening_train_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= OPENING_COUNT) return;
    s_selected_opening = idx;
    if (s_opening_picker) { lv_obj_del(s_opening_picker); s_opening_picker = nullptr; }
    trainer_enter(idx, true); // guided first time
}

static void opening_play_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= OPENING_COUNT) return;
    s_selected_opening = idx;
    if (s_opening_picker) { lv_obj_del(s_opening_picker); s_opening_picker = nullptr; }
    // Derive the FEN from the opening's move sequence and play vs engine
    char fen[120];
    opening_derive_fen(idx, fen, sizeof(fen));
    practice_new_game(fen);
}

static void opening_cancel_cb(lv_event_t* e) {
    (void)e;
    if (s_opening_picker) {
        lv_obj_del(s_opening_picker);
        s_opening_picker = nullptr;
    }
}

static void show_opening_picker() {
    // Remove any existing picker
    if (s_opening_picker) {
        lv_obj_del(s_opening_picker);
        s_opening_picker = nullptr;
    }

    lv_obj_t* scr = lv_scr_act();

    // Full-screen semi-transparent backdrop
    s_opening_picker = lv_obj_create(scr);
    lv_obj_set_size(s_opening_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_opening_picker, 0, 0);
    lv_obj_set_style_bg_color(s_opening_picker, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_opening_picker, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_opening_picker, 0, 0);
    lv_obj_set_style_pad_all(s_opening_picker, 0, 0);
    lv_obj_clear_flag(s_opening_picker, LV_OBJ_FLAG_SCROLLABLE);

    // Card panel
    int card_w = s_screen_w - 40;
    int card_h = s_screen_h - 80;
    lv_obj_t* card = lv_obj_create(s_opening_picker);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x7C6F9F), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Choose Opening");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    // Cancel button (top-right)
    lv_obj_t* cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, 60, 28);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, opening_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(cancel_lbl);

    // Column header
    int header_y = 32;
    lv_obj_t* hdr = lv_label_create(card);
    lv_label_set_text(hdr, "Opening                              Train   Play vs Engine");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hdr, 4, header_y);

    // Scrollable list container
    int list_top = header_y + 18;
    lv_obj_t* list = lv_obj_create(card);
    lv_obj_set_size(list, card_w - 16, card_h - list_top - 8);
    lv_obj_set_pos(list, 0, list_top);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    // Button widths
    const int btn_w  = 56;  // Train / Play buttons
    const int gap    = 4;
    const int row_h  = 42;

    for (int i = 0; i < OPENING_COUNT; i++) {
        bool is_sel = (i == s_selected_opening);

        // Row container
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, row_h);
        lv_obj_set_style_bg_color(row, lv_color_hex(is_sel ? 0x3A2E5E : 0x2E2E4E), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Opening name label (left, takes remaining space)
        lv_obj_t* name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, OPENINGS[i].name);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // "Train" button (green, right side)
        lv_obj_t* train_btn = lv_btn_create(row);
        lv_obj_set_size(train_btn, btn_w, row_h - 10);
        lv_obj_align(train_btn, LV_ALIGN_RIGHT_MID, -(btn_w + gap), 0);
        lv_obj_set_style_bg_color(train_btn, lv_color_hex(0x226622), 0);
        lv_obj_set_style_bg_color(train_btn, lv_color_hex(0x33AA33), LV_STATE_PRESSED);
        lv_obj_set_style_radius(train_btn, 6, 0);
        lv_obj_add_event_cb(train_btn, opening_train_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* train_lbl = lv_label_create(train_btn);
        lv_label_set_text(train_lbl, "Train");
        lv_obj_set_style_text_color(train_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(train_lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(train_lbl);

        // "Play" button (blue, rightmost)
        lv_obj_t* play_btn = lv_btn_create(row);
        lv_obj_set_size(play_btn, btn_w, row_h - 10);
        lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1A3A6A), 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x3366CC), LV_STATE_PRESSED);
        lv_obj_set_style_radius(play_btn, 6, 0);
        lv_obj_add_event_cb(play_btn, opening_play_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* play_lbl = lv_label_create(play_btn);
        lv_label_set_text(play_lbl, "Play");
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(play_lbl);
    }
}

static void practice_new_cb(lv_event_t* e) {
    (void)e;
    show_opening_picker();
}

static void practice_undo_cb(lv_event_t* e) {
    (void)e;
    if (s_trainer_active) return;    // undo not available in trainer
    if (s_engine_thinking) return;   // don't undo while engine is working
    practice_undo();
}

static void practice_diff_up_cb(lv_event_t* e) {
    (void)e;
    if (s_practice_depth < 5) {  // max 5: depth 6 causes quiescence stack overflow
        s_practice_depth++;
        if (s_practice_diff_lbl) {
            char buf[16];
            snprintf(buf, sizeof(buf), "Depth %d", s_practice_depth);
            lv_label_set_text(s_practice_diff_lbl, buf);
        }
    }
}

static void practice_diff_down_cb(lv_event_t* e) {
    (void)e;
    if (s_practice_depth > 1) {
        s_practice_depth--;
        if (s_practice_diff_lbl) {
            char buf[16];
            snprintf(buf, sizeof(buf), "Depth %d", s_practice_depth);
            lv_label_set_text(s_practice_diff_lbl, buf);
        }
    }
}

static void practice_swap_cb(lv_event_t* e) {
    (void)e;
    if (s_trainer_active) return;    // swap not available in trainer
    if (s_engine_thinking) return;   // don't swap while engine is working
    if (s_clock_started) return;
    s_practice_player_white = !s_practice_player_white;
    s_board_flipped = !s_board_flipped;
    practice_render_board();
    chess_ui_reset_highlight();
    practice_update_status();

    // If player is now black, engine plays first
    if (!s_practice_player_white) {
        // Save state for undo
        if (s_practice_undo_count < 60)
            s_practice_undo_stack[s_practice_undo_count++] = s_practice_state;
        practice_engine_respond();
    }
}

// ---------------------------------------------------------------------------
// Cell click callback
// ---------------------------------------------------------------------------

static void cell_event_cb(lv_event_t* e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  int r = (id >> 8) & 0xFF;
  int c = id & 0xFF;
  // Translate display coords back to logical coords when board is flipped
  if (s_board_flipped) {
    r = 7 - r;
    c = 7 - c;
  }
  // Practice mode: handle locally
  if (s_current_mode == 5) {
    practice_cell_tap(r, c);
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "TOUCH|action=board;row=%d;col=%d\n", r, c);
  if (s_send_fn) s_send_fn(buf);
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------

static void btn_hint_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=hint;x=0;y=0\n");
  lv_label_set_text(s_status_label, "Requesting hint...");
}
static void btn_back_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=undo;x=0;y=0\n");
  lv_label_set_text(s_status_label, "Undo last move");
}
static void btn_new_cb(lv_event_t* e);
static void btn_home_cb(lv_event_t* e);

// ---------------------------------------------------------------------------
// Confirmation dialog
// ---------------------------------------------------------------------------

static void confirm_yes_cb(lv_event_t* e) {
  (void)e;
  ConfirmAction action = s_confirm_action;
  s_confirm_action = CONFIRM_NONE;
  if (s_confirm_overlay) {
    lv_obj_del(s_confirm_overlay);
    s_confirm_overlay = nullptr;
  }
  if (action == CONFIRM_HOME) {
    if (s_current_mode != 5 && s_send_fn)
      s_send_fn("TOUCH|action=home;x=0;y=0\n");
    chess_ui_show_welcome();
  } else if (action == CONFIRM_NEW) {
    if (s_current_mode == 5) {
      show_opening_picker();
    } else {
      if (s_send_fn) s_send_fn("TOUCH|action=new;x=0;y=0\n");
      if (s_status_label) lv_label_set_text(s_status_label, "New game");
      // Reset clock (practice mode clock reset is handled inside practice_new_game)
      s_white_time_sec = s_clock_initial_sec;
      s_black_time_sec = s_clock_initial_sec;
      s_white_active = true;
      s_clock_running = false;
      s_clock_started = false;
      updateClockDisplay();
      // Reset move history
      s_move_count = 0;
      memset(s_move_list, 0, sizeof(s_move_list));
      updateMoveList();
      // Reset board flip and show swap button again (HvH)
      s_board_flipped = false;
      if (s_swap_btn && s_current_mode == 1)
        lv_obj_clear_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void confirm_no_cb(lv_event_t* e) {
  (void)e;
  s_confirm_action = CONFIRM_NONE;
  if (s_confirm_overlay) {
    lv_obj_del(s_confirm_overlay);
    s_confirm_overlay = nullptr;
  }
}

static void showConfirmDialog(const char* message, ConfirmAction action) {
  if (s_confirm_overlay) return; // already showing
  s_confirm_action = action;

  // Semi-transparent overlay covering the whole game screen
  s_confirm_overlay = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_confirm_overlay, s_screen_w, s_screen_h);
  lv_obj_set_pos(s_confirm_overlay, 0, 0);
  lv_obj_set_style_bg_color(s_confirm_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_confirm_overlay, LV_OPA_60, 0);
  lv_obj_set_style_border_width(s_confirm_overlay, 0, 0);
  lv_obj_set_style_radius(s_confirm_overlay, 0, 0);
  lv_obj_set_style_pad_all(s_confirm_overlay, 0, 0);
  lv_obj_clear_flag(s_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

  // Dialog box
  lv_obj_t* box = lv_obj_create(s_confirm_overlay);
  lv_obj_set_size(box, 280, 140);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x2a2a2a), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0x666666), 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_all(box, 10, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(box);
  lv_label_set_text(lbl, message);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 10);

  // Yes button
  lv_obj_t* yes = lv_btn_create(box);
  lv_obj_set_size(yes, 100, 40);
  lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 10, -5);
  lv_obj_set_style_bg_color(yes, lv_color_hex(0x4CAF50), 0);
  lv_obj_set_style_bg_color(yes, lv_color_hex(0x388E3C),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_radius(yes, 4, 0);
  lv_obj_set_style_shadow_width(yes, 0, 0);
  lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* yl = lv_label_create(yes);
  lv_label_set_text(yl, "Yes");
  lv_obj_set_style_text_color(yl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(yl);

  // No button
  lv_obj_t* no = lv_btn_create(box);
  lv_obj_set_size(no, 100, 40);
  lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -10, -5);
  lv_obj_set_style_bg_color(no, lv_color_hex(0xF44336), 0);
  lv_obj_set_style_bg_color(no, lv_color_hex(0xD32F2F),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_radius(no, 4, 0);
  lv_obj_set_style_shadow_width(no, 0, 0);
  lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* nl = lv_label_create(no);
  lv_label_set_text(nl, "No");
  lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(nl);
}

static void btn_new_cb(lv_event_t* e) {
  (void)e;
  showConfirmDialog("Start a new game?", CONFIRM_NEW);
}
static void btn_resign_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=resign;x=0;y=0\n");
  lv_label_set_text(s_status_label, "Resigned");
}

// Clock setup button — now accessed via Settings screen
static void btn_clock_cb(lv_event_t* e) {
  (void)e;
  if (s_settings_screen) lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_clear_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
}

// Play/pause button between the two clock panels
static void clock_play_pause_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (!s_clock_started) {
    // First press — start white's clock
    s_clock_started = true;
    s_clock_running = true;
    s_white_active = true;
  } else {
    // Toggle pause/resume
    s_clock_running = !s_clock_running;
  }
  updateClockDisplay();
}

// Clock panel tap — acts as a chess clock button
// White panel (bottom): white just moved → switch to black
static void clock_white_tap_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (!s_white_active) return; // only respond when it's your turn
  if (!s_clock_started) {
    s_clock_started = true;
    s_clock_running = true;
  } else if (s_clock_increment_sec > 0) {
    s_white_time_sec += s_clock_increment_sec;
  }
  s_white_active = false;
  updateClockDisplay();
}

// Black panel (top): black just moved → switch to white
static void clock_black_tap_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (s_white_active) return; // only respond when it's your turn
  if (!s_clock_started) {
    s_clock_started = true;
    s_clock_running = true;
  } else if (s_clock_increment_sec > 0) {
    s_black_time_sec += s_clock_increment_sec;
  }
  s_white_active = true;
  updateClockDisplay();
}

// Home button — back to welcome / mode selection screen
static void btn_home_cb(lv_event_t* e) {
  (void)e;
  showConfirmDialog("Return to home screen?", CONFIRM_HOME);
}

// Undo last move
static void btn_undo_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=undo;x=0;y=0\n");
}

// Swap sides (HvH only, before game starts)
static void btn_swap_cb(lv_event_t* e) {
  (void)e;
  s_board_flipped = !s_board_flipped;
  // Re-render the board with the flipped orientation
  if (s_last_fen[0])
    chess_ui_render_fen(s_last_fen);
  if (s_send_fn) s_send_fn("TOUCH|action=swap;x=0;y=0\n");
}

static void applyClockPreset(int time_sec, int increment_sec) {
  if (time_sec <= 0 && increment_sec <= 0) {
    s_no_clock = true;
    s_clock_initial_sec = 0;
    s_clock_increment_sec = 0;
  } else {
    s_no_clock = false;
    s_clock_initial_sec = time_sec;
    s_clock_increment_sec = increment_sec;
  }
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockDisplay();
  updateClockOnlyDisplay();
  // Update clock-only time control label
  if (s_co_tc_label) {
    if (s_no_clock) {
      lv_label_set_text(s_co_tc_label, "No Clock");
    } else {
      char tc_buf[16];
      int m = s_clock_initial_sec / 60;
      if (s_clock_increment_sec > 0)
        snprintf(tc_buf, sizeof(tc_buf), "%d+%d", m, s_clock_increment_sec);
      else
        snprintf(tc_buf, sizeof(tc_buf), "%d+0", m);
      lv_label_set_text(s_co_tc_label, tc_buf);
    }
  }
  // Switch back to the originating screen
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_from_clockonly) {
    s_clock_from_clockonly = false;
    if (s_clockonly_screen) lv_obj_clear_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
  } else if (s_clock_from_settings) {
    if (s_settings_screen) lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  }
}

static void clock_preset_cb(lv_event_t* e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < NUM_PRESETS)
    applyClockPreset(CLOCK_PRESETS[idx].time_sec,
                     CLOCK_PRESETS[idx].increment_sec);
}

static void clock_no_clock_cb(lv_event_t* e) {
  (void)e;
  applyClockPreset(0, 0);
}

static void clock_back_cb(lv_event_t* e) {
  (void)e;
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_from_clockonly) {
    s_clock_from_clockonly = false;
    if (s_clockonly_screen) lv_obj_clear_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
  } else if (s_clock_from_settings) {
    if (s_settings_screen) lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  }
}

static void updateCustomLabels() {
  if (!s_custom_min_label || !s_custom_inc_label) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d min", s_custom_minutes);
  lv_label_set_text(s_custom_min_label, buf);
  snprintf(buf, sizeof(buf), "%d sec", s_custom_increment);
  lv_label_set_text(s_custom_inc_label, buf);
}

static void clock_min_plus_cb(lv_event_t* e) {
  (void)e;
  if (s_custom_minutes < 180) s_custom_minutes++;
  updateCustomLabels();
}
static void clock_min_minus_cb(lv_event_t* e) {
  (void)e;
  if (s_custom_minutes > 1) s_custom_minutes--;
  updateCustomLabels();
}
static void clock_inc_plus_cb(lv_event_t* e) {
  (void)e;
  if (s_custom_increment < 60) s_custom_increment++;
  updateCustomLabels();
}
static void clock_inc_minus_cb(lv_event_t* e) {
  (void)e;
  if (s_custom_increment > 0) s_custom_increment--;
  updateCustomLabels();
}
static void clock_custom_apply_cb(lv_event_t* e) {
  (void)e;
  applyClockPreset(s_custom_minutes * 60, s_custom_increment);
}

// ---------------------------------------------------------------------------
// Welcome / Game screen switching
// ---------------------------------------------------------------------------

void chess_ui_show_welcome() {
  if (s_welcome_screen) lv_obj_clear_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_settings_screen) lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clockonly_screen) lv_obj_add_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
}

void chess_ui_show_game(const char* mode_name) {
  if (s_welcome_screen) lv_obj_add_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);

  // Reset move history
  s_move_count = 0;
  memset(s_move_list, 0, sizeof(s_move_list));

  bool is_hvh = (s_current_mode == 1);
  bool is_practice = (s_current_mode == 5);

  // Toggle HvH vs practice vs generic controls
  if (s_white_area) {
    if (is_hvh)
      lv_obj_clear_flag(s_white_area, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_white_area, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_black_area) {
    if (is_hvh)
      lv_obj_clear_flag(s_black_area, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_black_area, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_generic_ctrl_area) {
    if (is_hvh || is_practice)
      lv_obj_add_flag(s_generic_ctrl_area, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(s_generic_ctrl_area, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_practice_ctrl_area) {
    if (is_practice)
      lv_obj_clear_flag(s_practice_ctrl_area, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_practice_ctrl_area, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_swap_btn) {
    if (is_hvh)
      lv_obj_clear_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_blk_ml_box) {
    if (is_hvh && s_show_movelist)
      lv_obj_clear_flag(s_blk_ml_box, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_blk_ml_box, LV_OBJ_FLAG_HIDDEN);
  }

  // Apply settings visibility (clock, captures, movelist)
  applyVisibilitySettings();

  // Status / mode label (non-HvH, non-practice only)
  if (!is_hvh && !is_practice && mode_name && s_status_label)
    lv_label_set_text(s_status_label, mode_name);
  updateMoveList();

  // Show starting position and reset clock
  s_board_flipped = false;
  chess_ui_render_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR");
  chess_ui_reset_highlight();
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockDisplay();

  // Practice mode: show opening picker (which calls practice_new_game on selection)
  if (is_practice) {
    // Show opening picker; user chooses Train or Play from there
    practice_new_game(nullptr); // start with clean board until user picks
    show_opening_picker();
  }

#ifdef SIMULATOR
  if (!is_practice) {
  // Demo data — Italian Game (12 moves) in SAN notation
  static const char* demo_san[] = {
      "e4",
      "e5", // 1
      "Nf3",
      "Nc6", // 2
      "Bc4",
      "Bc5", // 3
      "c3",
      "Nf6", // 4
      "d4",
      "exd4", // 5
      "cxd4",
      "Bb4+", // 6
      "Nc3",
      "Nxe4", // 7
      "O-O",
      "Nxc3", // 8
      "bxc3",
      "Bxc3", // 9
      "Ba3",
      "d5", // 10
      "Bb5",
      "Qd6", // 11
      "Rc1",
      "O-O", // 12
  };
  static const char* demo_fen =
      "r1b2rk1/ppp2ppp/2nq4/1B1p4/3P4/B4N2/P4PPP/2RQ1RK1";
  s_move_count = 0;
  for (int i = 0; i < 24 && i < MAX_MOVE_HISTORY; i++) {
    strncpy(s_move_list[s_move_count], demo_san[i], 7);
    s_move_list[s_move_count][7] = '\0';
    s_move_count++;
  }
  chess_ui_render_fen(demo_fen);
  chess_ui_set_move(7, 0, 7, 2, "Rc1"); // highlight last white move a1→c1
  updateMoveList();
  s_white_time_sec = 8 * 60 + 34;
  s_black_time_sec = 7 * 60 + 12;
  s_white_active = false; // black to move
  s_clock_running = true;
  s_clock_started = true;
  updateClockDisplay();
  }
#endif
}

// Mode button callback — sends mode selection to master and switches to game
static void mode_btn_cb(lv_event_t* e) {
  intptr_t mode = (intptr_t)lv_event_get_user_data(e);
  s_current_mode = (int)mode;
  // Practice mode is local — don't notify master
  if (mode != 5) {
    char buf[48];
    snprintf(buf, sizeof(buf), "TOUCH|action=mode;value=%d\n", (int)mode);
    if (s_send_fn) s_send_fn(buf);
  }
  // Switch to game screen right away
  const char* name = (mode >= 1 && mode <= 5) ? MODE_NAMES[mode] : "Game";
  chess_ui_show_game(name);
}

// Clock-only mode button — opens standalone chess clock from welcome screen
static void clockonly_btn_cb(lv_event_t* e) {
  (void)e;
  if (s_welcome_screen) lv_obj_add_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  // Reset clock state
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockOnlyDisplay();
  if (s_clockonly_screen) lv_obj_clear_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// UI creation
// ---------------------------------------------------------------------------

static lv_obj_t* make_ctrl_btn(lv_obj_t* parent, const char* text,
                               int x, int y, int w, int h,
                               lv_event_cb_t cb) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_radius(btn, 4, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lbl);
  return btn;
}

// Create a button with 180°-rotated label via canvas pixel rotation
static lv_obj_t* make_rotated_ctrl_btn(lv_obj_t* parent, const char* text,
                                       int x, int y, int w, int h,
                                       lv_event_cb_t cb,
                                       lv_color_t* disp_buf) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_radius(btn, 4, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  if (s_blk_btn_scratch && disp_buf) {
    // Render text horizontally into scratch buffer
    lv_obj_t* tmp = lv_canvas_create(btn);
    lv_canvas_set_buffer(tmp, s_blk_btn_scratch,
                         BLK_BTN_CVS_W, BLK_BTN_CVS_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(tmp, lv_color_hex(0x333333), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_montserrat_14;
    dsc.color = lv_color_hex(0xFFFFFF);
    dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_point_t sz;
    lv_txt_get_size(&sz, text, dsc.font, 0, 0, BLK_BTN_CVS_W, LV_TEXT_FLAG_NONE);
    int ty = (BLK_BTN_CVS_H - sz.y) / 2;
    lv_canvas_draw_text(tmp, 0, ty, BLK_BTN_CVS_W, &dsc, text);

    // Rotate 180° into display buffer
    rotateBuf180(s_blk_btn_scratch, BLK_BTN_CVS_W, BLK_BTN_CVS_H, disp_buf);

    // Replace scratch canvas with display canvas
    lv_canvas_set_buffer(tmp, disp_buf,
                         BLK_BTN_CVS_W, BLK_BTN_CVS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(tmp);
  }
  return btn;
}

// ---------------------------------------------------------------------------
// Clock setup screen creation
// ---------------------------------------------------------------------------

// Apply current visibility settings to game screen widgets
static void applyVisibilitySettings() {
  // Clock panels
  if (s_clock_white_panel) {
    if (s_show_clock)
      lv_obj_clear_flag(s_clock_white_panel, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_clock_white_panel, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_clock_black_panel) {
    if (s_show_clock)
      lv_obj_clear_flag(s_clock_black_panel, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_clock_black_panel, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_clock_play_btn) {
    if (s_show_clock)
      lv_obj_clear_flag(s_clock_play_btn, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_clock_play_btn, LV_OBJ_FLAG_HIDDEN);
  }
  // Captured pieces bars
  if (s_white_cap_bar) {
    if (s_show_captures)
      lv_obj_clear_flag(s_white_cap_bar, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_white_cap_bar, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_black_cap_bar) {
    if (s_show_captures)
      lv_obj_clear_flag(s_black_cap_bar, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_black_cap_bar, LV_OBJ_FLAG_HIDDEN);
  }
  // Move list
  if (s_movelist_box) {
    if (s_show_movelist)
      lv_obj_clear_flag(s_movelist_box, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_movelist_box, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_blk_ml_box) {
    bool is_hvh = (s_current_mode == 1);
    if (is_hvh && s_show_movelist)
      lv_obj_clear_flag(s_blk_ml_box, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_blk_ml_box, LV_OBJ_FLAG_HIDDEN);
  }
}

// Settings screen toggle switch callbacks
static lv_obj_t* s_sw_clock = nullptr;
static lv_obj_t* s_sw_captures = nullptr;
static lv_obj_t* s_sw_movelist = nullptr;
static lv_obj_t* s_brightness_slider = nullptr;
static lv_obj_t* s_brightness_value_label = nullptr;

static void brightness_slider_cb(lv_event_t* e) {
  lv_obj_t* slider = lv_event_get_target(e);
  s_brightness = (int)lv_slider_get_value(slider);
  if (s_brightness < 1) s_brightness = 1;
  if (s_brightness_fn) s_brightness_fn(s_brightness);
  if (s_brightness_value_label) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_brightness);
    lv_label_set_text(s_brightness_value_label, buf);
  }
  saveSettings();
}

static void settings_toggle_cb(lv_event_t* e) {
  lv_obj_t* sw = lv_event_get_target(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (sw == s_sw_clock)
    s_show_clock = on;
  else if (sw == s_sw_captures)
    s_show_captures = on;
  else if (sw == s_sw_movelist)
    s_show_movelist = on;
  saveSettings();
  applyVisibilitySettings();
}

static void theme_prev_cb(lv_event_t* e) {
  (void)e;
  s_theme_index = (s_theme_index - 1 + NUM_THEMES) % NUM_THEMES;
  applyTheme();
  if (s_theme_label)
    lv_label_set_text(s_theme_label, THEMES[s_theme_index].name);
  saveSettings();
}

static void theme_next_cb(lv_event_t* e) {
  (void)e;
  s_theme_index = (s_theme_index + 1) % NUM_THEMES;
  applyTheme();
  if (s_theme_label)
    lv_label_set_text(s_theme_label, THEMES[s_theme_index].name);
  saveSettings();
}

static void settings_back_cb(lv_event_t* e) {
  (void)e;
  if (s_settings_screen) lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_settings_from_game) {
    if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (s_welcome_screen) lv_obj_clear_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  }
  s_settings_from_game = false;
}

static void settings_clock_btn_cb(lv_event_t* e) {
  (void)e;
  s_clock_from_settings = true;
  if (s_settings_screen) lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_clear_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
}

static void cogwheel_cb(lv_event_t* e) {
  (void)e;
  // Determine whether we came from welcome or game screen
  bool from_game = s_game_screen && !lv_obj_has_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  s_settings_from_game = from_game;
  if (from_game) {
    if (s_game_screen) lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (s_welcome_screen) lv_obj_add_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_settings_screen) lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
}

static void createSettingsScreen(lv_obj_t* parent, int screen_w, int screen_h) {
  s_settings_screen = lv_obj_create(parent);
  lv_obj_set_size(s_settings_screen, screen_w, screen_h);
  lv_obj_set_pos(s_settings_screen, 0, 0);
  lv_obj_set_style_bg_color(s_settings_screen, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(s_settings_screen, 0, 0);
  lv_obj_set_style_radius(s_settings_screen, 0, 0);
  lv_obj_set_style_pad_all(s_settings_screen, 0, 0);
  lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

  // Title
  lv_obj_t* stitle = lv_label_create(s_settings_screen);
  lv_label_set_text(stitle, LV_SYMBOL_SETTINGS "  Settings");
  lv_obj_set_style_text_color(stitle, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(stitle, &lv_font_montserrat_28, 0);
  lv_obj_align(stitle, LV_ALIGN_TOP_MID, 0, 24);

  // Toggle rows
  struct SettingsRow {
    const char* label;
    bool* value;
    lv_obj_t** sw_out;
  };
  SettingsRow rows[] = {
      {"Show Clock", &s_show_clock, &s_sw_clock},
      {"Show Captured Pieces", &s_show_captures, &s_sw_captures},
      {"Show Move List", &s_show_movelist, &s_sw_movelist},
  };
  int row_y = 90;
  int row_h = 56;
  int pad_x = 24;
  for (int i = 0; i < 3; i++) {
    // Row container
    lv_obj_t* row = lv_obj_create(s_settings_screen);
    lv_obj_set_size(row, screen_w - 2 * pad_x, row_h);
    lv_obj_set_pos(row, pad_x, row_y + i * (row_h + 8));
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 12, 0);

    // Label on left
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, rows[i].label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // Switch on right
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4CAF50), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (*rows[i].value)
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, settings_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    *rows[i].sw_out = sw;
  }

  // Brightness slider row
  {
    int bright_y = row_y + 3 * (row_h + 8);
    lv_obj_t* row = lv_obj_create(s_settings_screen);
    lv_obj_set_size(row, screen_w - 2 * pad_x, row_h);
    lv_obj_set_pos(row, pad_x, bright_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 12, 0);

    // Label
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Brightness");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // Value label on right
    s_brightness_value_label = lv_label_create(row);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_brightness);
    lv_label_set_text(s_brightness_value_label, buf);
    lv_obj_set_style_text_color(s_brightness_value_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_brightness_value_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_brightness_value_label, LV_ALIGN_RIGHT_MID, 0, 0);

    // Slider in the middle
    s_brightness_slider = lv_slider_create(row);
    lv_slider_set_range(s_brightness_slider, 1, 100);
    lv_slider_set_value(s_brightness_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_size(s_brightness_slider, 200, 10);
    lv_obj_align(s_brightness_slider, LV_ALIGN_CENTER, 20, 0);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(0xFFC107), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(s_brightness_slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  }

  // Theme selector row
  {
    int theme_y = row_y + 4 * (row_h + 8);
    lv_obj_t* row = lv_obj_create(s_settings_screen);
    lv_obj_set_size(row, screen_w - 2 * pad_x, row_h);
    lv_obj_set_pos(row, pad_x, theme_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 12, 0);

    // Label on left
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Theme");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // < button
    lv_obj_t* prev = lv_btn_create(row);
    lv_obj_set_size(prev, 36, 36);
    lv_obj_align(prev, LV_ALIGN_RIGHT_MID, -140, 0);
    lv_obj_set_style_radius(prev, 6, 0);
    lv_obj_set_style_bg_color(prev, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_color(prev, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_border_width(prev, 0, 0);
    lv_obj_add_event_cb(prev, theme_prev_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* pl = lv_label_create(prev);
    lv_label_set_text(pl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pl);

    // Theme name label (centered)
    s_theme_label = lv_label_create(row);
    lv_label_set_text(s_theme_label, THEMES[s_theme_index].name);
    lv_obj_set_style_text_color(s_theme_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_theme_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_theme_label, LV_ALIGN_RIGHT_MID, -60, 0);

    // > button
    lv_obj_t* next = lv_btn_create(row);
    lv_obj_set_size(next, 36, 36);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(next, 6, 0);
    lv_obj_set_style_bg_color(next, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_color(next, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_border_width(next, 0, 0);
    lv_obj_add_event_cb(next, theme_next_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* nl = lv_label_create(next);
    lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(nl);
  }

  // Theme preview panel
  {
    const ColorTheme& tp = THEMES[s_theme_index];
    int preview_y = row_y + 5 * (row_h + 8);
    int preview_h = 120;
    lv_obj_t* pnl = lv_obj_create(s_settings_screen);
    lv_obj_set_size(pnl, screen_w - 2 * pad_x, preview_h);
    lv_obj_set_pos(pnl, pad_x, preview_y);
    lv_obj_clear_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(pnl, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(pnl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pnl, 8, 0);
    lv_obj_set_style_border_width(pnl, 0, 0);
    lv_obj_set_style_pad_all(pnl, 0, 0);

    // Mini 4x4 board on the left
    int sq = 24;
    int board_x = 16;
    int board_y = (preview_h - 4 * sq) / 2;
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        lv_obj_t* s = lv_obj_create(pnl);
        lv_obj_set_size(s, sq, sq);
        lv_obj_set_pos(s, board_x + c * sq, board_y + r * sq);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s, 0, 0);
        lv_obj_set_style_border_width(s, 0, 0);
        lv_obj_set_style_pad_all(s, 0, 0);
        bool is_hl = (r == 1 && c == 2) || (r == 2 && c == 1);
        bool dark = (r + c) % 2 != 0;
        uint32_t clr;
        if (is_hl) clr = dark ? tp.hl_dark : tp.hl_light;
        else clr = dark ? tp.dark_sq : tp.light_sq;
        lv_obj_set_style_bg_color(s, lv_color_hex(clr), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        s_preview_sq[r][c] = s;
      }

    // "Board" label under mini board
    lv_obj_t* blbl = lv_label_create(pnl);
    lv_label_set_text(blbl, "Board");
    lv_obj_set_style_text_color(blbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(blbl, board_x + (4 * sq - 36) / 2, board_y + 4 * sq + 4);

    // Clock swatches on the right
    int clk_x = board_x + 4 * sq + 32;
    int clk_w = 130;
    int clk_h = 42;
    int clk_gap = 8;
    int clk_y0 = (preview_h - 2 * clk_h - clk_gap - 18) / 2;

    // Active clock swatch
    s_preview_clk_active = lv_obj_create(pnl);
    lv_obj_set_size(s_preview_clk_active, clk_w, clk_h);
    lv_obj_set_pos(s_preview_clk_active, clk_x, clk_y0);
    lv_obj_clear_flag(s_preview_clk_active, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_preview_clk_active, lv_color_hex(tp.clk_active_bg), 0);
    lv_obj_set_style_bg_opa(s_preview_clk_active, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_preview_clk_active, 6, 0);
    lv_obj_set_style_border_width(s_preview_clk_active, 0, 0);
    s_preview_clk_active_lbl = lv_label_create(s_preview_clk_active);
    lv_label_set_text(s_preview_clk_active_lbl, "10:00");
    lv_obj_set_style_text_color(s_preview_clk_active_lbl, lv_color_hex(tp.clk_active_fg), 0);
    lv_obj_set_style_text_font(s_preview_clk_active_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_preview_clk_active_lbl);

    // Inactive clock swatch
    s_preview_clk_inactive = lv_obj_create(pnl);
    lv_obj_set_size(s_preview_clk_inactive, clk_w, clk_h);
    lv_obj_set_pos(s_preview_clk_inactive, clk_x, clk_y0 + clk_h + clk_gap);
    lv_obj_clear_flag(s_preview_clk_inactive, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_preview_clk_inactive, lv_color_hex(tp.clk_inactive_bg), 0);
    lv_obj_set_style_bg_opa(s_preview_clk_inactive, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_preview_clk_inactive, 6, 0);
    lv_obj_set_style_border_width(s_preview_clk_inactive, 0, 0);
    s_preview_clk_inactive_lbl = lv_label_create(s_preview_clk_inactive);
    lv_label_set_text(s_preview_clk_inactive_lbl, "10:00");
    lv_obj_set_style_text_color(s_preview_clk_inactive_lbl, lv_color_hex(tp.clk_inactive_fg), 0);
    lv_obj_set_style_text_font(s_preview_clk_inactive_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_preview_clk_inactive_lbl);

    // "Clocks" label under swatches
    lv_obj_t* clbl = lv_label_create(pnl);
    lv_label_set_text(clbl, "Clocks");
    lv_obj_set_style_text_color(clbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(clbl, clk_x + (clk_w - 42) / 2, clk_y0 + 2 * clk_h + clk_gap + 4);

    // LED swatch on far right
    int led_x = clk_x + clk_w + 24;
    int led_w = 80;
    int led_h = 2 * clk_h + clk_gap;
    s_preview_led_box = lv_obj_create(pnl);
    lv_obj_set_size(s_preview_led_box, led_w, led_h);
    lv_obj_set_pos(s_preview_led_box, led_x, clk_y0);
    lv_obj_clear_flag(s_preview_led_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_preview_led_box, lv_color_hex(tp.led_bg), 0);
    lv_obj_set_style_bg_opa(s_preview_led_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_preview_led_box, 6, 0);
    lv_obj_set_style_border_width(s_preview_led_box, 0, 0);
    s_preview_led_lbl = lv_label_create(s_preview_led_box);
    lv_label_set_text(s_preview_led_lbl, "8:88");
    lv_obj_set_style_text_color(s_preview_led_lbl, lv_color_hex(tp.led_on), 0);
    lv_obj_set_style_text_font(s_preview_led_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_preview_led_lbl);

    lv_obj_t* ledlbl = lv_label_create(pnl);
    lv_label_set_text(ledlbl, "LED");
    lv_obj_set_style_text_color(ledlbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ledlbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ledlbl, led_x + (led_w - 22) / 2, clk_y0 + led_h + 4);
  }

  // Clock Settings button
  int btn_y = row_y + 5 * (row_h + 8) + 120 + 16;
  lv_obj_t* clk_btn = lv_btn_create(s_settings_screen);
  lv_obj_set_size(clk_btn, screen_w - 2 * pad_x, 50);
  lv_obj_set_pos(clk_btn, pad_x, btn_y);
  lv_obj_set_style_radius(clk_btn, 8, 0);
  lv_obj_set_style_bg_color(clk_btn, lv_color_hex(0x5D4037), 0);
  lv_obj_set_style_bg_color(clk_btn, lv_color_hex(0x795548), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(clk_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(clk_btn, 0, 0);
  lv_obj_set_style_border_width(clk_btn, 0, 0);
  lv_obj_add_event_cb(clk_btn, settings_clock_btn_cb, LV_EVENT_CLICKED, nullptr);
  {
    lv_obj_t* clbl = lv_label_create(clk_btn);
    lv_label_set_text(clbl, LV_SYMBOL_SETTINGS "  Clock Settings");
    lv_obj_set_style_text_color(clbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(clbl);
  }

  // WiFi & Updates — opens the network/OTA screen owned by wifi_ui.cpp.
  // Button is always created so the layout is stable; tap is a no-op when
  // no handler is registered (e.g. SDL simulator build).
  lv_obj_t* wifi_btn = lv_btn_create(s_settings_screen);
  lv_obj_set_size(wifi_btn, screen_w - 2 * pad_x, 50);
  lv_obj_align_to(wifi_btn, clk_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_set_style_radius(wifi_btn, 8, 0);
  lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x37474F), 0);
  lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x546E7A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(wifi_btn, 0, 0);
  lv_obj_set_style_border_width(wifi_btn, 0, 0);
  lv_obj_add_event_cb(wifi_btn, wifi_settings_btn_cb, LV_EVENT_CLICKED, nullptr);
  {
    lv_obj_t* wlbl = lv_label_create(wifi_btn);
    lv_label_set_text(wlbl, LV_SYMBOL_WIFI "  WiFi & Updates");
    lv_obj_set_style_text_color(wlbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wlbl);
  }

  // Back button at bottom
  lv_obj_t* back_btn = lv_btn_create(s_settings_screen);
  lv_obj_set_size(back_btn, 120, 44);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_radius(back_btn, 8, 0);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x444444), 0);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(back_btn, 0, 0);
  lv_obj_set_style_border_width(back_btn, 0, 0);
  lv_obj_add_event_cb(back_btn, settings_back_cb, LV_EVENT_CLICKED, nullptr);
  {
    lv_obj_t* blbl = lv_label_create(back_btn);
    lv_label_set_text(blbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(blbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(blbl);
  }
}

static void createClockScreen(lv_obj_t* parent, int screen_w, int screen_h) {
  s_clock_screen = lv_obj_create(parent);
  lv_obj_set_size(s_clock_screen, screen_w, screen_h);
  lv_obj_set_pos(s_clock_screen, 0, 0);
  lv_obj_set_style_bg_color(s_clock_screen, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(s_clock_screen, 0, 0);
  lv_obj_set_style_radius(s_clock_screen, 0, 0);
  lv_obj_set_style_pad_all(s_clock_screen, 0, 0);
  lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_scrollbar_mode(s_clock_screen, LV_SCROLLBAR_MODE_AUTO);

  int pad = 10;
  int cw = screen_w - 2 * pad;
  int y = 8;

  // ---- Title ----
  lv_obj_t* title = lv_label_create(s_clock_screen);
  lv_label_set_text(title, "Clock Setup");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(title, pad, y);
  y += 38;

  // ---- Preset buttons by category ----
  int pbtn_w = (cw - 2 * 6) / 3; // 3 per row, 6px gap
  int pbtn_h = 38;
  const char* last_cat = "";
  int col = 0;

  for (int i = 0; i < NUM_PRESETS; i++) {
    if (strcmp(last_cat, CLOCK_PRESETS[i].category) != 0) {
      if (col > 0) {
        y += pbtn_h + 4;
        col = 0;
      }
      last_cat = CLOCK_PRESETS[i].category;
      y += 8;
      lv_obj_t* cat = lv_label_create(s_clock_screen);
      lv_label_set_text(cat, last_cat);
      lv_obj_set_style_text_color(cat, lv_color_hex(0x999999), 0);
      lv_obj_set_pos(cat, pad, y);
      y += 20;
      col = 0;
    }

    int bx = pad + col * (pbtn_w + 6);
    lv_obj_t* btn = lv_btn_create(s_clock_screen);
    lv_obj_set_size(btn, pbtn_w, pbtn_h);
    lv_obj_set_pos(btn, bx, y);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(btn, clock_preset_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, CLOCK_PRESETS[i].label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);

    col++;
    if (col >= 3) {
      col = 0;
      y += pbtn_h + 4;
    }
  }
  if (col > 0) y += pbtn_h + 4;

  // ---- No Clock ----
  y += 8;
  lv_obj_t* no_btn = lv_btn_create(s_clock_screen);
  lv_obj_set_size(no_btn, cw, 40);
  lv_obj_set_pos(no_btn, pad, y);
  lv_obj_set_style_radius(no_btn, 6, 0);
  lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x444444), 0);
  lv_obj_set_style_bg_color(no_btn, lv_color_hex(0x666666),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(no_btn, 0, 0);
  lv_obj_set_style_border_width(no_btn, 0, 0);
  lv_obj_add_event_cb(no_btn, clock_no_clock_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* no_lbl = lv_label_create(no_btn);
  lv_label_set_text(no_lbl, "No Clock");
  lv_obj_set_style_text_color(no_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(no_lbl);
  y += 48;

  // ---- Custom section ----
  y += 4;
  lv_obj_t* cust_hdr = lv_label_create(s_clock_screen);
  lv_label_set_text(cust_hdr, "Custom");
  lv_obj_set_style_text_color(cust_hdr, lv_color_hex(0x999999), 0);
  lv_obj_set_pos(cust_hdr, pad, y);
  y += 22;

  int row_h = 36;
  int pm_btn_w = 36;
  int val_w = 70;
  int lbl_w = 68;

  // Time row: "Time:" [−] value [+]
  lv_obj_t* time_lbl = lv_label_create(s_clock_screen);
  lv_label_set_text(time_lbl, "Time:");
  lv_obj_set_style_text_color(time_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_pos(time_lbl, pad, y + 8);

  int cx = pad + lbl_w;
  // [−]
  lv_obj_t* tm_minus = lv_btn_create(s_clock_screen);
  lv_obj_set_size(tm_minus, pm_btn_w, row_h);
  lv_obj_set_pos(tm_minus, cx, y);
  lv_obj_set_style_radius(tm_minus, 4, 0);
  lv_obj_set_style_bg_color(tm_minus, lv_color_hex(0x333333), 0);
  lv_obj_set_style_shadow_width(tm_minus, 0, 0);
  lv_obj_set_style_border_width(tm_minus, 1, 0);
  lv_obj_set_style_border_color(tm_minus, lv_color_hex(0x555555), 0);
  lv_obj_add_event_cb(tm_minus, clock_min_minus_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* tm_ml = lv_label_create(tm_minus);
  lv_label_set_text(tm_ml, "-");
  lv_obj_set_style_text_color(tm_ml, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(tm_ml);
  cx += pm_btn_w + 2;

  // value
  s_custom_min_label = lv_label_create(s_clock_screen);
  char mb[16];
  snprintf(mb, sizeof(mb), "%d min", s_custom_minutes);
  lv_label_set_text(s_custom_min_label, mb);
  lv_obj_set_style_text_color(s_custom_min_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_size(s_custom_min_label, val_w, row_h);
  lv_obj_set_style_text_align(s_custom_min_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_custom_min_label, cx, y + 8);
  cx += val_w;

  // [+]
  lv_obj_t* tm_plus = lv_btn_create(s_clock_screen);
  lv_obj_set_size(tm_plus, pm_btn_w, row_h);
  lv_obj_set_pos(tm_plus, cx, y);
  lv_obj_set_style_radius(tm_plus, 4, 0);
  lv_obj_set_style_bg_color(tm_plus, lv_color_hex(0x333333), 0);
  lv_obj_set_style_shadow_width(tm_plus, 0, 0);
  lv_obj_set_style_border_width(tm_plus, 1, 0);
  lv_obj_set_style_border_color(tm_plus, lv_color_hex(0x555555), 0);
  lv_obj_add_event_cb(tm_plus, clock_min_plus_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* tm_pl = lv_label_create(tm_plus);
  lv_label_set_text(tm_pl, "+");
  lv_obj_set_style_text_color(tm_pl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(tm_pl);
  y += row_h + 6;

  // Increment row: "Incr:" [−] value [+]
  lv_obj_t* inc_lbl = lv_label_create(s_clock_screen);
  lv_label_set_text(inc_lbl, "Incr:");
  lv_obj_set_style_text_color(inc_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_pos(inc_lbl, pad, y + 8);

  cx = pad + lbl_w;
  lv_obj_t* ic_minus = lv_btn_create(s_clock_screen);
  lv_obj_set_size(ic_minus, pm_btn_w, row_h);
  lv_obj_set_pos(ic_minus, cx, y);
  lv_obj_set_style_radius(ic_minus, 4, 0);
  lv_obj_set_style_bg_color(ic_minus, lv_color_hex(0x333333), 0);
  lv_obj_set_style_shadow_width(ic_minus, 0, 0);
  lv_obj_set_style_border_width(ic_minus, 1, 0);
  lv_obj_set_style_border_color(ic_minus, lv_color_hex(0x555555), 0);
  lv_obj_add_event_cb(ic_minus, clock_inc_minus_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ic_ml = lv_label_create(ic_minus);
  lv_label_set_text(ic_ml, "-");
  lv_obj_set_style_text_color(ic_ml, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(ic_ml);
  cx += pm_btn_w + 2;

  s_custom_inc_label = lv_label_create(s_clock_screen);
  char ib[16];
  snprintf(ib, sizeof(ib), "%d sec", s_custom_increment);
  lv_label_set_text(s_custom_inc_label, ib);
  lv_obj_set_style_text_color(s_custom_inc_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_size(s_custom_inc_label, val_w, row_h);
  lv_obj_set_style_text_align(s_custom_inc_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_custom_inc_label, cx, y + 8);
  cx += val_w;

  lv_obj_t* ic_plus = lv_btn_create(s_clock_screen);
  lv_obj_set_size(ic_plus, pm_btn_w, row_h);
  lv_obj_set_pos(ic_plus, cx, y);
  lv_obj_set_style_radius(ic_plus, 4, 0);
  lv_obj_set_style_bg_color(ic_plus, lv_color_hex(0x333333), 0);
  lv_obj_set_style_shadow_width(ic_plus, 0, 0);
  lv_obj_set_style_border_width(ic_plus, 1, 0);
  lv_obj_set_style_border_color(ic_plus, lv_color_hex(0x555555), 0);
  lv_obj_add_event_cb(ic_plus, clock_inc_plus_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ic_pl = lv_label_create(ic_plus);
  lv_label_set_text(ic_pl, "+");
  lv_obj_set_style_text_color(ic_pl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(ic_pl);
  y += row_h + 8;

  // Apply Custom button
  lv_obj_t* apply_btn = lv_btn_create(s_clock_screen);
  lv_obj_set_size(apply_btn, cw, 40);
  lv_obj_set_pos(apply_btn, pad, y);
  lv_obj_set_style_radius(apply_btn, 6, 0);
  lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0x2196F3), 0);
  lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0x1976D2),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(apply_btn, 0, 0);
  lv_obj_set_style_border_width(apply_btn, 0, 0);
  lv_obj_add_event_cb(apply_btn, clock_custom_apply_cb, LV_EVENT_CLICKED,
                      nullptr);
  lv_obj_t* apply_lbl = lv_label_create(apply_btn);
  lv_label_set_text(apply_lbl, "Apply Custom");
  lv_obj_set_style_text_color(apply_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(apply_lbl);
  y += 48;

  // ---- Back button ----
  y += 4;
  lv_obj_t* back_btn = lv_btn_create(s_clock_screen);
  lv_obj_set_size(back_btn, cw, 40);
  lv_obj_set_pos(back_btn, pad, y);
  lv_obj_set_style_radius(back_btn, 6, 0);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), 0);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x777777),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(back_btn, 0, 0);
  lv_obj_set_style_border_width(back_btn, 0, 0);
  lv_obj_add_event_cb(back_btn, clock_back_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, "Back");
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(back_lbl);
}

// ---------------------------------------------------------------------------
// Clock-only screen callbacks
// ---------------------------------------------------------------------------

static void co_white_tap_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (!s_white_active) return;
  if (!s_clock_started) {
    s_clock_started = true;
    s_clock_running = true;
  } else if (s_clock_increment_sec > 0) {
    s_white_time_sec += s_clock_increment_sec;
  }
  s_white_active = false;
  updateClockOnlyDisplay();
}

static void co_black_tap_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (s_white_active) return;
  if (!s_clock_started) {
    s_clock_started = true;
    s_clock_running = true;
  } else if (s_clock_increment_sec > 0) {
    s_black_time_sec += s_clock_increment_sec;
  }
  s_white_active = true;
  updateClockOnlyDisplay();
}

static void co_play_pause_cb(lv_event_t* e) {
  (void)e;
  if (s_no_clock) return;
  if (!s_clock_started) {
    s_clock_started = true;
    s_clock_running = true;
    s_white_active = true;
  } else {
    s_clock_running = !s_clock_running;
  }
  updateClockOnlyDisplay();
}

static void co_home_cb(lv_event_t* e) {
  (void)e;
  s_clock_running = false;
  chess_ui_show_welcome();
}

static void co_clock_setup_cb(lv_event_t* e) {
  (void)e;
  s_clock_from_clockonly = true;
  s_clock_from_settings = false;
  if (s_clockonly_screen) lv_obj_add_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_clear_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
}

static void co_reset_cb(lv_event_t* e) {
  (void)e;
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockOnlyDisplay();
}

// ---------------------------------------------------------------------------
// Clock-only screen creation
// ---------------------------------------------------------------------------

static void createClockOnlyScreen(lv_obj_t* parent, int screen_w, int screen_h) {
  s_clockonly_screen = lv_obj_create(parent);
  lv_obj_set_size(s_clockonly_screen, screen_w, screen_h);
  lv_obj_set_pos(s_clockonly_screen, 0, 0);
  lv_obj_set_style_bg_color(s_clockonly_screen, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_border_width(s_clockonly_screen, 0, 0);
  lv_obj_set_style_radius(s_clockonly_screen, 0, 0);
  lv_obj_set_style_pad_all(s_clockonly_screen, 0, 0);
  lv_obj_add_flag(s_clockonly_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_clockonly_screen, LV_OBJ_FLAG_SCROLLABLE);

  int ctrl_strip_w = 56;
  int clock_area_w = screen_w - ctrl_strip_w;
  int half_h = screen_h / 2;

  // ---- Control strip (left side, vertical) ----
  lv_obj_t* ctrl = lv_obj_create(s_clockonly_screen);
  lv_obj_set_size(ctrl, ctrl_strip_w, screen_h);
  lv_obj_set_pos(ctrl, 0, 0);
  lv_obj_set_style_bg_color(ctrl, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_bg_opa(ctrl, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ctrl, 1, 0);
  lv_obj_set_style_border_color(ctrl, lv_color_hex(0x333333), 0);
  lv_obj_set_style_border_side(ctrl, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_radius(ctrl, 0, 0);
  lv_obj_set_style_pad_all(ctrl, 0, 0);
  lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);

  int btn_sz = 44;
  int gap = 8;
  int n_btns = 4;
  int total_btn_h = n_btns * btn_sz + (n_btns - 1) * gap;
  int btn_x = (ctrl_strip_w - btn_sz) / 2;
  int btn_y = (screen_h - total_btn_h) / 2;

  // Home
  {
    lv_obj_t* b = lv_btn_create(ctrl);
    lv_obj_set_size(b, btn_sz, btn_sz);
    lv_obj_set_pos(b, btn_x, btn_y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, co_home_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(l, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(l);
    btn_y += btn_sz + gap;
  }
  // Clock setup
  {
    lv_obj_t* b = lv_btn_create(ctrl);
    lv_obj_set_size(b, btn_sz, btn_sz);
    lv_obj_set_pos(b, btn_x, btn_y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, co_clock_setup_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(l, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(l);
    btn_y += btn_sz + gap;
  }
  // Play / Pause
  {
    lv_obj_t* b = lv_btn_create(ctrl);
    lv_obj_set_size(b, btn_sz, btn_sz);
    lv_obj_set_pos(b, btn_x, btn_y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4E2A1A), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x6D3B2A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, co_play_pause_cb, LV_EVENT_CLICKED, nullptr);
    s_co_play_lbl = lv_label_create(b);
    lv_label_set_text(s_co_play_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_co_play_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_co_play_lbl);
    btn_y += btn_sz + gap;
  }
  // Reset
  {
    lv_obj_t* b = lv_btn_create(ctrl);
    lv_obj_set_size(b, btn_sz, btn_sz);
    lv_obj_set_pos(b, btn_x, btn_y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, co_reset_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(l, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(l);
  }

  // Time control label (bottom of control strip)
  s_co_tc_label = lv_label_create(ctrl);
  lv_label_set_text(s_co_tc_label, "10+0");
  lv_obj_set_style_text_color(s_co_tc_label, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(s_co_tc_label, &lv_font_montserrat_12, 0);
  lv_obj_align(s_co_tc_label, LV_ALIGN_BOTTOM_MID, 0, -6);

  // ---- Black player panel (top-right, tappable) ----
  s_co_black_panel = lv_obj_create(s_clockonly_screen);
  lv_obj_set_size(s_co_black_panel, clock_area_w, half_h);
  lv_obj_set_pos(s_co_black_panel, ctrl_strip_w, 0);
  lv_obj_set_style_bg_color(s_co_black_panel, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(s_co_black_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_co_black_panel, 0, 0);
  lv_obj_set_style_radius(s_co_black_panel, 0, 0);
  lv_obj_set_style_pad_all(s_co_black_panel, 0, 0);
  lv_obj_clear_flag(s_co_black_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_co_black_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_co_black_panel, co_black_tap_cb, LV_EVENT_CLICKED, nullptr);
  if (s_co_black_buf) {
    s_co_black_canvas = lv_canvas_create(s_co_black_panel);
    lv_canvas_set_buffer(s_co_black_canvas, s_co_black_buf,
                         SEG7_DSP_W, SEG7_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_co_black_canvas);
  }

  // ---- White player panel (bottom-right, tappable) ----
  s_co_white_panel = lv_obj_create(s_clockonly_screen);
  lv_obj_set_size(s_co_white_panel, clock_area_w, half_h);
  lv_obj_set_pos(s_co_white_panel, ctrl_strip_w, half_h);
  lv_obj_set_style_bg_color(s_co_white_panel, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(s_co_white_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_co_white_panel, 0, 0);
  lv_obj_set_style_radius(s_co_white_panel, 0, 0);
  lv_obj_set_style_pad_all(s_co_white_panel, 0, 0);
  lv_obj_clear_flag(s_co_white_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_co_white_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_co_white_panel, co_white_tap_cb, LV_EVENT_CLICKED, nullptr);
  if (s_co_white_buf) {
    s_co_white_canvas = lv_canvas_create(s_co_white_panel);
    lv_canvas_set_buffer(s_co_white_canvas, s_co_white_buf,
                         SEG7_DSP_W, SEG7_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_co_white_canvas);
  }

  // Hidden scratch canvas (source dimensions, before rotation)
  if (s_co_scratch_buf) {
    s_co_scratch_canvas = lv_canvas_create(s_clockonly_screen);
    lv_canvas_set_buffer(s_co_scratch_canvas, s_co_scratch_buf,
                         SEG7_CVS_W, SEG7_CVS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(s_co_scratch_canvas, LV_OBJ_FLAG_HIDDEN);
  }
}

void chess_ui_create(int screen_w, int screen_h,
                     const lv_font_t* piece_font,
                     chess_ui_send_fn_t send_fn,
                     chess_ui_brightness_fn_t brightness_fn) {
  s_send_fn = send_fn;
  s_brightness_fn = brightness_fn;
  s_piece_font = piece_font;
  s_screen_w = screen_w;
  s_screen_h = screen_h;
  loadSettings();

#ifndef SIMULATOR
  // Engine task on core 0 (LVGL runs on core 1).
  // Stack allocated from internal SRAM — PSRAM task stacks require
  // CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY which may not be set.
  s_engine_trigger = xSemaphoreCreateBinary();
  s_engine_done    = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(
      engine_task_fn, "engine",
      ENGINE_TASK_STACK, nullptr,
      1,  // lower priority than LVGL task
      &s_engine_task_handle,
      0);

  // LVGL timer: polls every 50ms for engine search completion.
  // Runs inside the LVGL task so it can safely update widgets.
  s_engine_poll_timer = lv_timer_create(practice_engine_poll_timer_cb, 50, nullptr);
  lv_timer_pause(s_engine_poll_timer);
#endif

  // Apply saved brightness
  if (s_brightness_fn) s_brightness_fn(s_brightness);

  // Apply saved theme colors
  {
    const ColorTheme& t = THEMES[s_theme_index];
    s_light_sq = lv_color_hex(t.light_sq);
    s_dark_sq = lv_color_hex(t.dark_sq);
    s_highlight_light = lv_color_hex(t.hl_light);
    s_highlight_dark = lv_color_hex(t.hl_dark);
  }

  // Allocate canvas buffers on heap (works on both ESP32 and desktop)
  s_clk_src_buf = (lv_color_t*)malloc(CLK_SRC_W * CLK_SRC_H * sizeof(lv_color_t));
  s_clk_w_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));
  s_clk_b_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));

  // Allocate canvas buffers for 180°-rotated black buttons
  s_blk_btn_scratch = (lv_color_t*)malloc(BLK_BTN_CVS_W * BLK_BTN_CVS_H * sizeof(lv_color_t));
  for (int i = 0; i < BLK_BTN_COUNT; i++)
    s_blk_btn_buf[i] = (lv_color_t*)malloc(BLK_BTN_CVS_W * BLK_BTN_CVS_H * sizeof(lv_color_t));

  // Allocate canvas buffers for clock-only 7-segment display
  // Scratch = source (horizontal), display = rotated 90° CW (same pixel count)
  s_co_scratch_buf = (lv_color_t*)malloc(SEG7_CVS_W * SEG7_CVS_H * sizeof(lv_color_t));
  s_co_white_buf = (lv_color_t*)malloc(SEG7_DSP_W * SEG7_DSP_H * sizeof(lv_color_t));
  s_co_black_buf = (lv_color_t*)malloc(SEG7_DSP_W * SEG7_DSP_H * sizeof(lv_color_t));

  // (Black moves canvas buffers removed — using rotated lv_table instead)

  // ---------- Screen ----------
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);

  // ==========================================================================
  // WELCOME SCREEN — mode selection
  // ==========================================================================
  s_welcome_screen = lv_obj_create(scr);
  lv_obj_set_size(s_welcome_screen, screen_w, screen_h);
  lv_obj_set_pos(s_welcome_screen, 0, 0);
  lv_obj_clear_flag(s_welcome_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(s_welcome_screen, 0, 0);
  lv_obj_set_style_border_width(s_welcome_screen, 0, 0);
  lv_obj_set_style_radius(s_welcome_screen, 0, 0);
  lv_obj_set_style_bg_color(s_welcome_screen, lv_color_hex(0x1a1a1a), 0);

  // Title
  lv_obj_t* title = lv_label_create(s_welcome_screen);
  lv_label_set_text(title, "OpenChess");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  // Subtitle
  lv_obj_t* subtitle = lv_label_create(s_welcome_screen);
  lv_label_set_text(subtitle, "Place a piece on a lit square\nor select a mode below");
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x999999), 0);
  lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 62);

  // WiFi status badge — top-left corner, updated by chess_ui_set_wifi_status()
  s_wifi_badge = lv_label_create(s_welcome_screen);
  lv_label_set_text(s_wifi_badge, LV_SYMBOL_WIFI " offline");
  lv_obj_set_style_text_color(s_wifi_badge, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(s_wifi_badge, &lv_font_montserrat_14, 0);
  lv_obj_align(s_wifi_badge, LV_ALIGN_TOP_LEFT, 12, 12);

  // Mode buttons
  struct ModeInfo {
    const char* label;
    uint32_t color;
    int mode_id;
  };
  ModeInfo modes[] = {
      {"Human vs Human", 0x2196F3, 1},     // Blue
      {"Human vs Stockfish", 0x4CAF50, 2}, // Green
      {"Online (Lichess)", 0xFFC107, 3},   // Yellow/Amber
      {"Practice", 0x9C27B0, 5},           // Purple
      {"Sensor Test", 0xF44336, 4},        // Red
  };

  int mbtn_w = screen_w - 40;
  int mbtn_h = 54;
  int mbtn_gap = 10;
  int num_modes = 5;
  int total_h = num_modes * mbtn_h + (num_modes - 1) * mbtn_gap;
  int mbtn_start_y = (screen_h - total_h) / 2 + 20; // shifted down a bit for title

  for (int i = 0; i < num_modes; i++) {
    lv_obj_t* btn = lv_btn_create(s_welcome_screen);
    lv_obj_set_size(btn, mbtn_w, mbtn_h);
    lv_obj_set_pos(btn, 20, mbtn_start_y + i * (mbtn_h + mbtn_gap));
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(modes[i].color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, mode_btn_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)modes[i].mode_id);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, modes[i].label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
  }

  // Clock-only mode button (below mode buttons, distinct style)
  {
    int co_y = mbtn_start_y + num_modes * (mbtn_h + mbtn_gap) + 10;
    lv_obj_t* co_btn = lv_btn_create(s_welcome_screen);
    lv_obj_set_size(co_btn, mbtn_w, mbtn_h);
    lv_obj_set_pos(co_btn, 20, co_y);
    lv_obj_set_style_radius(co_btn, 8, 0);
    lv_obj_set_style_bg_color(co_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_bg_color(co_btn, lv_color_hex(0x455A64), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(co_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(co_btn, 0, 0);
    lv_obj_set_style_border_width(co_btn, 0, 0);
    lv_obj_add_event_cb(co_btn, clockonly_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* co_lbl = lv_label_create(co_btn);
    lv_label_set_text(co_lbl, LV_SYMBOL_CHARGE " Chess Clock");
    lv_obj_set_style_text_color(co_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(co_lbl);
  }

  // Settings cogwheel button (bottom-right of welcome screen)
  {
    lv_obj_t* cog_btn = lv_btn_create(s_welcome_screen);
    lv_obj_set_size(cog_btn, 48, 48);
    lv_obj_align(cog_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_radius(cog_btn, 24, 0);
    lv_obj_set_style_bg_color(cog_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(cog_btn, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cog_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(cog_btn, 0, 0);
    lv_obj_set_style_border_width(cog_btn, 0, 0);
    lv_obj_add_event_cb(cog_btn, cogwheel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cog_lbl = lv_label_create(cog_btn);
    lv_label_set_text(cog_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(cog_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_center(cog_lbl);
  }

  // ==========================================================================
  // GAME SCREEN — board, clocks, controls (starts hidden)
  // ==========================================================================
  s_game_screen = lv_obj_create(scr);
  lv_obj_set_size(s_game_screen, screen_w, screen_h);
  lv_obj_set_pos(s_game_screen, 0, 0);
  lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(s_game_screen, 0, 0);
  lv_obj_set_style_border_width(s_game_screen, 0, 0);
  lv_obj_set_style_radius(s_game_screen, 0, 0);
  lv_obj_set_style_bg_color(s_game_screen, lv_color_hex(0x1a1a1a), 0);
  lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN); // hidden initially

  // ---------- Layout math ----------
  // Board width = 6/8 of screen width (leaves room for clocks on right)
  int cell_size = (screen_w * 6 / 8) / 8;
  int board_side = cell_size * 8;
  int board_x = 4;
  int clock_panel_w = screen_w - board_side - board_x - 8;

  // Piece zoom: scale 30×30 source images to fill cell_size
  s_piece_zoom = (uint16_t)(cell_size * 256 / PIECE_IMG_SIZE);

  int cap_bar_h = 26;  // captured pieces info bar height
  int cap_gap = 2;     // gap between captures bar and board
  int ctrl_btn_h = 36; // button row height
  // Center the board + captures bars + one row of buttons
  int content_h = cap_bar_h + cap_gap + board_side + cap_gap + cap_bar_h + 4 + ctrl_btn_h;
  int board_y = (screen_h - content_h) / 2 + cap_bar_h + cap_gap;

  // ---------- Board container ----------
  lv_obj_t* board = lv_obj_create(s_game_screen);
  lv_obj_set_size(board, board_side, board_side);
  lv_obj_set_pos(board, board_x, board_y);
  lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(board, 0, 0);
  lv_obj_set_style_border_width(board, 0, 0);
  lv_obj_set_style_radius(board, 0, 0);
  lv_obj_set_style_bg_color(board, lv_color_hex(0x000000), 0);

  // ---------- Cells ----------
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      lv_obj_t* b = lv_btn_create(board);
      lv_obj_set_size(b, cell_size, cell_size);
      lv_obj_set_pos(b, c * cell_size, r * cell_size);
      lv_obj_set_style_radius(b, 0, 0);
      lv_obj_set_style_border_width(b, 0, 0);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_set_style_pad_all(b, 0, 0);
      lv_color_t sq = squareColor(r, c);
      lv_obj_set_style_bg_color(b, sq, 0);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(b, sq, LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_set_style_bg_color(b, sq, LV_PART_MAIN | LV_STATE_FOCUSED);
      intptr_t cell_id = (intptr_t)((r << 8) | c);
      lv_obj_add_event_cb(b, cell_event_cb, LV_EVENT_CLICKED, (void*)cell_id);
      s_btns[r][c] = b;

      lv_obj_t* img = lv_img_create(b);
      lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
      lv_obj_center(img);
      s_labels[r][c] = img;
    }
  }

  // ==========================================================================
  // CAPTURED PIECES BARS (chess.com-style, always visible)
  // ==========================================================================
  int cap_y_white = board_y + board_side + cap_gap; // below board
  int cap_y_black = board_y - cap_gap - cap_bar_h;  // above board

  // White's captures bar (shows black pieces that white captured)
  s_white_cap_bar = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_white_cap_bar, board_side, cap_bar_h);
  lv_obj_set_pos(s_white_cap_bar, board_x, cap_y_white);
  lv_obj_clear_flag(s_white_cap_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_white_cap_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_white_cap_bar, 0, 0);
  lv_obj_set_style_pad_all(s_white_cap_bar, 0, 0);
  for (int i = 0; i < MAX_CAPTURES; i++) {
    s_wcap_imgs[i] = lv_img_create(s_white_cap_bar);
    lv_obj_add_flag(s_wcap_imgs[i], LV_OBJ_FLAG_HIDDEN);
  }
  // Black's captures bar (shows white pieces that black captured)
  s_black_cap_bar = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_black_cap_bar, board_side, cap_bar_h);
  lv_obj_set_pos(s_black_cap_bar, board_x, cap_y_black);
  lv_obj_clear_flag(s_black_cap_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_black_cap_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_black_cap_bar, 0, 0);
  lv_obj_set_style_pad_all(s_black_cap_bar, 0, 0);
  for (int i = 0; i < MAX_CAPTURES; i++) {
    s_bcap_imgs[i] = lv_img_create(s_black_cap_bar);
    lv_obj_add_flag(s_bcap_imgs[i], LV_OBJ_FLAG_HIDDEN);
  }
  // ==========================================================================
  // CONTROL AREAS — mode-dependent, created once, toggled via show/hide
  // ==========================================================================
  int ctrl_h = ctrl_btn_h;
  int ctrl_gap = 6;
  int ctrl_area_y = cap_y_white + cap_bar_h + 4; // right below captures bar

  // ------------------------------------------------------------------
  // Generic control area (Stockfish / Lichess / SensorTest)
  // Status label + buttons: Home, Hint, Undo, New, Clock, Resign
  // ------------------------------------------------------------------
  s_generic_ctrl_area = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_generic_ctrl_area, screen_w - 8, 56);
  lv_obj_set_pos(s_generic_ctrl_area, board_x, ctrl_area_y);
  lv_obj_clear_flag(s_generic_ctrl_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_generic_ctrl_area, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_generic_ctrl_area, 0, 0);
  lv_obj_set_style_pad_all(s_generic_ctrl_area, 0, 0);

  s_status_label = lv_label_create(s_generic_ctrl_area);
  lv_label_set_text(s_status_label, "Ready");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(s_status_label, 4, 0);

  {
    int gy = 18;
    int gn = 5;
    int gw = (screen_w - 8 - (gn - 1) * ctrl_gap) / gn;
    make_ctrl_btn(s_generic_ctrl_area, "Home", 0 * (gw + ctrl_gap), gy, gw, ctrl_h, btn_home_cb);
    make_ctrl_btn(s_generic_ctrl_area, "Hint", 1 * (gw + ctrl_gap), gy, gw, ctrl_h, btn_hint_cb);
    make_ctrl_btn(s_generic_ctrl_area, "Undo", 2 * (gw + ctrl_gap), gy, gw, ctrl_h, btn_undo_cb);
    make_ctrl_btn(s_generic_ctrl_area, "New", 3 * (gw + ctrl_gap), gy, gw, ctrl_h, btn_new_cb);
    make_ctrl_btn(s_generic_ctrl_area, "Resign", 4 * (gw + ctrl_gap), gy, gw, ctrl_h, btn_resign_cb);
  }

  // ------------------------------------------------------------------
  // HvH: White area (below board) — buttons + swap (tight to captures bar)
  // ------------------------------------------------------------------
  {
    int wa_h = ctrl_h + 4 + ctrl_h + 4; // buttons + gap + swap + pad
    s_white_area = lv_obj_create(s_game_screen);
    lv_obj_set_size(s_white_area, screen_w - 8, wa_h);
    lv_obj_set_pos(s_white_area, board_x, ctrl_area_y);
    lv_obj_clear_flag(s_white_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_white_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_white_area, 0, 0);
    lv_obj_set_style_pad_all(s_white_area, 0, 0);
    lv_obj_add_flag(s_white_area, LV_OBJ_FLAG_HIDDEN);

    int hvh_n = 3;
    int hvh_w = (screen_w - 8 - (hvh_n - 1) * ctrl_gap) / hvh_n;
    int wy = 0;
    make_ctrl_btn(s_white_area, "Home", 0 * (hvh_w + ctrl_gap), wy, hvh_w, ctrl_h, btn_home_cb);
    make_ctrl_btn(s_white_area, "Undo", 1 * (hvh_w + ctrl_gap), wy, hvh_w, ctrl_h, btn_undo_cb);
    make_ctrl_btn(s_white_area, "New", 2 * (hvh_w + ctrl_gap), wy, hvh_w, ctrl_h, btn_new_cb);

    // Swap button — centered row below the 4 buttons
    int total_btns_w = hvh_n * hvh_w + (hvh_n - 1) * ctrl_gap;
    int swap_w = 120;
    int swap_x = (total_btns_w - swap_w) / 2;
    int swap_y = wy + ctrl_h + 4;
    s_swap_btn = lv_btn_create(s_white_area);
    lv_obj_set_size(s_swap_btn, swap_w, ctrl_h);
    lv_obj_set_pos(s_swap_btn, swap_x, swap_y);
    lv_obj_set_style_radius(s_swap_btn, 4, 0);
    lv_obj_set_style_bg_color(s_swap_btn, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_bg_color(s_swap_btn, lv_color_hex(0x795548),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_swap_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_swap_btn, 0, 0);
    lv_obj_set_style_border_width(s_swap_btn, 1, 0);
    lv_obj_set_style_border_color(s_swap_btn, lv_color_hex(0x795548), 0);
    lv_obj_add_event_cb(s_swap_btn, btn_swap_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_swap_btn, LV_OBJ_FLAG_HIDDEN);
    {
      lv_obj_t* sl = lv_label_create(s_swap_btn);
      lv_label_set_text(sl, LV_SYMBOL_REFRESH " Swap");
      lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_center(sl);
    }
  }

  // ------------------------------------------------------------------
  // HvH: Black area (above board) — buttons (rotated 180°, tight)
  // ------------------------------------------------------------------
  {
    int ba_h = ctrl_h + 4; // buttons + small pad
    s_black_area = lv_obj_create(s_game_screen);
    lv_obj_set_size(s_black_area, screen_w - 8, ba_h);
    lv_obj_set_pos(s_black_area, board_x, cap_y_black - 4 - ba_h);
    lv_obj_clear_flag(s_black_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_black_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_black_area, 0, 0);
    lv_obj_set_style_pad_all(s_black_area, 0, 0);
    lv_obj_add_flag(s_black_area, LV_OBJ_FLAG_HIDDEN);

    // Buttons reversed + canvas-rotated 180° so black player reads them
    int hvh_n = 3;
    int hvh_w = (screen_w - 8 - (hvh_n - 1) * ctrl_gap) / hvh_n;
    int by_b = 2;
    make_rotated_ctrl_btn(s_black_area, "New", 0 * (hvh_w + ctrl_gap), by_b, hvh_w, ctrl_h, btn_new_cb, s_blk_btn_buf[0]);
    make_rotated_ctrl_btn(s_black_area, "Undo", 1 * (hvh_w + ctrl_gap), by_b, hvh_w, ctrl_h, btn_undo_cb, s_blk_btn_buf[1]);
    make_rotated_ctrl_btn(s_black_area, "Home", 2 * (hvh_w + ctrl_gap), by_b, hvh_w, ctrl_h, btn_home_cb, s_blk_btn_buf[2]);

    s_black_moves_label = nullptr; // no per-player moves — combined list used
  }

  // ------------------------------------------------------------------
  // Practice mode control area (below board, shown in mode 5 only)
  // Status label + buttons: Home, Undo, New, Swap + Difficulty
  // ------------------------------------------------------------------
  {
    int pa_h = ctrl_h + 4 + ctrl_h + 4 + 16; // buttons + swap row + difficulty row
    s_practice_ctrl_area = lv_obj_create(s_game_screen);
    lv_obj_set_size(s_practice_ctrl_area, screen_w - 8, pa_h);
    lv_obj_set_pos(s_practice_ctrl_area, board_x, ctrl_area_y);
    lv_obj_clear_flag(s_practice_ctrl_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_practice_ctrl_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_practice_ctrl_area, 0, 0);
    lv_obj_set_style_pad_all(s_practice_ctrl_area, 0, 0);
    lv_obj_add_flag(s_practice_ctrl_area, LV_OBJ_FLAG_HIDDEN);

    // Status label
    s_practice_status_lbl = lv_label_create(s_practice_ctrl_area);
    lv_label_set_text(s_practice_status_lbl, "Your turn");
    lv_obj_set_style_text_color(s_practice_status_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(s_practice_status_lbl, 4, 0);

    // Row 1: Home, Undo, New
    int pr_y1 = 18;
    int pr_n = 3;
    int pr_w = (screen_w - 8 - (pr_n - 1) * ctrl_gap) / pr_n;
    make_ctrl_btn(s_practice_ctrl_area, "Home", 0 * (pr_w + ctrl_gap), pr_y1, pr_w, ctrl_h, btn_home_cb);
    s_practice_undo_btn = make_ctrl_btn(s_practice_ctrl_area, "Undo", 1 * (pr_w + ctrl_gap), pr_y1, pr_w, ctrl_h, practice_undo_cb);
    make_ctrl_btn(s_practice_ctrl_area, "New", 2 * (pr_w + ctrl_gap), pr_y1, pr_w, ctrl_h, practice_new_cb);

    // Row 2: Swap + Difficulty ("-" label "+")  |  (or trainer progress label + hint btn)
    int pr_y2 = pr_y1 + ctrl_h + 4;
    int total_area_w = screen_w - 8;
    int swap_w = 120;
    s_practice_swap_btn = make_ctrl_btn(s_practice_ctrl_area, LV_SYMBOL_REFRESH " Swap",
                  0, pr_y2, swap_w, ctrl_h, practice_swap_cb);

    int diff_area_x = swap_w + ctrl_gap;
    int diff_btn_w = 36;
    s_practice_diff_minus = make_ctrl_btn(s_practice_ctrl_area, LV_SYMBOL_MINUS,
                  diff_area_x, pr_y2, diff_btn_w, ctrl_h, practice_diff_down_cb);

    s_practice_diff_lbl = lv_label_create(s_practice_ctrl_area);
    {
      char dbuf[16];
      snprintf(dbuf, sizeof(dbuf), "Depth %d", s_practice_depth);
      lv_label_set_text(s_practice_diff_lbl, dbuf);
    }
    lv_obj_set_style_text_color(s_practice_diff_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(s_practice_diff_lbl, diff_area_x + diff_btn_w + 8, pr_y2 + 8);

    int plus_x = total_area_w - diff_btn_w;
    s_practice_diff_plus = make_ctrl_btn(s_practice_ctrl_area, LV_SYMBOL_PLUS,
                  plus_x, pr_y2, diff_btn_w, ctrl_h, practice_diff_up_cb);

    // Trainer progress label — spans the full row 2 width, hidden until trainer mode
    s_trainer_progress_lbl = lv_label_create(s_practice_ctrl_area);
    lv_label_set_text(s_trainer_progress_lbl, "Move 1/5  White  [Guided]");
    lv_obj_set_style_text_color(s_trainer_progress_lbl, lv_color_hex(0xAAFFAA), 0);
    lv_obj_set_style_text_font(s_trainer_progress_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_trainer_progress_lbl, 0, pr_y2 + 6);
    lv_obj_add_flag(s_trainer_progress_lbl, LV_OBJ_FLAG_HIDDEN);

    // Hint button (quiz mode only) — right-aligned in row 2
    s_trainer_hint_btn = make_ctrl_btn(s_practice_ctrl_area, LV_SYMBOL_EYE_OPEN " Hint",
                         plus_x - 80 - ctrl_gap, pr_y2, 80, ctrl_h, trainer_hint_btn_cb);
    lv_obj_add_flag(s_trainer_hint_btn, LV_OBJ_FLAG_HIDDEN);
  }

  // ------------------------------------------------------------------
  // HvH: Black move list (above board, LEFT half, rotated 180° via canvas)
  // ------------------------------------------------------------------
  {
    int blk_btn_area_h = ctrl_h + 4;
    int blk_ml_w = screen_w / 2 - 4;
    int blk_ml_x = board_x; // left half (mirroring player 1's right half)
    int blk_ml_h = cap_y_black - 4 - blk_btn_area_h - 4;
    if (blk_ml_h < 40) blk_ml_h = 40;
    int blk_ml_y = cap_y_black - 4 - blk_btn_area_h - 4 - blk_ml_h;
    if (blk_ml_y < 0) {
      blk_ml_y = 0;
      blk_ml_h = cap_y_black - 4 - blk_btn_area_h - 8;
    }
    // Store dimensions for updateMoveList canvas rendering
    s_blk_ml_w = blk_ml_w;
    s_blk_ml_h = blk_ml_h;
    // Allocate canvas buffers
    s_blk_ml_scratch = (lv_color_t*)malloc(blk_ml_w * blk_ml_h * sizeof(lv_color_t));
    s_blk_ml_buf = (lv_color_t*)malloc(blk_ml_w * blk_ml_h * sizeof(lv_color_t));

    s_blk_ml_box = lv_obj_create(s_game_screen);
    lv_obj_set_size(s_blk_ml_box, blk_ml_w, blk_ml_h);
    lv_obj_set_pos(s_blk_ml_box, blk_ml_x, blk_ml_y);
    lv_obj_clear_flag(s_blk_ml_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_blk_ml_box, lv_color_hex(0x262421), 0);
    lv_obj_set_style_bg_opa(s_blk_ml_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_blk_ml_box, 4, 0);
    lv_obj_set_style_border_width(s_blk_ml_box, 1, 0);
    lv_obj_set_style_border_color(s_blk_ml_box, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_pad_all(s_blk_ml_box, 0, 0);
    lv_obj_add_flag(s_blk_ml_box, LV_OBJ_FLAG_HIDDEN); // shown in HvH only

    if (s_blk_ml_scratch && s_blk_ml_buf) {
      s_blk_ml_src_canvas = lv_canvas_create(s_blk_ml_box);
      lv_canvas_set_buffer(s_blk_ml_src_canvas, s_blk_ml_scratch,
                           blk_ml_w, blk_ml_h, LV_IMG_CF_TRUE_COLOR);
      lv_obj_add_flag(s_blk_ml_src_canvas, LV_OBJ_FLAG_HIDDEN);

      s_blk_ml_canvas = lv_canvas_create(s_blk_ml_box);
      lv_canvas_set_buffer(s_blk_ml_canvas, s_blk_ml_buf,
                           blk_ml_w, blk_ml_h, LV_IMG_CF_TRUE_COLOR);
      lv_obj_set_pos(s_blk_ml_canvas, 0, 0);
    }
  }

  // ---------- Chess clock (right of board, rotated 90° CW) ----------
  int clock_x = board_x + board_side + 4;
  int play_btn_h = 28;
  int clock_gap = 4;
  int clock_panel_h = (board_side - play_btn_h - 2 * clock_gap) / 2;

  // Hidden scratch canvas for horizontal text rendering
  if (s_clk_src_buf) {
    s_clk_src_canvas = lv_canvas_create(s_game_screen);
    lv_canvas_set_buffer(s_clk_src_canvas, s_clk_src_buf,
                         CLK_SRC_W, CLK_SRC_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(s_clk_src_canvas, LV_OBJ_FLAG_HIDDEN);
  }

  // Black clock panel (top-right)
  s_clock_black_panel = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_clock_black_panel, clock_panel_w, clock_panel_h);
  lv_obj_set_pos(s_clock_black_panel, clock_x, board_y);
  lv_obj_clear_flag(s_clock_black_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_clock_black_panel, 4, 0);
  lv_obj_set_style_border_width(s_clock_black_panel, 0, 0);
  lv_obj_set_style_pad_all(s_clock_black_panel, 0, 0);
  lv_obj_set_style_bg_opa(s_clock_black_panel, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_clock_black_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_clock_black_panel, clock_black_tap_cb, LV_EVENT_CLICKED, nullptr);

  if (s_clk_b_buf) {
    s_clk_b_canvas = lv_canvas_create(s_clock_black_panel);
    lv_canvas_set_buffer(s_clk_b_canvas, s_clk_b_buf,
                         CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_clk_b_canvas);
  }

  // Play/pause button (between clocks)
  int play_btn_y = board_y + clock_panel_h + clock_gap;
  s_clock_play_btn = lv_btn_create(s_game_screen);
  lv_obj_set_size(s_clock_play_btn, clock_panel_w, play_btn_h);
  lv_obj_set_pos(s_clock_play_btn, clock_x, play_btn_y);
  lv_obj_set_style_radius(s_clock_play_btn, 4, 0);
  lv_obj_set_style_bg_color(s_clock_play_btn, lv_color_hex(0x4E2A1A), 0);
  lv_obj_set_style_bg_color(s_clock_play_btn, lv_color_hex(0x6D3B2A),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(s_clock_play_btn, 0, 0);
  lv_obj_set_style_border_width(s_clock_play_btn, 0, 0);
  lv_obj_add_event_cb(s_clock_play_btn, clock_play_pause_cb, LV_EVENT_CLICKED, nullptr);
  s_clock_play_lbl = lv_label_create(s_clock_play_btn);
  lv_label_set_text(s_clock_play_lbl, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(s_clock_play_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(s_clock_play_lbl);

  // White clock panel (bottom-right)
  s_clock_white_panel = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_clock_white_panel, clock_panel_w, clock_panel_h);
  lv_obj_set_pos(s_clock_white_panel, clock_x, play_btn_y + play_btn_h + clock_gap);
  lv_obj_clear_flag(s_clock_white_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_clock_white_panel, 4, 0);
  lv_obj_set_style_border_width(s_clock_white_panel, 0, 0);
  lv_obj_set_style_pad_all(s_clock_white_panel, 0, 0);
  lv_obj_set_style_bg_opa(s_clock_white_panel, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_clock_white_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_clock_white_panel, clock_white_tap_cb, LV_EVENT_CLICKED, nullptr);

  if (s_clk_w_buf) {
    s_clk_w_canvas = lv_canvas_create(s_clock_white_panel);
    lv_canvas_set_buffer(s_clk_w_canvas, s_clk_w_buf,
                         CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_clk_w_canvas);
  }

  // ---------- Combined move list box (right half, below button row) ----------
  {
    int ml_x = screen_w / 2;                                  // right half starts here
    int ml_w = screen_w / 2 - 4;                              // right half width
    int ml_y = ctrl_area_y + ctrl_btn_h + 4 + ctrl_btn_h + 8; // below buttons + swap row + gap
    int ml_h = screen_h - ml_y - 4;                           // fill remaining space
    if (ml_h < 40) ml_h = 40;
    s_movelist_box = lv_obj_create(s_game_screen);
    lv_obj_set_size(s_movelist_box, ml_w, ml_h);
    lv_obj_set_pos(s_movelist_box, ml_x, ml_y);
    lv_obj_set_style_bg_color(s_movelist_box, lv_color_hex(0x262421), 0);
    lv_obj_set_style_bg_opa(s_movelist_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_movelist_box, 4, 0);
    lv_obj_set_style_border_width(s_movelist_box, 1, 0);
    lv_obj_set_style_border_color(s_movelist_box, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_pad_all(s_movelist_box, 4, 0);
    lv_obj_set_scrollbar_mode(s_movelist_box, LV_SCROLLBAR_MODE_AUTO);

    int tbl_w = ml_w - 12;
    s_movelist_table = lv_table_create(s_movelist_box);
    lv_table_set_col_cnt(s_movelist_table, 3);
    lv_table_set_row_cnt(s_movelist_table, 1);
    lv_table_set_col_width(s_movelist_table, 0, tbl_w * 2 / 10); // move number
    lv_table_set_col_width(s_movelist_table, 1, tbl_w * 4 / 10); // white move
    lv_table_set_col_width(s_movelist_table, 2, tbl_w * 4 / 10); // black move
    lv_obj_set_width(s_movelist_table, tbl_w);
    // Style: dark bg, no borders between cells, light text
    lv_obj_set_style_bg_color(s_movelist_table, lv_color_hex(0x262421), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_movelist_table, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_movelist_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_movelist_table, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_movelist_table, lv_color_hex(0x999999), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_movelist_table, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(s_movelist_table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(s_movelist_table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(s_movelist_table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(s_movelist_table, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_movelist_table, LV_OPA_TRANSP, LV_PART_MAIN);
  }

  updateClockDisplay();
  lv_timer_create(clockTimerCb, 1000, nullptr);

  // ---------- Settings cogwheel on game screen (bottom-left, very dim) ----------
  {
    lv_obj_t* cog = lv_btn_create(s_game_screen);
    lv_obj_set_size(cog, 36, 36);
    lv_obj_set_pos(cog, 4, screen_h - 40);
    lv_obj_set_style_radius(cog, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cog, 0, 0);
    lv_obj_set_style_border_width(cog, 0, 0);
    lv_obj_add_event_cb(cog, cogwheel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(cog);
    lv_label_set_text(lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
  }

  // ==========================================================================
  // CLOCK SETUP SCREEN (starts hidden, shown via Clock button)
  // ==========================================================================
  createClockScreen(scr, screen_w, screen_h);

  // ==========================================================================
  // SETTINGS SCREEN (starts hidden, shown via cogwheel)
  // ==========================================================================
  createSettingsScreen(scr, screen_w, screen_h);

  // ==========================================================================
  // CLOCK-ONLY SCREEN (starts hidden, shown via Chess Clock button)
  // ==========================================================================
  createClockOnlyScreen(scr, screen_w, screen_h);
}

// ===========================================================================
// External integration hooks — small additions, see chess_ui.h
// ===========================================================================

void chess_ui_set_wifi_button_handler(chess_ui_btn_cb_t cb, void* user_data) {
  s_wifi_btn_cb        = cb;
  s_wifi_btn_user_data = user_data;
}

void chess_ui_set_wifi_status(bool online, const char* ssid, const char* ip) {
  if (!s_wifi_badge) return;
  char buf[96];
  if (online && ssid && ssid[0]) {
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", ssid);
  } else if (online) {
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " online");
  } else {
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " offline");
  }
  lv_label_set_text(s_wifi_badge, buf);
  lv_obj_set_style_text_color(s_wifi_badge,
      lv_color_hex(online ? 0x77DD77 : 0x888888), 0);
  (void)ip; // shown on the WiFi screen, not the badge
}
