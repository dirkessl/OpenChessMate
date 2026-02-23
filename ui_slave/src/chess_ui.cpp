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
static lv_obj_t* s_clock_play_btn = nullptr;
static lv_obj_t* s_clock_play_lbl = nullptr;
static int s_white_time_sec = 10 * 60;
static int s_black_time_sec = 10 * 60;
static bool s_white_active = true;
static bool s_clock_running = false;

// Canvas-based rotated clock
#define CLK_SRC_W 108
#define CLK_SRC_H 42
#define CLK_DSP_W CLK_SRC_H // 42
#define CLK_DSP_H CLK_SRC_W // 108
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
    "Sensor Test"         // 4
};

// Current game mode (0=select, 1=HvH, 2=Stockfish, 3=Lichess, 4=SensorTest)
static int s_current_mode = 0;

// Clock configuration
static int s_clock_initial_sec = 10 * 60; // default 10 minutes per side
static int s_clock_increment_sec = 0;     // Fischer increment per move
static bool s_clock_started = false;      // set true on first move
static bool s_no_clock = false;           // unlimited / no clock mode

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
static bool s_clock_from_settings = false; // track where clock screen was opened from
static bool s_settings_from_game = false;  // track if settings opened from game screen

// Settings persistence via simple config file
static const char* SETTINGS_FILE = "chess_settings.dat";

static void saveSettings() {
  FILE* f = fopen(SETTINGS_FILE, "wb");
  if (!f) return;
  uint8_t data[3] = {
      (uint8_t)(s_show_clock ? 1 : 0),
      (uint8_t)(s_show_captures ? 1 : 0),
      (uint8_t)(s_show_movelist ? 1 : 0),
  };
  fwrite(data, 1, sizeof(data), f);
  fclose(f);
}

static void loadSettings() {
  FILE* f = fopen(SETTINGS_FILE, "rb");
  if (!f) return;
  uint8_t data[3] = {1, 1, 1};
  if (fread(data, 1, sizeof(data), f) == sizeof(data)) {
    s_show_clock = data[0] != 0;
    s_show_captures = data[1] != 0;
    s_show_movelist = data[2] != 0;
  }
  fclose(f);
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

static void updateClockDisplay() {
  if (!s_clk_src_canvas || !s_clk_w_canvas || !s_clk_b_canvas) return;

  lv_color_t w_bg, w_fg, b_bg, b_fg;
  if (s_white_active) {
    w_bg = lv_color_hex(0x6D3B2A); // reddish brown active
    w_fg = lv_color_hex(0xFFFFFF);
    b_bg = lv_color_hex(0x3E2117); // reddish brown dim
    b_fg = lv_color_hex(0x999999);
  } else {
    w_bg = lv_color_hex(0x3E2117);
    w_fg = lv_color_hex(0x999999);
    b_bg = lv_color_hex(0x6D3B2A);
    b_fg = lv_color_hex(0xFFFFFF);
  }
  lv_obj_set_style_bg_color(s_clock_white_panel, w_bg, 0);
  lv_obj_set_style_bg_color(s_clock_black_panel, b_bg, 0);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &lv_font_montserrat_32;
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
        lv_img_set_zoom(s_labels[r][c], s_piece_zoom);
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
    if (s_send_fn) s_send_fn("TOUCH|action=home;x=0;y=0\n");
    chess_ui_show_welcome();
  } else if (action == CONFIRM_NEW) {
    if (s_send_fn) s_send_fn("TOUCH|action=new;x=0;y=0\n");
    if (s_status_label) lv_label_set_text(s_status_label, "New game");
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

// Swap sides (HvH only, at game start)
static void btn_swap_cb(lv_event_t* e) {
  (void)e;
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
  // Switch back to the originating screen
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_from_settings) {
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
  if (s_clock_from_settings) {
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
}

void chess_ui_show_game(const char* mode_name) {
  if (s_welcome_screen) lv_obj_add_flag(s_welcome_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_clock_screen) lv_obj_add_flag(s_clock_screen, LV_OBJ_FLAG_HIDDEN);
  if (s_game_screen) lv_obj_clear_flag(s_game_screen, LV_OBJ_FLAG_HIDDEN);

  // Reset move history
  s_move_count = 0;
  memset(s_move_list, 0, sizeof(s_move_list));

  bool is_hvh = (s_current_mode == 1);

  // Toggle HvH vs generic controls
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
    if (is_hvh)
      lv_obj_add_flag(s_generic_ctrl_area, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(s_generic_ctrl_area, LV_OBJ_FLAG_HIDDEN);
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

  // Status / mode label (non-HvH only)
  if (!is_hvh && mode_name && s_status_label)
    lv_label_set_text(s_status_label, mode_name);
  updateMoveList();

  // Show starting position and reset clock
  chess_ui_render_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR");
  chess_ui_reset_highlight();
  s_white_time_sec = s_clock_initial_sec;
  s_black_time_sec = s_clock_initial_sec;
  s_white_active = true;
  s_clock_running = false;
  s_clock_started = false;
  updateClockDisplay();

#ifdef SIMULATOR
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
#endif
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

  // Clock Settings button
  int btn_y = row_y + 3 * (row_h + 8) + 16;
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

void chess_ui_create(int screen_w, int screen_h,
                     const lv_font_t* piece_font,
                     chess_ui_send_fn_t send_fn) {
  s_send_fn = send_fn;
  s_piece_font = piece_font;
  s_screen_w = screen_w;
  s_screen_h = screen_h;
  loadSettings();

  // Square colors
  s_light_sq = lv_color_hex(0xF0D9B5);
  s_dark_sq = lv_color_hex(0xB58863);
  s_highlight_light = lv_color_hex(0xAAD751);
  s_highlight_dark = lv_color_hex(0x6B8E23);

  // Allocate canvas buffers on heap (works on both ESP32 and desktop)
  s_clk_src_buf = (lv_color_t*)malloc(CLK_SRC_W * CLK_SRC_H * sizeof(lv_color_t));
  s_clk_w_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));
  s_clk_b_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));

  // Allocate canvas buffers for 180°-rotated black buttons
  s_blk_btn_scratch = (lv_color_t*)malloc(BLK_BTN_CVS_W * BLK_BTN_CVS_H * sizeof(lv_color_t));
  for (int i = 0; i < BLK_BTN_COUNT; i++)
    s_blk_btn_buf[i] = (lv_color_t*)malloc(BLK_BTN_CVS_W * BLK_BTN_CVS_H * sizeof(lv_color_t));

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
}
