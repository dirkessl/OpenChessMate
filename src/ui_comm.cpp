#include "ui_comm.h"

static HardwareSerial UI_SERIAL(2);
static String recvLine = "";
static ui_touch_handler_t touchHandler = nullptr;

namespace UIComm {

void begin(int baud, int rxPin, int txPin) {
  UI_SERIAL.begin(baud, SERIAL_8N1, rxPin, txPin);
}

static void callTouchHandler(const String& action, int x, int y) {
  if (touchHandler) {
    touchHandler(action.c_str(), x, y);
  }
}

static String getValueForKey(const String& payload, const String& key) {
  int idx = payload.indexOf(key + "=");
  if (idx < 0) return String();
  idx += key.length() + 1;
  int end = payload.indexOf(';', idx);
  if (end < 0) end = payload.length();
  return payload.substring(idx, end);
}

static void parseLine(const String& line) {
  if (line.length() == 0) return;
  int sep = line.indexOf('|');
  String type = (sep > 0) ? line.substring(0, sep) : line;
  String payload = (sep > 0) ? line.substring(sep + 1) : String();
  type.trim();
  payload.trim();
  if (type == "TOUCH") {
    String action = getValueForKey(payload, "action");
    int x = -1, y = -1;
    if (action == "board") {
      // Board cell touches send row= and col= instead of x/y
      String rs = getValueForKey(payload, "row");
      String cs = getValueForKey(payload, "col");
      x = rs.length() ? rs.toInt() : -1; // row
      y = cs.length() ? cs.toInt() : -1; // col
    } else if (action == "mode") {
      // Mode selection sends value=N
      String vs = getValueForKey(payload, "value");
      x = vs.length() ? vs.toInt() : -1;
    } else {
      String xs = getValueForKey(payload, "x");
      String ys = getValueForKey(payload, "y");
      x = xs.length() ? xs.toInt() : -1;
      y = ys.length() ? ys.toInt() : -1;
    }
    callTouchHandler(action, x, y);
  } else if (type == "CMD") {
    // Generic command from UI (log for now)
    Serial.printf("UI CMD: %s\n", payload.c_str());
  } else {
    Serial.printf("Unknown UI msg: %s\n", line.c_str());
  }
}

void loop() {
  while (UI_SERIAL.available()) {
    char c = (char)UI_SERIAL.read();
    if (c == '\r') continue;
    if (c == '\n') {
      parseLine(recvLine);
      recvLine = "";
    } else {
      recvLine += c;
      if (recvLine.length() > 1024) recvLine.remove(0, recvLine.length() - 1024);
    }
  }
}

void setTouchHandler(ui_touch_handler_t h) {
  touchHandler = h;
}

// Outgoing
void sendSimple(const String& msg) {
  UI_SERIAL.print(msg);
  UI_SERIAL.print('\n');
}

void sendStateUpdate(const String& fen, const String& lastMove) {
  String payload = "STATE|fen=" + fen;
  if (lastMove.length() > 0)
    payload += ";move=" + lastMove;
  sendSimple(payload);
}

void sendHintResponse(const String& san) {
  String payload = "HINT|move=" + san;
  sendSimple(payload);
}

void sendMode(int mode) {
  sendSimple("MODE|value=" + String(mode));
}

} // namespace UIComm
