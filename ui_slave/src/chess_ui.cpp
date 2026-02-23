/*
 * chess_ui.cpp — Shared chess UI implementation
 *
 * Pure LVGL code, no platform dependencies.
 * Works on both ESP32 (Arduino) and desktop (SDL simulator).
 */
#include "chess_ui.h"
#include "pieces/pieces.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static chess_ui_send_fn_t s_send_fn = nullptr;
static const lv_font_t* s_piece_font = nullptr;

// LVGL widget pointers
static lv_obj_t* s_welcome_screen = nullptr; // Welcome / mode selection panel
static lv_obj_t* s_game_screen = nullptr;    // Game board + clock + buttons panel
static lv_obj_t* s_status_label = nullptr;
static lv_obj_t* s_btns[8][8];   // button per cell (holds bg color)
static lv_obj_t* s_labels[8][8]; // label inside each button (piece glyph)

// Chess clock
static lv_obj_t* s_clock_white_panel = nullptr;
static lv_obj_t* s_clock_black_panel = nullptr;
static int s_white_time_sec = 10 * 60;
static int s_black_time_sec = 10 * 60;
static bool s_white_active = true;
static bool s_clock_running = false;

// Canvas-based rotated clock
#define CLK_SRC_W 110
#define CLK_SRC_H 36
#define CLK_DSP_W CLK_SRC_H // 36
#define CLK_DSP_H CLK_SRC_W // 110
static lv_color_t* s_clk_src_buf = nullptr;
static lv_color_t* s_clk_w_buf = nullptr;
static lv_color_t* s_clk_b_buf = nullptr;
static lv_obj_t* s_clk_src_canvas = nullptr;
static lv_obj_t* s_clk_w_canvas = nullptr;
static lv_obj_t* s_clk_b_canvas = nullptr;

// Highlight tracking
static lv_color_t s_highlight_light;
static lv_color_t s_highlight_dark;
static lv_color_t s_light_sq;
static lv_color_t s_dark_sq;
static int s_hl_from_r = -1, s_hl_from_c = -1;
static int s_hl_to_r = -1, s_hl_to_c = -1;

// Screen dimensions (cached for welcome screen layout)
static int s_screen_w = 320;
static int s_screen_h = 480;

// Mode names for display
static const char* MODE_NAMES[] = {
    "Select Mode",        // 0 = selection
    "Human vs Human",     // 1
    "Human vs Stockfish", // 2
    "Online (Lichess)",   // 3
    "Sensor Test"         // 4
};

// Current game mode (0=select, 1=HvH, 2=Stockfish, 3=Lichess, 4=SensorTest)
static int s_current_mode = 0;

// Clock configuration
static int s_clock_initial_sec = 10 * 60; // default 10 minutes per side
static int s_clock_increment_sec = 0;     // Fischer increment per move
static bool s_clock_started = false;      // set true on first move
static bool s_no_clock = false;           // unlimited / no clock mode

// Clock setup screen
static lv_obj_t* s_clock_screen = nullptr;
static lv_obj_t* s_custom_min_label = nullptr;
static lv_obj_t* s_custom_inc_label = nullptr;
static int s_custom_minutes = 10;
static int s_custom_increment = 0;

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

static void updateClockDisplay() {
  if (!s_clk_src_canvas || !s_clk_w_canvas || !s_clk_b_canvas) return;

  lv_color_t w_bg, w_fg, b_bg, b_fg;
  if (s_white_active) {
    w_bg = lv_color_hex(0xF0F0F0);
    w_fg = lv_color_hex(0x000000);
    b_bg = lv_color_hex(0x2a2a2a);
    b_fg = lv_color_hex(0x888888);
  } else {
    w_bg = lv_color_hex(0x2a2a2a);
    w_fg = lv_color_hex(0x888888);
    b_bg = lv_color_hex(0x333333);
    b_fg = lv_color_hex(0xFFFFFF);
  }
  lv_obj_set_style_bg_color(s_clock_white_panel, w_bg, 0);
  lv_obj_set_style_bg_color(s_clock_black_panel, b_bg, 0);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &lv_font_montserrat_28;
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
  int r = 0, c = 0;
  for (const char* p = fen; *p && r < 8; p++) {
    char ch = *p;
    if (ch == '/') {
      r++;
      c = 0;
      continue;
    }
    if (ch == ' ') break; // end of board part
    if (ch >= '1' && ch <= '8') {
      int empties = ch - '0';
      for (int e = 0; e < empties && c < 8; e++) {
        lv_obj_add_flag(s_labels[r][c], LV_OBJ_FLAG_HIDDEN);
        c++;
      }
    } else {
      const lv_img_dsc_t* img = fen_char_to_img(ch);
      if (img) {
        lv_img_set_src(s_labels[r][c], img);
        // In HvH mode, rotate black pieces 180° so they face the black player
        bool is_black_piece = (ch >= 'a' && ch <= 'z');
        int angle = (s_current_mode == 1 && is_black_piece) ? 1800 : 0;
        lv_img_set_angle(s_labels[r][c], angle);
        lv_obj_clear_flag(s_labels[r][c], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_labels[r][c], LV_OBJ_FLAG_HIDDEN);
      }
      c++;
    }
  }
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
      } else if (mode >= 1 && mode <= 4) {
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
// Cell click callback
// ---------------------------------------------------------------------------

static void cell_event_cb(lv_event_t* e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  int r = (id >> 8) & 0xFF;
  int c = id & 0xFF;
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
static void btn_new_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=new;x=0;y=0\n");
  lv_label_set_text(s_status_label, "New game");
}
static void btn_resign_cb(lv_event_t* e) {
  (void)e;
  if (s_send_fn) s_send_fn("TOUCH|action=resign;x=0;y=0\n");
  lv_label_set_text(s_status_label, "Resigned");
}

// Clock setup button — opens the clock configuration screen
static void btn_clock_cb(lv_event_t* e) {
  (void)e;
  if (s_game_screen) lv_obj_add_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_clear_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
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
  // Switch back to game screen
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
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
  if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
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
}

void chess_ui_show_game(const char* mode_name) {
  if (s_welcome_screen) lv_obj_add_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);
  if (mode_name && s_status_label) lv_label_set_text(s_status_label, mode_name);
  // Show starting position and reset clock
  chess_ui_render_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR");
  chess_ui_reset_highlight();
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockDisplay();
}

// Mode button callback — sends mode selection to master and switches to game
static void mode_btn_cb(lv_event_t* e) {
  intptr_t mode = (intptr_t)lv_event_get_user_data(e);
  s_current_mode = (int)mode;
  char buf[48];
  snprintf(buf, sizeof(buf), "TOUCH|action=mode;value=%d\n", (int)mode);
  if (s_send_fn) s_send_fn(buf);
  // Switch to game screen right away
  const char* name = (mode >= 1 && mode <= 4) ? MODE_NAMES[mode] : "Game";
  chess_ui_show_game(name);
}

// ---------------------------------------------------------------------------
// UI creation
// ---------------------------------------------------------------------------

static void make_ctrl_btn(lv_obj_t* parent, const char* text,
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
}

// ---------------------------------------------------------------------------
// Clock setup screen creation
// ---------------------------------------------------------------------------

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

void chess_ui_create(int screen_w, int screen_h,
                     const lv_font_t* piece_font,
                     chess_ui_send_fn_t send_fn) {
  s_send_fn = send_fn;
  s_piece_font = piece_font;
  s_screen_w = screen_w;
  s_screen_h = screen_h;

  // Square colors
  s_light_sq = lv_color_hex(0xF0D9B5);
  s_dark_sq = lv_color_hex(0xB58863);
  s_highlight_light = lv_color_hex(0xAAD751);
  s_highlight_dark = lv_color_hex(0x6B8E23);

  // Allocate canvas buffers on heap (works on both ESP32 and desktop)
  s_clk_src_buf = (lv_color_t*)malloc(CLK_SRC_W * CLK_SRC_H * sizeof(lv_color_t));
  s_clk_w_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));
  s_clk_b_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));

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
      {"Sensor Test", 0xF44336, 4},        // Red
  };

  int mbtn_w = screen_w - 40;
  int mbtn_h = 54;
  int mbtn_gap = 10;
  int total_h = 4 * mbtn_h + 3 * mbtn_gap;
  int mbtn_start_y = (screen_h - total_h) / 2 + 20; // shifted down a bit for title

  for (int i = 0; i < 4; i++) {
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

  int btn_h_total = 32;
  int status_h = 16;
  int content_h = board_side + 8 + status_h + 8 + btn_h_total;
  int board_y = (screen_h - content_h) / 2;

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

  // ---------- Status label ----------
  s_status_label = lv_label_create(s_game_screen);
  lv_label_set_text(s_status_label, "Ready");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(s_status_label, board_x + 4, board_y + board_side + 6);

  // ---------- Control buttons ----------
  int btn_y = board_y + board_side + 24;
  int btn_w = (board_side - 4 * 4) / 5; // 5 buttons, 4px gap
  int btn_h = 30;
  int btn_gap = 4;

  make_ctrl_btn(s_game_screen, "Hint", board_x + 0 * (btn_w + btn_gap), btn_y, btn_w, btn_h, btn_hint_cb);
  make_ctrl_btn(s_game_screen, "Back", board_x + 1 * (btn_w + btn_gap), btn_y, btn_w, btn_h, btn_back_cb);
  make_ctrl_btn(s_game_screen, "New", board_x + 2 * (btn_w + btn_gap), btn_y, btn_w, btn_h, btn_new_cb);
  make_ctrl_btn(s_game_screen, "Clock", board_x + 3 * (btn_w + btn_gap), btn_y, btn_w, btn_h, btn_clock_cb);
  make_ctrl_btn(s_game_screen, "Resign", board_x + 4 * (btn_w + btn_gap), btn_y, btn_w, btn_h, btn_resign_cb);

  // ---------- Chess clock (right of board, rotated 90° CW) ----------
  int clock_x = board_x + board_side + 4;
  int clock_panel_h = (board_side - 4) / 2;

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

  if (s_clk_b_buf) {
    s_clk_b_canvas = lv_canvas_create(s_clock_black_panel);
    lv_canvas_set_buffer(s_clk_b_canvas, s_clk_b_buf,
                         CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_clk_b_canvas);
  }

  // White clock panel (bottom-right)
  s_clock_white_panel = lv_obj_create(s_game_screen);
  lv_obj_set_size(s_clock_white_panel, clock_panel_w, clock_panel_h);
  lv_obj_set_pos(s_clock_white_panel, clock_x, board_y + clock_panel_h + 4);
  lv_obj_clear_flag(s_clock_white_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_clock_white_panel, 4, 0);
  lv_obj_set_style_border_width(s_clock_white_panel, 0, 0);
  lv_obj_set_style_pad_all(s_clock_white_panel, 0, 0);
  lv_obj_set_style_bg_opa(s_clock_white_panel, LV_OPA_COVER, 0);

  if (s_clk_w_buf) {
    s_clk_w_canvas = lv_canvas_create(s_clock_white_panel);
    lv_canvas_set_buffer(s_clk_w_canvas, s_clk_w_buf,
                         CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_clk_w_canvas);
  }

  updateClockDisplay();
  lv_timer_create(clockTimerCb, 1000, nullptr);

  // ==========================================================================
  // CLOCK SETUP SCREEN (starts hidden, shown via Clock button)
  // ==========================================================================
  createClockScreen(scr, screen_w, screen_h);
}
