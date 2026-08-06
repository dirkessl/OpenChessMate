#ifndef SIMULATOR

#include "ota_updater_ui.h"
#include "version.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>

const char* OtaUpdaterUI::getCurrentVersion() {
  return FIRMWARE_VERSION;
}

bool OtaUpdaterUI::isNewerVersion(const String& current, const String& remote) {
  if (current == "dev") return true;
  int cMa = 0, cMi = 0, cPa = 0, rMa = 0, rMi = 0, rPa = 0;
  sscanf(current.c_str(), "%d.%d.%d", &cMa, &cMi, &cPa);
  sscanf(remote.c_str(),  "%d.%d.%d", &rMa, &rMi, &rPa);
  if (rMa != cMa) return rMa > cMa;
  if (rMi != cMi) return rMi > cMi;
  return rPa > cPa;
}

bool OtaUpdaterUI::beginHttpGet(HTTPClient& http, const String& url, int timeoutMs) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  http.setUserAgent("OpenChessMateUI/" FIRMWARE_VERSION);
  if (!http.begin(url)) {
    Serial.println("OTA-UI: Failed to connect: " + url);
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA-UI: HTTP %d from: %s\n", code, url.c_str());
    http.end();
    return false;
  }
  return true;
}

OtaUpdateInfoUI OtaUpdaterUI::checkForUpdate() {
  OtaUpdateInfoUI info;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA-UI: No WiFi, skipping update check");
    return info;
  }

  HTTPClient http;
  if (!beginHttpGet(http, OTA_GITHUB_API_URL)) return info;
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("OTA-UI: Failed to parse release JSON");
    return info;
  }

  String tag = doc["tag_name"] | "";
  if (tag.startsWith("v")) tag = tag.substring(1);
  if (tag.isEmpty()) return info;

  info.version = tag;
  if (!isNewerVersion(FIRMWARE_VERSION, tag)) {
    Serial.printf("OTA-UI: Up to date (v%s)\n", FIRMWARE_VERSION);
    return info;
  }

  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String name = asset["name"] | "";
    if (name == OTA_FIRMWARE_ASSET_NAME) {
      info.firmwareUrl = asset["browser_download_url"] | "";
      break;
    }
  }

  if (info.firmwareUrl.length()) {
    info.available = true;
    Serial.printf("OTA-UI: Update available v%s -> v%s\n", FIRMWARE_VERSION, tag.c_str());
  } else {
    Serial.printf("OTA-UI: Release v%s has no '%s' asset\n", tag.c_str(), OTA_FIRMWARE_ASSET_NAME);
  }
  return info;
}

bool OtaUpdaterUI::applyFirmwareFromUrl(const String& url) {
  if (url.isEmpty()) return false;
  Serial.println("OTA-UI: Downloading firmware from: " + url);

  HTTPClient http;
  if (!beginHttpGet(http, url, 30000)) return false;
  int len = http.getSize();
  if (len <= 0) {
    Serial.println("OTA-UI: Invalid content length");
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  bool ok = applyFirmwareFromStream(*stream, (size_t)len);
  http.end();
  return ok;
}

bool OtaUpdaterUI::applyFirmwareFromStream(Stream& stream, size_t contentLength) {
  Serial.printf("OTA-UI: Starting firmware update (%u bytes)\n", (unsigned)contentLength);

  if (!Update.begin(contentLength, U_FLASH)) {
    Serial.printf("OTA-UI: Update.begin failed: %s\n", Update.errorString());
    return false;
  }

  size_t written = Update.writeStream(stream);
  if (written != contentLength) {
    Serial.printf("OTA-UI: writeStream short: %u/%u\n", (unsigned)written, (unsigned)contentLength);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("OTA-UI: Update.end failed: %s\n", Update.errorString());
    return false;
  }
  Serial.println("OTA-UI: Firmware update successful — rebooting");
  delay(500);
  ESP.restart();
  return true;
}

void OtaUpdaterUI::autoUpdate(OtaUpdateInfoUI& info, bool apply) {
  info = checkForUpdate();
  if (info.available && apply) applyFirmwareFromUrl(info.firmwareUrl);
}

#endif // !SIMULATOR
