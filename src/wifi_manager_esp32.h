#ifndef WIFI_MANAGER_ESP32_H
#define WIFI_MANAGER_ESP32_H

#include "board_driver.h"
#include "ota_updater.h"
#include "stockfish_settings.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

// Forward declarations
struct LichessConfig;
class MoveHistory;

// ---------------------------
// WiFi Configuration
// ---------------------------
#define AP_SSID "OpenChess"
#define AP_PASSWORD "chess123"
#define AP_PORT 80
// Your WiFi Network Credentials for internet connection (can also be set via web interface)
#define SECRET_SSID "YOURSSID"
#define SECRET_PASS "YOURWIFIPW"
// Set to 1 if the same SSID is available on multiple channels. Will scan all channels and sort by signal strength.
// Will take longer to connect but helps find the AP with best signal in a mesh network.
// Don't enable unless you have multiple APs with the same SSID on different channels, otherwise it just adds unnecessary delay (around +10 seconds) to WiFi connection.
#define WIFI_SCAN_ALL_CHANNELS 1

// ---------------------------
// WiFi Manager Class for ESP32
// ---------------------------
class WiFiManagerESP32 {
 private:
  AsyncWebServer server;

  Preferences prefs;
  String wifiSSID;
  String wifiPassword;
  String gameMode;
  String lichessToken;

  BotConfig botConfig = {StockfishSettings::medium(), true};
  bool scanAllChannels;

  MoveHistory* moveHistory;
  BoardDriver* boardDriver;
  String currentFen;
  float boardEvaluation;

  // Board edit storage (pending edits from web interface)
  String pendingFenEdit;
  volatile bool hasPendingEdit;

  // Pending resign/draw from web interface
  volatile bool hasPendingResign;
  volatile bool hasPendingDraw;
  char pendingResignColor; // 'w' or 'b' — the side resigning

  // Promotion state for web-based piece selection
  struct PromotionState {
    volatile bool pending; // True while waiting for web client to choose a piece
    volatile char choice;  // Piece chosen by web client ('q','r','b','n') or ' ' if none yet
    char color;            // 'w' or 'b' — color of the promoting pawn
    void reset() {
      pending = false;
      choice = ' ';
      color = ' ';
    }
  };
  PromotionState promotion;

  // Web client heartbeat (tracks whether board.html is actively polling)
  unsigned long lastBoardPollTime; // millis() of last /board-update GET request

  // Deferred WiFi reconnection (set by web handler, processed in main loop)
  String pendingWiFiSSID;
  String pendingWiFiPassword;
  volatile bool hasPendingWiFi;

  // Web interface methods
  String getWiFiInfoJSON();
  String getBoardUpdateJSON();
  String getLichessInfoJSON();
  String getBoardSettingsJSON();
  void handleBoardEditSuccess(AsyncWebServerRequest* request);
  void handlePromotion(AsyncWebServerRequest* request);
  void handleConnectWiFi(AsyncWebServerRequest* request);
  void handleGameSelection(AsyncWebServerRequest* request);
  void handleSaveLichessToken(AsyncWebServerRequest* request);
  void handleBoardSettings(AsyncWebServerRequest* request);
  void handleBoardCalibration(AsyncWebServerRequest* request);
  void handleResign(AsyncWebServerRequest* request);
  void handleDraw(AsyncWebServerRequest* request);
  void getHardwareConfigJSON(AsyncWebServerRequest* request);
  void handleHardwareConfig(AsyncWebServerRequest* request);
  void handleGamesRequest(AsyncWebServerRequest* request);
  void handleDeleteGame(AsyncWebServerRequest* request);
  // OTA update handlers
  void handleOtaStatus(AsyncWebServerRequest* request);
  void handleOtaSettings(AsyncWebServerRequest* request);
  void handleOtaApply(AsyncWebServerRequest* request);
  // ESPAsyncWebServer body handlers receive data in chunks via callbacks (data, len, index, total),
  // not as a continuous Stream. This means we can't reuse the Stream-based OtaUpdater methods directly.
  // For firmware: we call Update.begin/write/end incrementally across chunks.
  // For web assets: we buffer the TAR to a temp file first, then pass it as a Stream to the TAR parser
  // (the TAR format requires sequential 512-byte header reads that can't be split across async chunks).
  void onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);

  // OTA state
  OtaUpdater otaUpdater;
  OtaUpdateInfo lastUpdateInfo;
  bool autoOtaEnabled;
  // Temporary file for web asset TAR upload (needed because the TAR parser requires a seekable Stream)
  File otaTarFile;

 public:
  WiFiManagerESP32(BoardDriver* boardDriver, MoveHistory* moveHistory);
  void begin();

  // OTA update support
  OtaUpdater& getOtaUpdater() { return otaUpdater; }
  bool isAutoOtaEnabled() const { return autoOtaEnabled; }

  // Configuration getters
  String getWiFiSSID() { return wifiSSID; }
  String getWiFiPassword() { return wifiPassword; }
  // Game selection via web
  int getSelectedGameMode() const { return gameMode.toInt(); }
  void resetGameSelection() { gameMode = "0"; };
  // Bot configuration
  BotConfig getBotConfig() { return botConfig; }
  // Lichess configuration
  LichessConfig getLichessConfig();
  String getLichessToken() { return lichessToken; }
  // Board state management (FEN-based)
  void updateBoardState(const String& fen, float evaluation = 0.0f);
  String getCurrentFen() const { return currentFen; }
  float getEvaluation() const { return boardEvaluation; }
  // Board edit management (FEN-based)
  bool getPendingBoardEdit(String& fenOut);
  void clearPendingEdit();
  // Resign/Draw management (from web interface)
  bool getPendingResign(char& resignColor);
  bool getPendingDraw();
  void clearPendingResign();
  void clearPendingDraw();
  // Promotion management (from web interface)
  void startPromotionWait(char color);
  bool isPromotionPending() const { return promotion.pending; }
  char getPromotionChoice() const { return promotion.choice; }
  void clearPromotion();
  // Web client connection check
  bool isWebClientConnected() const;
  // WiFi connection management
  bool connectToWiFi(const String& ssid, const String& password, bool fromWeb = false);
  // Call from main loop to process deferred WiFi reconnection
  void checkPendingWiFi();
};

#endif // WIFI_MANAGER_ESP32_H
