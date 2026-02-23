/*
 * Desktop LVGL simulator - Platform layer
 *
 * Uses SDL2 for rendering and a TCP server for protocol messages.
 * All chess UI logic is in the shared chess_ui module.
 */
#include "chess_ui.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <lvgl.h>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Use generated C-format font from ui_slave project
extern "C" {
extern const lv_font_t OpenChessFont_32;
}

// TCP port for receiving protocol messages
static const int TCP_PORT = 8765;

static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture* g_texture = nullptr;

static std::queue<std::string> g_incoming;
static std::mutex g_in_mutex;
static std::atomic<bool> g_running{true};

// Mouse state for LVGL input device
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static bool g_mouse_pressed = false;

// ---------------------------------------------------------------------------
// TCP server thread
// ---------------------------------------------------------------------------
static void tcp_server_thread() {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return;
  }
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(TCP_PORT);
  if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(srv);
    return;
  }
  if (listen(srv, 1) < 0) {
    perror("listen");
    close(srv);
    return;
  }
  std::cout << "TCP server listening on port " << TCP_PORT << "\n";
  while (g_running) {
    int cl = accept(srv, nullptr, nullptr);
    if (cl < 0) {
      if (g_running) perror("accept");
      break;
    }
    std::cout << "Client connected\n";
    char buf[1024];
    std::string line;
    while (g_running) {
      ssize_t r = recv(cl, buf, sizeof(buf), 0);
      if (r <= 0) break;
      for (ssize_t i = 0; i < r; ++i) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
          std::lock_guard<std::mutex> lk(g_in_mutex);
          g_incoming.push(line);
          line.clear();
        } else {
          line.push_back(c);
        }
      }
    }
    close(cl);
    std::cout << "Client disconnected\n";
  }
  close(srv);
}

// ---------------------------------------------------------------------------
// Platform callback: send message to stdout
// ---------------------------------------------------------------------------
static void platformSend(const char* msg) {
  std::cout << "TX: " << msg;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::cerr << "SDL_Init fail: " << SDL_GetError() << "\n";
    return 1;
  }

  int screen_w = 480, screen_h = 800;
  g_window = SDL_CreateWindow("OpenChess UI (LVGL)",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              screen_w, screen_h, SDL_WINDOW_SHOWN);
  if (!g_window) {
    std::cerr << "SDL_CreateWindow: " << SDL_GetError() << "\n";
    return 1;
  }
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  if (!g_renderer)
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
  if (!g_renderer) {
    std::cerr << "SDL_CreateRenderer: " << SDL_GetError() << "\n";
    return 1;
  }
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
                                SDL_TEXTUREACCESS_STREAMING,
                                screen_w, screen_h);
  if (!g_texture) {
    std::cerr << "SDL_CreateTexture: " << SDL_GetError() << "\n";
    return 1;
  }
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_RenderPresent(g_renderer);
  SDL_RaiseWindow(g_window);
  std::cout << "SDL window created (" << screen_w << "x" << screen_h << ")\n";

  // ---- LVGL init ----
  lv_init();

  static lv_color_t* buf =
      (lv_color_t*)malloc(screen_w * screen_h * sizeof(lv_color_t));
  if (!buf) {
    std::cerr << "Display buffer alloc failed\n";
    return 1;
  }

  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screen_w * screen_h);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screen_w;
  disp_drv.ver_res = screen_h;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.direct_mode = 1;
  disp_drv.flush_cb = [](lv_disp_drv_t* drv, const lv_area_t* area,
                         lv_color_t* color_p) {
    int x = area->x1, y = area->y1;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    int sw = drv->hor_res;
    lv_color_t* area_start = color_p + (y * sw + x);
    SDL_Rect rect = {x, y, w, h};
    SDL_UpdateTexture(g_texture, &rect, area_start,
                      sw * (int)sizeof(lv_color_t));
    lv_disp_flush_ready(drv);
  };
  lv_disp_drv_register(&disp_drv);
  std::cout << "LVGL display registered (" << screen_w << "x" << screen_h
            << ")\n";

  // ---- Mouse input device ----
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = [](lv_indev_drv_t* drv, lv_indev_data_t* data) {
    (void)drv;
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_pressed ? LV_INDEV_STATE_PRESSED
                                  : LV_INDEV_STATE_RELEASED;
  };
  lv_indev_drv_register(&indev_drv);
  std::cout << "Mouse input device registered\n";

  // ---- Create shared chess UI ----
  chess_ui_create(screen_w, screen_h, &lv_font_montserrat_14, platformSend);

  // ---- TCP server ----
  std::thread tcp_thread(tcp_server_thread);

  // ---- Main loop ----
  auto last = std::chrono::steady_clock::now();
  while (g_running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) g_running = false;
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        g_mouse_pressed = true;
        g_mouse_x = e.button.x;
        g_mouse_y = e.button.y;
      }
      if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        g_mouse_pressed = false;
        g_mouse_x = e.button.x;
        g_mouse_y = e.button.y;
      }
      if (e.type == SDL_MOUSEMOTION) {
        g_mouse_x = e.motion.x;
        g_mouse_y = e.motion.y;
      }
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last)
                       .count();
    if (elapsed > 5) {
      lv_tick_inc((uint32_t)elapsed);
      last = now;
    }
    lv_timer_handler();

    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);

    // Process incoming TCP messages
    while (true) {
      std::string line;
      {
        std::lock_guard<std::mutex> lk(g_in_mutex);
        if (g_incoming.empty()) break;
        line = g_incoming.front();
        g_incoming.pop();
      }
      chess_ui_handle_message(line.c_str());
    }
    SDL_Delay(10);
  }

  tcp_thread.join();
  free(buf);
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  return 0;
}
