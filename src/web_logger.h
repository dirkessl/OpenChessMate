#ifndef WEB_LOGGER_H
#define WEB_LOGGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define HISTORY_LINES 30
#define MAX_LINE_LEN 128

class WebLogger : public Print {
 public:
  WebLogger();
  void begin(AsyncWebServer& server);

  // Print interface
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  using Print::write;

 private:
  char history[HISTORY_LINES][MAX_LINE_LEN + 1];
  size_t head;    // current write slot
  size_t count;   // number of committed entries
  size_t lineLen; // bytes in the current history slot
  SemaphoreHandle_t mutex;
  AsyncEventSource* events;

  void appendChar(uint8_t c);
  void sendLine();
  static void heartbeatTask(void* param);
};

extern WebLogger webLog;

#endif // WEB_LOGGER_H
