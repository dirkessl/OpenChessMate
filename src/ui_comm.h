#pragma once

#include <Arduino.h>

// Simple UI communication over UART with a secondary ESP32 running LVGL.
// Line-based messages: <TYPE>|key1=val1;key2=val2\n

typedef void (*ui_touch_handler_t)(const char* action, int x, int y);

namespace UIComm {
void begin(int baud = 115200, int rxPin = 16, int txPin = 17);
void loop();
void setTouchHandler(ui_touch_handler_t h);

// Outgoing messages
void sendStateUpdate(const String& fen, const String& lastMove);
void sendHintResponse(const String& san);
void sendMode(int mode);
void sendSimple(const String& msg);
} // namespace UIComm
