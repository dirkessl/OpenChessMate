/**
 * @file lvgl_v8_port.h
 * @brief LVGL v8 port for VIEWE 4.3" ESP32-S3 display (portrait 480x800).
 *        Provides display flush, touch read, tick timer, and FreeRTOS task.
 *        Uses sw_rotate to present native 800x480 as 480x800 portrait.
 *
 * Based on VIEWE's official LVGL v8 example:
 *   https://github.com/VIEWESMART/UEDX80480043E-WB-A-4.3inch-Touch-Display
 */
#ifndef LVGL_V8_PORT_H
#define LVGL_V8_PORT_H

#include <ESP_Panel_Library.h>
#include <lvgl.h>

/* ==========================================================================
 * Configuration
 * ========================================================================== */

/**
 * LVGL buffer height (lines). Partial-buffer mode: smaller = less RAM,
 * larger = fewer flushes.  48 lines is a good balance for sw_rotate.
 */
#define LVGL_PORT_BUFFER_SIZE_HEIGHT (48)

/**
 * Tearing avoidance mode:
 *   0 = none  (partial buffer, required for sw_rotate / portrait mode)
 *   1 = full-size + direct draw (no tearing, uses more RAM, no sw_rotate)
 *   2 = full-size + copy mode    (no tearing, uses most RAM, no sw_rotate)
 *   3 = partial + copy (smaller buffer, some tearing risk)
 *
 * NOTE: sw_rotate (portrait) requires mode 0 — it cannot use direct_mode.
 */
#define LVGL_PORT_AVOID_TEARING_MODE (0)

/** LVGL handler task stack size (bytes) */
#define LVGL_PORT_TASK_STACK_SIZE (6 * 1024)

/** LVGL handler task priority */
#define LVGL_PORT_TASK_PRIORITY (2)

/** Core to pin the LVGL task to (-1 = no pinning) */
#define LVGL_PORT_TASK_CORE (1)

/** Maximum time to wait for the LVGL mutex (ms).  0 = forever. */
#define LVGL_PORT_LOCK_TIMEOUT_MS (0)

/* Rounder callback for RGB panels (align to 2px boundary) */
#define LVGL_PORT_ROUNDER_CB (1)

/* ==========================================================================
 * API
 * ========================================================================== */

/**
 * Initialise the LVGL port: creates buffers, display driver, input driver,
 * tick timer, FreeRTOS mutex + handler task.
 *
 * @param lcd   Pointer to the already-initialised LCD from ESP32_Display_Panel
 * @param touch Pointer to the already-initialised Touch (may be NULL)
 */
void lvgl_port_init(ESP_PanelLcd* lcd, ESP_PanelTouch* touch);

/**
 * Lock the LVGL mutex. Call before any LVGL API from outside the handler task.
 * @param timeout_ms  Max wait in ms (0 = forever)
 * @return true if lock acquired
 */
bool lvgl_port_lock(uint32_t timeout_ms = LVGL_PORT_LOCK_TIMEOUT_MS);

/** Unlock the LVGL mutex. */
void lvgl_port_unlock(void);

#endif /* LVGL_V8_PORT_H */
