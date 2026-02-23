/* Minimal LVGL configuration for desktop simulator */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Memory and rendering */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (512U * 1024U)

/* Color depth */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Use desktop tick */
#define LV_TICK_CUSTOM 0

/* Fonts – enable a small built-in font so LVGL always has a valid default.
 * The custom chess font is applied at runtime via styles. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Features */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_LOG 0
#define LV_USE_USER_DATA 1
#define LV_USE_FILESYSTEM 0

/* Animations, timers */
#define LV_USE_ANIMATION 1

/* Canvas (for rotated clock rendering) */
#define LV_USE_CANVAS 1

#endif /*LV_CONF_H*/
