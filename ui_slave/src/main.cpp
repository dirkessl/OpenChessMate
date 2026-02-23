/**
 * @file main.cpp
 * @brief OpenChessMate UI Slave — VIEWE 4.3" ESP32-S3 display (portrait 480×800)
 *
 * Platform layer: initialises the display panel (ESP32_Display_Panel),
 * sets up LVGL via lvgl_v8_port (sw_rotate for portrait), creates the
 * shared chess UI, and polls serial for protocol messages from the master.
 *
 * Hardware:
 *   Board:   VIEWE UEDX80480043E-WB-A (ESP32-S3-N16R8)
 *   Display: 800×480 RGB IPS (ST7262E43-G4) — rotated to 480×800 portrait
 *   Touch:   GT911 capacitive (I2C)
 *   Serial:  UART0 on IO43/IO44 (CH340C USB-UART + board UART header)
 */

#include "chess_ui.h"
#include "lvgl_v8_port.h"
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <lvgl.h>

// ---------------------------------------------------------------------------
// Serial protocol — line-buffered reader
// ---------------------------------------------------------------------------
static String s_rx_buf;

/// Send a protocol message to the master board via Serial (UART0).
static void platformSend(const char* msg) {
  Serial.print(msg);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  // UART0 on IO43(TX) / IO44(RX) — shared between CH340C USB-C and UART header
  Serial.begin(115200);
  delay(200);
  Serial.println("UI Slave (VIEWE 4.3\") starting...");

  // ---- Display panel init (LCD + touch + backlight) ----
  ESP_Panel* panel = new ESP_Panel();
  panel->init();
  panel->begin();
  Serial.println("Display panel initialised");

  // ---- LVGL port (buffers, flush, touch, tick, FreeRTOS task) ----
  lvgl_port_init(panel->getLcd(), panel->getTouch());
  Serial.println("LVGL port initialised");

  // ---- Chess UI (portrait 480×800, runs under LVGL mutex) ----
  lvgl_port_lock(0);
  chess_ui_create(480, 800, &lv_font_montserrat_14, platformSend);
  lvgl_port_unlock();

  Serial.println("UI ready");
}

void loop() {
  // Poll serial for newline-terminated messages from the master
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (s_rx_buf.length() > 0) {
        lvgl_port_lock(0);
        chess_ui_handle_message(s_rx_buf.c_str());
        lvgl_port_unlock();
        s_rx_buf = "";
      }
    } else {
      s_rx_buf += c;
      if (s_rx_buf.length() > 1024)
        s_rx_buf = s_rx_buf.substring(s_rx_buf.length() - 1024);
    }
  }
  delay(5);
}
