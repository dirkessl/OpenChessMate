#include <Adafruit_FT6206.h>
#include <Arduino.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
// Generated LVGL font
extern const lv_font_t OpenChessFont_32;
// Serial settings — must match master wiring
#define UI_SERIAL_BAUD 115200
#define UI_SERIAL_RX 25 // connect to master TX (GPIO25)
#define UI_SERIAL_TX 34 // connect to master RX (GPIO34)

TFT_eSPI tft = TFT_eSPI();
Adafruit_FT6206 touch = Adafruit_FT6206();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;

/* Simple line reader for incoming messages */
String inLine = "";

// UI elements
lv_obj_t* lastMoveLabel;
lv_obj_t* cells[8][8];      // button objects per cell
lv_obj_t* cellLabels[8][8]; // label inside each button (for glyph fallback)
lv_style_t style_piece_white, style_piece_black;

// Chess clock
static lv_obj_t* clock_white_panel = nullptr;
static lv_obj_t* clock_black_panel = nullptr;
static int white_time_sec = 10 * 60;
static int black_time_sec = 10 * 60;
static bool white_active = true;
static bool clock_running = true;

// Canvas-based rotated clock
#define CLK_SRC_W 110
#define CLK_SRC_H 36
#define CLK_DSP_W CLK_SRC_H
#define CLK_DSP_H CLK_SRC_W
static lv_color_t* clk_src_buf = nullptr;
static lv_color_t* clk_w_buf = nullptr;
static lv_color_t* clk_b_buf = nullptr;
static lv_obj_t* clk_src_canvas = nullptr;
static lv_obj_t* clk_w_canvas = nullptr;
static lv_obj_t* clk_b_canvas = nullptr;

// Piece bitmap cache
struct PieceBitmap {
  uint16_t* buf = nullptr;
  int w = 0;
  int h = 0;
};
static PieceBitmap pieceCache[128]; // indexed by ascii

// Forward declaration
static const char* pieceToUnicode(char piece);

// Try to load a 24-bit BMP from SPIFFS into RGB565 buffer. Returns true on success.
static bool loadBmpTo565(const char* path, uint16_t*& outBuf, int& outW, int& outH) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  // Read BMP header
  uint8_t header[54];
  if (f.read(header, 54) != 54) {
    f.close();
    return false;
  }
  // Check 'BM'
  if (header[0] != 'B' || header[1] != 'M') {
    f.close();
    return false;
  }
  uint32_t dataOffset = *(uint32_t*)&header[10];
  int32_t width = *(int32_t*)&header[18];
  int32_t height = *(int32_t*)&header[22];
  uint16_t planes = *(uint16_t*)&header[26];
  uint16_t bpp = *(uint16_t*)&header[28];
  if (bpp != 24 || planes != 1) {
    f.close();
    return false;
  }
  if (width <= 0 || height <= 0) {
    f.close();
    return false;
  }
  // BMP rows are padded to 4 bytes
  int rowSize = (width * 3 + 3) & ~3;
  // allocate buffer
  uint16_t* buf = (uint16_t*)malloc(width * height * sizeof(uint16_t));
  if (!buf) {
    f.close();
    return false;
  }
  // Seek to pixel data
  f.seek(dataOffset);
  // BMP stores pixels bottom-up
  for (int y = height - 1; y >= 0; --y) {
    // read a row
    uint8_t row[rowSize];
    if (f.read(row, rowSize) != rowSize) {
      free(buf);
      f.close();
      return false;
    }
    int idx = 0;
    for (int x = 0; x < width; ++x) {
      uint8_t b = row[idx++];
      uint8_t g = row[idx++];
      uint8_t r = row[idx++];
      uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      buf[y * width + x] = rgb565;
    }
  }
  f.close();
  outBuf = buf;
  outW = width;
  outH = height;
  return true;
}

// Draw piece at cell (r,c) by pushing bitmap scaled to cell size if available
static void drawPieceAt(int r, int c, char piece, int cellX, int cellY, int cellW, int cellH) {
  if (piece == ' ') return;
  PieceBitmap& pb = pieceCache[(int)piece];
  if (!pb.buf) {
    // try load from /pieces/<char>.bmp and /pieces/<lower>.bmp
    char path[64];
    snprintf(path, sizeof(path), "/pieces/%c.bmp", piece);
    if (!loadBmpTo565(path, pb.buf, pb.w, pb.h)) {
      // try opposite case
      char p2 = (piece >= 'A' && piece <= 'Z') ? (char)(piece + 32) : (char)(piece - 32);
      snprintf(path, sizeof(path), "/pieces/%c.bmp", p2);
      if (!loadBmpTo565(path, pb.buf, pb.w, pb.h)) {
        pb.buf = nullptr;
        pb.w = pb.h = 0;
      }
    }
  }
  if (pb.buf) {
    // scale naive: if sizes match, push directly; else center and stretch proportionally
    if (pb.w == cellW && pb.h == cellH) {
      tft.pushImage(cellX, cellY, cellW, cellH, pb.buf);
    } else {
      // scale to cell size by simple nearest-neighbor
      uint16_t* tmp = (uint16_t*)malloc(cellW * cellH * sizeof(uint16_t));
      if (!tmp) return;
      for (int yy = 0; yy < cellH; ++yy) {
        int sy = (yy * pb.h) / cellH;
        for (int xx = 0; xx < cellW; ++xx) {
          int sx = (xx * pb.w) / cellW;
          tmp[yy * cellW + xx] = pb.buf[sy * pb.w + sx];
        }
      }
      tft.pushImage(cellX, cellY, cellW, cellH, tmp);
      free(tmp);
    }
  } else {
    // fallback: set label text
    if (r >= 0 && r < 8 && c >= 0 && c < 8) {
      // fallback: show unicode chess glyph instead of letter
      const char* glyph = pieceToUnicode(piece);
      if (glyph && glyph[0]) {
        lv_label_set_text(cellLabels[r][c], glyph);
        if (piece >= 'A' && piece <= 'Z')
          lv_obj_add_style(cellLabels[r][c], &style_piece_white, 0);
        else
          lv_obj_add_style(cellLabels[r][c], &style_piece_black, 0);
      } else {
        char s[2] = {piece, '\0'};
        lv_label_set_text(cellLabels[r][c], s);
      }
    }
  }
}
lv_style_t style_white, style_black, style_highlight;

int screen_w = 320;
int screen_h = 480;
int g_board_x = 4;
int g_board_y = 10;
int g_cell_w = 0;
int g_cell_h = 0;

// Forward
void serialPoll();
void handleLine(const String& line);
void cell_event_cb(lv_event_t* e);
void set_cell_style(int r, int c, lv_style_t* s);
void clear_highlight_timer_cb(lv_timer_t* t);

static void formatTime(int total_sec, char* buf, int buf_size) {
  int mins = total_sec / 60;
  int secs = total_sec % 60;
  snprintf(buf, buf_size, "%d:%02d", mins, secs);
}

// Rotate pixel buffer 90 deg clockwise: src(sw x sh) -> dst(sh x sw)
static void rotateBuf90CW(const lv_color_t* src, int sw, int sh, lv_color_t* dst) {
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      dst[x * sh + (sh - 1 - y)] = src[y * sw + x];
    }
  }
}

static void updateClockDisplay() {
  if (!clk_src_canvas || !clk_w_canvas || !clk_b_canvas) return;
  lv_color_t w_bg, w_fg, b_bg, b_fg;
  if (white_active) {
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
  lv_obj_set_style_bg_color(clock_white_panel, w_bg, 0);
  lv_obj_set_style_bg_color(clock_black_panel, b_bg, 0);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &lv_font_montserrat_28;
  char tbuf[16];

  // White clock
  lv_canvas_fill_bg(clk_src_canvas, w_bg, LV_OPA_COVER);
  formatTime(white_time_sec, tbuf, sizeof(tbuf));
  dsc.color = w_fg;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  lv_point_t sz;
  lv_txt_get_size(&sz, tbuf, dsc.font, 0, 0, CLK_SRC_W, LV_TEXT_FLAG_NONE);
  int ty = (CLK_SRC_H - sz.y) / 2;
  lv_canvas_draw_text(clk_src_canvas, 0, ty, CLK_SRC_W, &dsc, tbuf);
  rotateBuf90CW(clk_src_buf, CLK_SRC_W, CLK_SRC_H, clk_w_buf);
  lv_obj_invalidate(clk_w_canvas);

  // Black clock
  lv_canvas_fill_bg(clk_src_canvas, b_bg, LV_OPA_COVER);
  formatTime(black_time_sec, tbuf, sizeof(tbuf));
  dsc.color = b_fg;
  lv_txt_get_size(&sz, tbuf, dsc.font, 0, 0, CLK_SRC_W, LV_TEXT_FLAG_NONE);
  ty = (CLK_SRC_H - sz.y) / 2;
  lv_canvas_draw_text(clk_src_canvas, 0, ty, CLK_SRC_W, &dsc, tbuf);
  rotateBuf90CW(clk_src_buf, CLK_SRC_W, CLK_SRC_H, clk_b_buf);
  lv_obj_invalidate(clk_b_canvas);
}

static void clockTimerCb(lv_timer_t* t) {
  if (!clock_running) return;
  if (white_active) {
    if (white_time_sec > 0) white_time_sec--;
  } else {
    if (black_time_sec > 0) black_time_sec--;
  }
  updateClockDisplay();
}

// Parse UCI like e2e4 or e7e8q
static bool parseUciMove(const String& uci, int& fr, int& fc, int& tr, int& tc) {
  if (uci.length() < 4) return false;
  char ffile = uci.charAt(0);
  char frank = uci.charAt(1);
  char tfile = uci.charAt(2);
  char trank = uci.charAt(3);
  if (ffile < 'a' || ffile > 'h') return false;
  if (tfile < 'a' || tfile > 'h') return false;
  if (frank < '1' || frank > '8') return false;
  if (trank < '1' || trank > '8') return false;
  fc = ffile - 'a';
  tc = tfile - 'a';
  // rows: row 0 = rank 8
  fr = 8 - (frank - '0');
  tr = 8 - (trank - '0');
  return true;
}

// Map FEN piece char to a UTF-8 chess glyph string (♔..♟)
static const char* pieceToUnicode(char piece) {
  switch (piece) {
    case 'K':
      return u8"♔"; // white king U+2654
    case 'Q':
      return u8"♕"; // white queen U+2655
    case 'R':
      return u8"♖"; // white rook U+2656
    case 'B':
      return u8"♗"; // white bishop U+2657
    case 'N':
      return u8"♘"; // white knight U+2658
    case 'P':
      return u8"♙"; // white pawn U+2659
    case 'k':
      return u8"♚"; // black king U+265A
    case 'q':
      return u8"♛"; // black queen U+265B
    case 'r':
      return u8"♜"; // black rook U+265C
    case 'b':
      return u8"♝"; // black bishop U+265D
    case 'n':
      return u8"♞"; // black knight U+265E
    case 'p':
      return u8"♟"; // black pawn U+265F
    default:
      return "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("UI Slave starting...");

  // Serial2 for master comms
  Serial2.begin(UI_SERIAL_BAUD, SERIAL_8N1, UI_SERIAL_RX, UI_SERIAL_TX);

  // Init SPIFFS for piece bitmaps
  if (!SPIFFS.begin(true)) {
    Serial.println("Warning: SPIFFS mount failed - piece images unavailable");
  }

  // Init display
  tft.init();
  tft.setRotation(1); // portrait
  screen_w = tft.width();
  screen_h = tft.height();

  // Init LVGL
  lv_init();
  buf1 = (lv_color_t*)malloc(sizeof(lv_color_t) * screen_w * 16);
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, screen_w * 16);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screen_w;
  disp_drv.ver_res = screen_h;
  // Optimized flush: push the whole area as one image to the TFT
  disp_drv.flush_cb = [](lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    // LVGL uses native lv_color_t (assumed 16-bit RGB565 here). Cast to uint16_t* and push.
    tft.pushImage(x1, y1, w, h, (uint16_t*)color_p);
    lv_disp_flush_ready(disp);
  };
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Init touch
  if (!touch.begin(40)) {
    Serial.println("Warning: FT6236 not found");
  }
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = [](lv_indev_drv_t* drv, lv_indev_data_t* data) {
    if (!touch.touched()) {
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    TS_Point p = touch.getPoint();
    data->point.x = p.x;
    data->point.y = p.y;
    data->state = LV_INDEV_STATE_PR;
  };
  lv_indev_drv_register(&indev_drv);

  // Styles
  lv_style_init(&style_white);
  lv_style_set_bg_color(&style_white, lv_color_hex(0xF0D9B5));
  lv_style_init(&style_black);
  lv_style_set_bg_color(&style_black, lv_color_hex(0xB58863));
  lv_style_init(&style_highlight);
  lv_style_set_bg_color(&style_highlight, lv_color_hex(0xAAD751));

  // Allocate canvas buffers on heap
  clk_src_buf = (lv_color_t*)malloc(CLK_SRC_W * CLK_SRC_H * sizeof(lv_color_t));
  clk_w_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));
  clk_b_buf = (lv_color_t*)malloc(CLK_DSP_W * CLK_DSP_H * sizeof(lv_color_t));
  if (!clk_src_buf || !clk_w_buf || !clk_b_buf) {
    Serial.println("Warning: clock canvas alloc failed");
  }

  // Build UI: smaller board, centered vertically, clocks on right
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);

  // Board: 6/8 of screen width (leaves room for clocks on right)
  int cell_size = (screen_w * 6 / 8) / 8;
  int board_side = cell_size * 8;
  int board_x = 4;
  int clock_panel_w = screen_w - board_side - board_x - 8;
  // Vertical centering: board + status + buttons
  int btn_h_total = 32;
  int status_h = 16;
  int content_h = board_side + 8 + status_h + 8 + btn_h_total;
  int board_y = (screen_h - content_h) / 2;
  g_board_x = board_x;
  g_board_y = board_y;
  g_cell_w = cell_size;
  g_cell_h = cell_size;

  // Board container
  lv_obj_t* board_cont = lv_obj_create(scr);
  lv_obj_set_size(board_cont, board_side, board_side);
  lv_obj_set_pos(board_cont, board_x, board_y);
  lv_obj_clear_flag(board_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(board_cont, 0, 0);
  lv_obj_set_style_border_width(board_cont, 0, 0);
  lv_obj_set_style_radius(board_cont, 0, 0);
  lv_obj_set_style_bg_color(board_cont, lv_color_hex(0x000000), 0);

  // Chess cells
  for (int r = 0; r < 8; ++r) {
    for (int c = 0; c < 8; ++c) {
      lv_obj_t* btn = lv_btn_create(board_cont);
      lv_obj_set_size(btn, cell_size, cell_size);
      lv_obj_set_pos(btn, c * cell_size, r * cell_size);
      lv_obj_set_style_radius(btn, 0, 0);
      lv_obj_set_style_border_width(btn, 0, 0);
      lv_obj_set_style_shadow_width(btn, 0, 0);
      lv_obj_set_style_pad_all(btn, 0, 0);
      lv_color_t sq_color = ((r + c) % 2 == 0) ? lv_color_hex(0xF0D9B5) : lv_color_hex(0xB58863);
      lv_obj_set_style_bg_color(btn, sq_color, 0);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(btn, sq_color, LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_set_style_bg_color(btn, sq_color, LV_PART_MAIN | LV_STATE_FOCUSED);
      lv_obj_add_event_cb(btn, cell_event_cb, LV_EVENT_CLICKED, (void*)((r << 8) | c));
      cells[r][c] = btn;
      // Label for fallback piece glyph
      lv_obj_t* pl = lv_label_create(btn);
      lv_label_set_text(pl, "");
      lv_obj_set_style_text_font(pl, &OpenChessFont_32, 0);
      lv_obj_center(pl);
      cellLabels[r][c] = pl;
    }
  }

  // Status label (below board)
  lastMoveLabel = lv_label_create(scr);
  lv_label_set_text(lastMoveLabel, "Last move: -");
  lv_obj_set_style_text_color(lastMoveLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(lastMoveLabel, board_x + 4, board_y + board_side + 6);

  // Control buttons (below status, under the board)
  int btn_y = board_y + board_side + 24;
  int btn_w = (board_side - 3 * 4) / 4; // 4 buttons with 4px gap
  int btn_h = 30;
  int btn_gap = 4;

  auto make_btn = [&](const char* label_text, int idx, lv_event_cb_t cb) {
    lv_obj_t* b = lv_btn_create(scr);
    lv_obj_set_size(b, btn_w, btn_h);
    lv_obj_set_pos(b, board_x + idx * (btn_w + btn_gap), btn_y);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x666666), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, label_text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return b;
  };

  make_btn("Hint", 0, [](lv_event_t* e) {
    Serial2.print("TOUCH|action=hint;x=0;y=0\n");
    lv_label_set_text(lastMoveLabel, "Requesting hint...");
  });
  make_btn("Back", 1, [](lv_event_t* e) {
    Serial2.print("TOUCH|action=undo;x=0;y=0\n");
    lv_label_set_text(lastMoveLabel, "Undo last move");
  });
  make_btn("New", 2, [](lv_event_t* e) {
    Serial2.print("TOUCH|action=new;x=0;y=0\n");
    lv_label_set_text(lastMoveLabel, "New game");
  });
  make_btn("Resign", 3, [](lv_event_t* e) {
    Serial2.print("TOUCH|action=resign;x=0;y=0\n");
    lv_label_set_text(lastMoveLabel, "Resigned");
  });

  // Chess clock: right of board, two panels stacked vertically
  // Text drawn horizontally on hidden canvas, then rotated 90 deg CW
  int clock_x = board_x + board_side + 4;
  int clock_panel_h = (board_side - 4) / 2;

  // Hidden source canvas
  if (clk_src_buf) {
    clk_src_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(clk_src_canvas, clk_src_buf, CLK_SRC_W, CLK_SRC_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(clk_src_canvas, LV_OBJ_FLAG_HIDDEN);
  }

  // Black clock panel (top-right)
  clock_black_panel = lv_obj_create(scr);
  lv_obj_set_size(clock_black_panel, clock_panel_w, clock_panel_h);
  lv_obj_set_pos(clock_black_panel, clock_x, board_y);
  lv_obj_clear_flag(clock_black_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(clock_black_panel, 4, 0);
  lv_obj_set_style_border_width(clock_black_panel, 0, 0);
  lv_obj_set_style_pad_all(clock_black_panel, 0, 0);
  lv_obj_set_style_bg_opa(clock_black_panel, LV_OPA_COVER, 0);

  if (clk_b_buf) {
    clk_b_canvas = lv_canvas_create(clock_black_panel);
    lv_canvas_set_buffer(clk_b_canvas, clk_b_buf, CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(clk_b_canvas);
  }

  // White clock panel (bottom-right)
  clock_white_panel = lv_obj_create(scr);
  lv_obj_set_size(clock_white_panel, clock_panel_w, clock_panel_h);
  lv_obj_set_pos(clock_white_panel, clock_x, board_y + clock_panel_h + 4);
  lv_obj_clear_flag(clock_white_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(clock_white_panel, 4, 0);
  lv_obj_set_style_border_width(clock_white_panel, 0, 0);
  lv_obj_set_style_pad_all(clock_white_panel, 0, 0);
  lv_obj_set_style_bg_opa(clock_white_panel, LV_OPA_COVER, 0);

  if (clk_w_buf) {
    clk_w_canvas = lv_canvas_create(clock_white_panel);
    lv_canvas_set_buffer(clk_w_canvas, clk_w_buf, CLK_DSP_W, CLK_DSP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(clk_w_canvas);
  }

  // Initial clock display + timer
  updateClockDisplay();
  lv_timer_create(clockTimerCb, 1000, nullptr);
}

void loop() {
  static unsigned long last = millis();
  unsigned long now = millis();
  lv_tick_inc(now - last);
  last = now;
  lv_timer_handler();

  serialPoll();
  delay(10);
}

void serialPoll() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (inLine.length()) {
        handleLine(inLine);
        inLine = "";
      }
    } else {
      inLine += c;
      if (inLine.length() > 1024) inLine = inLine.substring(inLine.length() - 1024);
    }
  }
}

void set_cell_style(int r, int c, lv_style_t* s) {
  if (r < 0 || r > 7 || c < 0 || c > 7) return;
  lv_color_t col;
  lv_style_get_prop(s, LV_STYLE_BG_COLOR, (lv_style_value_t*)&col);
  lv_obj_set_style_bg_color(cells[r][c], col, 0);
  lv_obj_set_style_bg_color(cells[r][c], col, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(cells[r][c], col, LV_PART_MAIN | LV_STATE_FOCUSED);
}

void clear_highlight_timer_cb(lv_timer_t* t) {
  int* coords = (int*)t->user_data;
  if (coords) {
    int fr = coords[0];
    int fc = coords[1];
    int tr = coords[2];
    int tc = coords[3];
    // restore original colors
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        if ((r + c) % 2 == 0)
          set_cell_style(r, c, &style_white);
        else
          set_cell_style(r, c, &style_black);
      }
    free(coords);
  }
  lv_timer_del(t);
}

void handleLine(const String& line) {
  Serial.printf("UI RX: %s\n", line.c_str());
  int sep = line.indexOf('|');
  String type = (sep >= 0) ? line.substring(0, sep) : line;
  String payload = (sep >= 0) ? line.substring(sep + 1) : String();
  type.trim();
  payload.trim();
  if (type == "HINT") {
    int idx = payload.indexOf("move=");
    String move = (idx >= 0) ? payload.substring(idx + 5) : String();
    move.trim();
    if (move.length()) {
      lv_label_set_text(lastMoveLabel, ("Last move: " + move).c_str());
      int fr, fc, tr, tc;
      if (parseUciMove(move, fr, fc, tr, tc)) {
        // highlight origin and dest
        set_cell_style(fr, fc, &style_highlight);
        set_cell_style(tr, tc, &style_highlight);
        // schedule clear after 3s
        int* coords = (int*)malloc(sizeof(int) * 4);
        coords[0] = fr;
        coords[1] = fc;
        coords[2] = tr;
        coords[3] = tc;
        lv_timer_t* t = lv_timer_create(clear_highlight_timer_cb, 3000, coords);
      }
    }
  } else if (type == "STATE") {
    // payload may contain fen=<fenstring>
    int idx = payload.indexOf("fen=");
    if (idx >= 0) {
      String fen = payload.substring(idx + 4);
      // fen may be terminated by ';' or end of string
      int end = fen.indexOf(';');
      if (end >= 0) fen = fen.substring(0, end);
      fen.trim();
      if (fen.length()) {
        lv_label_set_text(lastMoveLabel, ("FEN: " + fen).c_str());
        // update board: clear labels and draw pieces
        int len = fen.length();
        for (int i = 0; i < 8; ++i)
          for (int j = 0; j < 8; ++j) lv_label_set_text(cellLabels[i][j], "");
        int r = 0;
        int offset = 0;
        while (r < 8 && offset < len) {
          int slash = fen.indexOf('/', offset);
          String rank = (slash >= 0) ? fen.substring(offset, slash) : fen.substring(offset);
          rank.trim();
          int c = 0;
          for (int k = 0; k < rank.length() && c < 8; ++k) {
            char ch = rank.charAt(k);
            if (ch >= '1' && ch <= '8') {
              int empties = ch - '0';
              for (int e = 0; e < empties && c < 8; ++e) {
                c++;
              }
            } else {
              // draw piece image or fallback label
              int x = g_board_x + c * g_cell_w;
              int y = g_board_y + r * g_cell_h;
              drawPieceAt(r, c, ch, x, y, g_cell_w, g_cell_h);
              c++;
            }
          }
          r++;
          if (slash >= 0)
            offset = slash + 1;
          else
            offset = len;
        }
      }
    } else {
      lv_label_set_text(lastMoveLabel, "Board updated");
    }
  } else if (type == "ERROR") {
    lv_label_set_text(lastMoveLabel, "Hint error");
  }
}

void cell_event_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
  int r = (id >> 8) & 0xFF;
  int c = id & 0xFF;
  char buf[64];
  snprintf(buf, sizeof(buf), "TOUCH|action=board;row=%d;col=%d\n", r, c);
  Serial2.print(buf);
}
