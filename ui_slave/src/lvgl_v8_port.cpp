/**
 * @file lvgl_v8_port.cpp
 * @brief LVGL v8 port implementation for VIEWE 4.3" ESP32-S3 display (portrait).
 *
 * Provides:
 *  - Display flush callback (partial buffer, sw_rotate for portrait 480x800)
 *  - Touch read callback (GT911 via ESP32_Display_Panel)
 *  - LVGL tick via esp_timer (1 ms)
 *  - FreeRTOS handler task + mutex for thread safety
 *
 * Based on VIEWE's official LVGL v8 example:
 *   https://github.com/VIEWESMART/UEDX80480043E-WB-A-4.3inch-Touch-Display
 */

#include "lvgl_v8_port.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/* ---------- Globals ---------- */
static ESP_PanelLcd* _lcd = nullptr;
static ESP_PanelTouch* _touch = nullptr;
static SemaphoreHandle_t _lvgl_mutex = nullptr;
static TaskHandle_t _lvgl_task = nullptr;

/* ---------- Tick timer ---------- */
static void lvgl_tick_cb(void* arg) {
  (void)arg;
  lv_tick_inc(1);
}

/* ---------- Display flush ---------- */

#if LVGL_PORT_AVOID_TEARING_MODE != 0

/*
 * Avoid-tearing mode: the draw buffer IS the framebuffer.
 * We just tell LVGL we're done; the LCD driver handles the rest.
 */
static void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  (void)area;
  (void)color_p;

  /* For direct-draw modes the LCD is already showing the buffer,
     or we need to swap framebuffers. */
#if LVGL_PORT_AVOID_TEARING_MODE == 1
  /* Full-size + direct draw — swap buffers via the LCD driver */
  _lcd->drawBitmap(area->x1, area->y1,
                   area->x2 - area->x1 + 1,
                   area->y2 - area->y1 + 1,
                   (const uint8_t*)color_p);
#elif LVGL_PORT_AVOID_TEARING_MODE == 2
  /* Full-size + copy — always draw */
  _lcd->drawBitmap(area->x1, area->y1,
                   area->x2 - area->x1 + 1,
                   area->y2 - area->y1 + 1,
                   (const uint8_t*)color_p);
#endif
  lv_disp_flush_ready(drv);
}

#else /* LVGL_PORT_AVOID_TEARING_MODE == 0 */

/* Standard partial-buffer flush */
static void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  const int x_start = area->x1;
  const int y_start = area->y1;
  const int width = area->x2 - area->x1 + 1;
  const int height = area->y2 - area->y1 + 1;

  _lcd->drawBitmap(x_start, y_start, width, height, (const uint8_t*)color_p);
  lv_disp_flush_ready(drv);
}

#endif /* LVGL_PORT_AVOID_TEARING_MODE */

/* ---------- Rounder callback ---------- */
#if LVGL_PORT_ROUNDER_CB
static void rounder_callback(lv_disp_drv_t* drv, lv_area_t* area) {
  (void)drv;
  /* Align x to 2-pixel boundary (required by some RGB LCD drivers) */
  area->x1 = area->x1 & ~1;
  area->x2 = area->x2 | 1;
}
#endif

/* ---------- Touch read ---------- */
static void touchpad_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  ESP_PanelTouchPoint point;
  int count = _touch ? _touch->readPoints(&point, 1) : 0;

  if (count > 0) {
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* ---------- LVGL handler task ---------- */
static void lvgl_task_fn(void* arg) {
  (void)arg;
  uint32_t task_delay_ms = 5;
  while (true) {
    if (lvgl_port_lock(0)) {
      task_delay_ms = lv_timer_handler();
      lvgl_port_unlock();
    }
    if (task_delay_ms < 1) task_delay_ms = 1;
    if (task_delay_ms > 50) task_delay_ms = 50;
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

/* ---------- Public API ---------- */

void lvgl_port_init(ESP_PanelLcd* lcd, ESP_PanelTouch* touch) {
  _lcd = lcd;
  _touch = touch;

  /* --- LVGL init --- */
  lv_init();

  /* --- Tick timer (1 ms) --- */
  const esp_timer_create_args_t tick_args = {
      .callback = lvgl_tick_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lvgl_tick",
      .skip_unhandled_events = false,
  };
  esp_timer_handle_t tick_timer;
  esp_timer_create(&tick_args, &tick_timer);
  esp_timer_start_periodic(tick_timer, 1000); // 1 ms

  /* --- Display driver --- */
  static lv_disp_draw_buf_t draw_buf;
  static lv_disp_drv_t disp_drv;

  const uint32_t hor_res = 800;
  const uint32_t ver_res = 480;
  const uint32_t buf_h = LVGL_PORT_BUFFER_SIZE_HEIGHT;

#if LVGL_PORT_AVOID_TEARING_MODE == 0
  /* Partial buffer mode — allocate two draw buffers in PSRAM */
  const size_t buf_size = hor_res * buf_h;
  lv_color_t* buf1 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(buf1 && buf2);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);

#elif LVGL_PORT_AVOID_TEARING_MODE == 1
  /* Full-frame direct-draw mode — use the LCD's framebuffers directly */
  void *fb1 = nullptr, *fb2 = nullptr;
  _lcd->getFrameBuffers(1, &fb1, &fb2);
  assert(fb1);
  lv_disp_draw_buf_init(&draw_buf,
                        (lv_color_t*)fb1,
                        (lv_color_t*)fb2,
                        hor_res * ver_res);

#elif LVGL_PORT_AVOID_TEARING_MODE == 2
  /* Full-frame copy mode — allocate full buffer + use LCD framebuffer */
  const size_t buf_size = hor_res * ver_res;
  void* fb1 = nullptr;
  _lcd->getFrameBuffers(1, &fb1);
  lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(fb1 && buf2);
  lv_disp_draw_buf_init(&draw_buf, (lv_color_t*)fb1, buf2, buf_size);
#endif

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = hor_res;
  disp_drv.ver_res = ver_res;
  disp_drv.flush_cb = flush_callback;
  disp_drv.draw_buf = &draw_buf;
#if LVGL_PORT_AVOID_TEARING_MODE == 1
  disp_drv.direct_mode = true;
  disp_drv.full_refresh = false;
#elif LVGL_PORT_AVOID_TEARING_MODE == 2
  disp_drv.direct_mode = false;
  disp_drv.full_refresh = true;
#endif
  /* Portrait mode: rotate display output 90° CW.
   * Native 800×480 landscape → presented as 480×800 portrait.
   * Requires AVOID_TEARING_MODE == 0 (sw_rotate can't use direct_mode). */
  disp_drv.sw_rotate = 1;
  disp_drv.rotated = LV_DISP_ROT_90;
#if LVGL_PORT_ROUNDER_CB
  disp_drv.rounder_cb = rounder_callback;
#endif
  lv_disp_drv_register(&disp_drv);

  /* --- Touch input driver --- */
  if (_touch) {
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);
  }

  /* --- Mutex --- */
  _lvgl_mutex = xSemaphoreCreateRecursiveMutex();
  assert(_lvgl_mutex);

  /* --- Handler task --- */
  xTaskCreatePinnedToCore(
      lvgl_task_fn,
      "lvgl",
      LVGL_PORT_TASK_STACK_SIZE,
      nullptr,
      LVGL_PORT_TASK_PRIORITY,
      &_lvgl_task,
      LVGL_PORT_TASK_CORE);
}

bool lvgl_port_lock(uint32_t timeout_ms) {
  if (!_lvgl_mutex) return false;
  const TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                             : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(_lvgl_mutex, ticks) == pdTRUE;
}

void lvgl_port_unlock(void) {
  if (_lvgl_mutex) {
    xSemaphoreGiveRecursive(_lvgl_mutex);
  }
}
