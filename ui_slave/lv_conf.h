/* ==========================================================================
 * LVGL v8 configuration for VIEWE 4.3" 800x480 ESP32-S3 display
 * Board: UEDX80480043E-WB-A  (RGB bus, GT911 touch, 8MB PSRAM)
 * ========================================================================== */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---------- Memory ---------- */
/* Use stdlib malloc/free so allocations land in PSRAM when configured */
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM == 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc
#endif

/* ---------- Color ---------- */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0 /* RGB bus — no byte swap */
#define LV_COLOR_SCREEN_TRANSP 0

/* ---------- HAL ---------- */
/* Tick is provided by esp_timer in lvgl_v8_port.cpp — use custom tick */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM == 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

/* ---------- Drawing ---------- */
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4

/* ---------- Logging ---------- */
#define LV_USE_LOG 0
#if LV_USE_LOG
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#endif

/* ---------- Asserts ---------- */
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/* ---------- Fonts ---------- */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ---------- User data ---------- */
#define LV_USE_USER_DATA 1

/* ---------- Performance monitor ---------- */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* ---------- Features ---------- */
#define LV_USE_FILESYSTEM 0
#define LV_USE_SNAPSHOT 0

/* ---------- Widgets ---------- */
#define LV_USE_CANVAS 1
#define LV_USE_ANIMATION 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_ROLLER 1
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_CHART 0
#define LV_USE_METER 0

/* ---------- Themes ---------- */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/* ---------- GPU / DMA2D ---------- */
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_SWM341_DMA2D 0

#endif /*LV_CONF_H*/
