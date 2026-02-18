#include "wifi_manager_esp32.h"
#include "chess_lichess.h"
#include "chess_utils.h"
#include "move_history.h"
#include "version.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>

static const char* INITIAL_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

WiFiManagerESP32::WiFiManagerESP32(BoardDriver* bd, MoveHistory* mh) : boardDriver(bd), moveHistory(mh), server(AP_PORT), wifiSSID(SECRET_SSID), wifiPassword(SECRET_PASS), gameMode("0"), lichessToken(""), botConfig(), currentFen(INITIAL_FEN), hasPendingEdit(false), boardEvaluation(0.0f), otaUpdater(bd), autoOtaEnabled(false) {}

void WiFiManagerESP32::begin() {
  Serial.println("=== Starting OpenChess WiFi Manager (ESP32) ===");

  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - WiFi credentials not loaded");
  } else {
    prefs.begin("wifiCreds", false);
    if (prefs.isKey("ssid")) {
      wifiSSID = prefs.getString("ssid", SECRET_SSID);
      wifiPassword = prefs.getString("pass", SECRET_PASS);
    }
    prefs.end();

    // Load Lichess token
    prefs.begin("lichess", false);
    if (prefs.isKey("token")) {
      lichessToken = prefs.getString("token", "");
    }
    prefs.end();
    if (lichessToken.length() > 0) {
      Serial.println("Lichess API token loaded from NVS");
    }

    // Load OTA auto-update preference
    prefs.begin("ota", false);
    autoOtaEnabled = prefs.getBool("autoUpdate", false);
    prefs.end();
  }
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("ERROR: Failed to create Access Point!");
    return;
  }
  bool connected = connectToWiFi(wifiSSID, wifiPassword);
  Serial.println("==== WiFi Connection Information ====");
  Serial.println("A WiFi Access Point was created:");
  Serial.println("- SSID: " AP_SSID);
  Serial.println("- Password: " AP_PASSWORD);
  Serial.println("- Website: http://" + WiFi.softAPIP().toString());
  Serial.println("- MAC Address: " + WiFi.softAPmacAddress());
  if (connected) {
    Serial.println("Connected to WiFi network: ");
    Serial.println("- SSID: " + wifiSSID);
    Serial.println("- Password: " + wifiPassword);
    Serial.println("- Website: http://" + WiFi.localIP().toString());
  } else {
    Serial.println("Configure WiFi credentials from the web interface to join your WiFi network (Stockfish needs internet)");
  }
  Serial.println("=====================================");

  // Set up web server routes with async handlers
  server.on("/board-update", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardUpdateJSON()); });
  server.on("/board-update", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardEditSuccess(request); });
  server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getWiFiInfoJSON()); });
  server.on("/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleConnectWiFi(request); });
  server.on("/gameselect", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleGameSelection(request); });
  server.on("/lichess", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getLichessInfoJSON()); });
  server.on("/lichess", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleSaveLichessToken(request); });
  server.on("/board-settings", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardSettingsJSON()); });
  server.on("/board-settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardSettings(request); });
  server.on("/board-calibrate", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardCalibration(request); });
  server.on("/games", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleGamesRequest(request); });
  server.on("/games", HTTP_DELETE, [this](AsyncWebServerRequest* request) { this->handleDeleteGame(request); });
  // OTA update endpoints
  server.on("/ota/status", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleOtaStatus(request); });
  server.on("/ota/settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaSettings(request); });
  server.on("/ota/check", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleOtaCheck(request); });
  server.on("/ota/apply", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaApply(request); });
  // OTA manual upload endpoints — JS sends raw binary body (application/octet-stream),
  // so only the body handler (3rd callback) fires; the multipart file handler (2nd) is unused.
  server.on(
      "/ota/upload/firmware", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onFirmwareUploadBody(request, data, len, index, total); });
  server.on(
      "/ota/upload/web", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onWebAssetsUploadBody(request, data, len, index, total); });
  // Serve sound files directly (no gzip variant exists, avoids .gz probe errors)
  server.serveStatic("/sounds/", LittleFS, "/sounds/").setTryGzipFirst(false);
  // Serve piece SVGs with aggressive caching, otherwise chrome doesn't actually use the cached versions
  server.serveStatic("/pieces/", LittleFS, "/pieces/").setCacheControl("max-age=31536000, immutable");
  // Serve all other static files from LittleFS (gzip handled automatically)
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println("Web server started on port 80");
}

String WiFiManagerESP32::getBoardUpdateJSON() {
  JsonDocument doc;
  doc["fen"] = currentFen;
  doc["evaluation"] = serialized(String(boardEvaluation, 2));
  String output;
  serializeJson(doc, output);
  return output;
}

String WiFiManagerESP32::getWiFiInfoJSON() {
  JsonDocument doc;
  doc["ssid"] = wifiSSID;
  doc["password"] = wifiPassword;
  doc["connected"] = (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  doc["ap_ssid"] = AP_SSID;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["local_ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardEditSuccess(AsyncWebServerRequest* request) {
  if (request->hasArg("fen")) {
    pendingFenEdit = request->arg("fen");
    hasPendingEdit = true;
    Serial.println("Board edit received (FEN): " + pendingFenEdit);
    request->send(200, "text/plain", "OK");
  } else {
    Serial.println("Board edit failed: no FEN parameter");
    request->send(400, "text/plain", "Missing FEN parameter");
  }
}

void WiFiManagerESP32::handleConnectWiFi(AsyncWebServerRequest* request) {
  String newWifiSSID = "";
  String newWifiPassword = "";
  if (request->hasArg("ssid"))
    newWifiSSID = request->arg("ssid");
  if (request->hasArg("password"))
    newWifiPassword = request->arg("password");

  if (newWifiSSID.length() >= 1 && newWifiPassword.length() >= 5 && (newWifiSSID != wifiSSID || newWifiPassword != wifiPassword)) {
    request->send(200, "text/plain", "OK");
    if (connectToWiFi(newWifiSSID, newWifiPassword, true)) {
      if (!ChessUtils::ensureNvsInitialized())
        Serial.println("NVS init failed - WiFi credentials not saved");
      prefs.begin("wifiCreds", false);
      prefs.putString("ssid", newWifiSSID);
      prefs.putString("pass", newWifiPassword);
      prefs.end();
      wifiSSID = newWifiSSID;
      wifiPassword = newWifiPassword;
      Serial.println("WiFi credentials updated and saved to NVS");
    }
    return;
  }
  request->send(400, "text/plain", "ERROR");
}

void WiFiManagerESP32::handleGameSelection(AsyncWebServerRequest* request) {
  int mode = 0;
  if (request->hasArg("gamemode"))
    mode = request->arg("gamemode").toInt();
  gameMode = String(mode);
  // If bot game mode, also handle bot config
  if (mode == 2) {
    if (request->hasArg("difficulty") && request->hasArg("playerColor")) {
      switch (request->arg("difficulty").toInt()) {
        case 1:
          botConfig.stockfishSettings = StockfishSettings::easy();
          break;
        case 2:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
        case 3:
          botConfig.stockfishSettings = StockfishSettings::hard();
          break;
        case 4:
          botConfig.stockfishSettings = StockfishSettings::expert();
          break;
        default:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
      }
      botConfig.playerIsWhite = request->arg("playerColor") == "white";
      Serial.printf("Bot configuration received: Depth=%d, Player is %s\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
    } else {
      request->send(400, "text/plain", "Missing bot parameters");
      return;
    }
  }
  // If Lichess mode, verify token exists
  if (mode == 3) {
    if (lichessToken.length() == 0) {
      request->send(400, "text/plain", "No Lichess API token configured");
      return;
    }
    Serial.println("Lichess mode selected via web");
  }
  Serial.println("Game mode selected via web: " + gameMode);
  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getLichessInfoJSON() {
  // Don't expose the actual token, just whether it exists and a masked version
  String maskedToken = "";
  if (lichessToken.length() > 8) {
    maskedToken = lichessToken.substring(0, 4) + "..." + lichessToken.substring(lichessToken.length() - 4);
  } else if (lichessToken.length() > 0) {
    maskedToken = "****";
  }
  JsonDocument doc;
  doc["hasToken"] = (lichessToken.length() > 0);
  doc["maskedToken"] = maskedToken;
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleSaveLichessToken(AsyncWebServerRequest* request) {
  if (!request->hasArg("token")) {
    request->send(400, "text/plain", "Missing token parameter");
    return;
  }

  String newToken = request->arg("token");
  newToken.trim();

  if (newToken.length() < 10) {
    request->send(400, "text/plain", "Token too short");
    return;
  }

  // Save to NVS
  if (!ChessUtils::ensureNvsInitialized()) {
    request->send(500, "text/plain", "NVS init failed");
    return;
  }

  prefs.begin("lichess", false);
  prefs.putString("token", newToken);
  prefs.end();

  lichessToken = newToken;
  Serial.println("Lichess API token saved to NVS");

  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getBoardSettingsJSON() {
  JsonDocument doc;
  doc["brightness"] = boardDriver->getBrightness();
  doc["dimMultiplier"] = boardDriver->getDimMultiplier();
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardSettings(AsyncWebServerRequest* request) {
  bool changed = false;

  if (request->hasArg("brightness")) {
    int brightness = request->arg("brightness").toInt();
    if (brightness >= 0 && brightness <= 255) {
      boardDriver->setBrightness((uint8_t)brightness);
      changed = true;
    }
  }

  if (request->hasArg("dimMultiplier")) {
    int dimMult = request->arg("dimMultiplier").toInt();
    if (dimMult >= 0 && dimMult <= 100) {
      boardDriver->setDimMultiplier((uint8_t)dimMult);
      changed = true;
    }
  }

  if (changed) {
    boardDriver->saveLedSettings();
    Serial.println("Board settings updated via web interface");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "No valid settings provided");
  }
}

void WiFiManagerESP32::handleBoardCalibration(AsyncWebServerRequest* request) {
  boardDriver->triggerCalibration();
  request->send(200, "text/plain", "Calibration will start on next reboot");
}

LichessConfig WiFiManagerESP32::getLichessConfig() {
  LichessConfig config;
  config.apiToken = lichessToken;
  return config;
}

void WiFiManagerESP32::updateBoardState(const String& fen, float evaluation) {
  currentFen = fen;
  boardEvaluation = evaluation;
}

bool WiFiManagerESP32::getPendingBoardEdit(String& fenOut) {
  if (hasPendingEdit) {
    fenOut = pendingFenEdit;
    return true;
  }
  return false;
}

void WiFiManagerESP32::clearPendingEdit() {
  currentFen = pendingFenEdit;
  hasPendingEdit = false;
}

bool WiFiManagerESP32::connectToWiFi(const String& ssid, const String& password, bool fromWeb) {
  if (!fromWeb && WiFi.status() == WL_CONNECTED) {
    Serial.println("Already connected to WiFi");
    return true;
  }
  Serial.println("=== Connecting to WiFi Network" + String(fromWeb ? "(from web)" : "") + " ===");
  Serial.printf("SSID: %s\nPassword: %s\n", ssid.c_str(), password.c_str());

  // ESP32 can run both AP and Station modes simultaneously
  WiFi.mode(WIFI_AP_STA);
#if defined(WIFI_SCAN_ALL_CHANNELS) && WIFI_SCAN_ALL_CHANNELS != 0
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    boardDriver->showConnectingAnimation();
    attempts++;
    Serial.printf("Connection attempt %d/10 - Status: %d\n", attempts, WiFi.status());
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi!");
    return true;
  } else {
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

void WiFiManagerESP32::handleGamesRequest(AsyncWebServerRequest* request) {
  if (request->hasArg("id")) {
    String idStr = request->arg("id");

    // GET /games?id=live1 — return live moves file directly
    if (idStr == "live1") {
      if (!MoveHistory::quietExists("/games/live.bin")) {
        request->send(404, "text/plain", "No live game");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=live2 — return live FEN table file directly
    if (idStr == "live2") {
      if (!MoveHistory::quietExists("/games/live_fen.bin")) {
        request->send(404, "text/plain", "No live FEN table");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live_fen.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=N — return binary of game N
    int id = idStr.toInt();
    if (id <= 0) {
      request->send(400, "text/plain", "Invalid game id");
      return;
    }

    String path = MoveHistory::gamePath(id);
    if (!MoveHistory::quietExists(path.c_str())) {
      request->send(404, "text/plain", "Game not found");
      return;
    }
    // Serve file directly from LittleFS
    AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, "application/octet-stream", true);
    request->send(response);
  } else {
    // GET /games — return JSON list of all saved games
    request->send(200, "application/json", moveHistory->getGameListJSON());
  }
}

void WiFiManagerESP32::handleDeleteGame(AsyncWebServerRequest* request) {
  if (!request->hasArg("id")) {
    request->send(400, "text/plain", "Missing id parameter");
    return;
  }

  int id = request->arg("id").toInt();
  if (id <= 0) {
    request->send(400, "text/plain", "Invalid game id");
    return;
  }

  if (moveHistory->deleteGame(id))
    request->send(200, "text/plain", "OK");
  else
    request->send(404, "text/plain", "Game not found");
}

// ========== OTA Update Handlers ==========

void WiFiManagerESP32::handleOtaStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["autoUpdate"] = autoOtaEnabled;
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleOtaSettings(AsyncWebServerRequest* request) {
  if (request->hasArg("autoUpdate")) {
    autoOtaEnabled = request->arg("autoUpdate") == "1";
    if (ChessUtils::ensureNvsInitialized()) {
      Preferences p;
      p.begin("ota", false);
      p.putBool("autoUpdate", autoOtaEnabled);
      p.end();
    }
    Serial.printf("OTA: Auto-update %s\n", autoOtaEnabled ? "enabled" : "disabled");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "Missing parameter");
  }
}

void WiFiManagerESP32::handleOtaCheck(AsyncWebServerRequest* request) {
  lastUpdateInfo = otaUpdater.checkForUpdate();
  JsonDocument doc;
  doc["available"] = lastUpdateInfo.available;
  doc["version"] = lastUpdateInfo.version;
  doc["currentVersion"] = FIRMWARE_VERSION;
  doc["hasFirmware"] = lastUpdateInfo.firmwareUrl.length() > 0;
  doc["hasWebAssets"] = lastUpdateInfo.webAssetsUrl.length() > 0;
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

// Parameters passed to the OTA apply task via heap allocation (avoids static variable race conditions)
struct OtaApplyParams {
  OtaUpdateInfo info;
  OtaUpdater* updater;
};

void WiFiManagerESP32::handleOtaApply(AsyncWebServerRequest* request) {
  if (!lastUpdateInfo.available) {
    request->send(400, "text/plain", "No update available. Check for updates first.");
    return;
  }

  request->send(200, "text/plain", "Updating... The board will reboot when complete.");

  // Run update in a separate task to not block the web server response.
  // Heap-allocate params so the info survives after this function returns.
  auto* params = new OtaApplyParams{lastUpdateInfo, &otaUpdater};

  xTaskCreate(
      [](void* param) {
        auto* p = static_cast<OtaApplyParams*>(param);
        delay(500); // Give time for the HTTP response to be sent
        p->updater->applyUpdate(p->info);
        delete p;
        vTaskDelete(NULL);
      },
      "ota_apply", 8192, params, 1, NULL);
}

void WiFiManagerESP32::onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Can't use applyFirmwareFromStream() here — ESPAsyncWebServer delivers the body in async chunks,
  // not as a Stream. We call Update.begin/write/end incrementally across chunk callbacks instead.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Firmware upload started (%d bytes)\n", total);
    if (!Update.begin(total, U_FLASH)) {
      Serial.printf("OTA: Not enough space: %s\n", Update.errorString());
      return;
    }
  }
  if (Update.isRunning()) {
    if (Update.write(data, len) != len) {
      Serial.printf("OTA: Write failed: %s\n", Update.errorString());
      Update.abort();
    }
  }
  if (index + len == total) {
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
    if (!Update.isRunning()) {
      // Update.begin() failed or a write error aborted the update
      request->send(500, "text/plain", "Firmware update failed");
    } else if (Update.end(true)) {
      Serial.println("OTA: Firmware upload complete, rebooting...");
      request->send(200, "text/plain", "Firmware updated! Rebooting...");
      boardDriver->flashBoardAnimation(LedColors::Blue, 2);
      delay(500);
      ESP.restart();
    } else {
      Serial.printf("OTA: Finalize failed: %s\n", Update.errorString());
      request->send(500, "text/plain", String("Firmware update failed: ") + Update.errorString());
    }
  }
}

void WiFiManagerESP32::onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Can't use applyWebAssetsFromStream() directly — the TAR parser reads 512-byte headers
  // sequentially from a Stream, but async chunks can split a header across callbacks.
  // So we buffer the TAR to a temp file, then pass it as a Stream to the parser.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Web assets upload started (%d bytes)\n", total);
    otaTarFile = LittleFS.open("/ota_temp.tar", "w");
    if (!otaTarFile) {
      Serial.println("OTA: Failed to create temp file");
      return;
    }
  }
  if (otaTarFile)
    otaTarFile.write(data, len);
  if (index + len == total) {
    if (otaTarFile) {
      otaTarFile.close();
      File tarFile = LittleFS.open("/ota_temp.tar", "r");
      if (tarFile) {
        size_t tarSize = tarFile.size();
        bool success = otaUpdater.applyWebAssetsFromStream(tarFile, tarSize);
        tarFile.close();
        LittleFS.remove("/ota_temp.tar");
        if (success)
          request->send(200, "text/plain", "Web assets updated successfully!");
        else
          request->send(500, "text/plain", "Web assets update failed");
      } else {
        request->send(500, "text/plain", "Failed to read temp file");
      }
    } else {
      request->send(500, "text/plain", "Upload failed");
    }
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
  }
}
