/* LVGL configuration for ESP32 ui_slave */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Memory */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U) /* 48KB for LVGL heap */

/* Color depth */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Tick */
#define LV_TICK_CUSTOM 0

/* Fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Features */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_LOG 0
#define LV_USE_USER_DATA 1
#define LV_USE_FILESYSTEM 0

/* Widgets */
#define LV_USE_CANVAS 1

/* Animations */
#define LV_USE_ANIMATION 1

#endif /*LV_CONF_H*/
