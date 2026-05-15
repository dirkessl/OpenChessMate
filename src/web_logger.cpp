#include "web_logger.h"

WebLogger webLog;

WebLogger::WebLogger() : head(0), count(0), lineLen(0), events(nullptr) {
  mutex = xSemaphoreCreateMutex();
}

void WebLogger::begin(AsyncWebServer& server) {
  events = new AsyncEventSource("/logs/stream");
  events->onConnect([this](AsyncEventSourceClient* client) {
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(25)) != pdTRUE) return;
    client->send("1", "clear", 0, 1000);
    size_t start = (head + HISTORY_LINES - count) % HISTORY_LINES;
    for (size_t i = 0; i < count; i++) client->send(history[(start + i) % HISTORY_LINES], "log");
    xSemaphoreGive(mutex);
  });
  server.addHandler(events);
  xTaskCreate(heartbeatTask, "log_heartbeat", 2048, this, 1, nullptr);
}

void WebLogger::heartbeatTask(void* param) {
  WebLogger* logger = static_cast<WebLogger*>(param);
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (logger->events && logger->events->count() > 0)
      logger->events->send("1", "heartbeat", 0, 1000);
  }
}

void WebLogger::sendLine() {
  history[head][lineLen] = '\0';
  if (events && events->count() > 0)
    events->send(history[head], "log");
  head = (head + 1) % HISTORY_LINES;
  if (count < HISTORY_LINES) count++;
  lineLen = 0;
}

void WebLogger::appendChar(uint8_t c) {
  if (c == '\n') {
    sendLine();
  } else if (c != '\r') {
    if (lineLen >= MAX_LINE_LEN) sendLine();
    if (lineLen == 0 && count == HISTORY_LINES) count--;
    history[head][lineLen++] = (char)c;
  }
}

size_t WebLogger::write(uint8_t c) {
  return write(&c, 1);
}

size_t WebLogger::write(const uint8_t* buffer, size_t size) {
  Serial.write(buffer, size);
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(1)) != pdTRUE) return size;
  for (size_t i = 0; i < size; i++) appendChar(buffer[i]);
  xSemaphoreGive(mutex);
  return size;
}
