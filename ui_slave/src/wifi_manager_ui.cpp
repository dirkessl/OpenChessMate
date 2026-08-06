#ifndef SIMULATOR

#include "wifi_manager_ui.h"
#include "secrets.h"
#include "version.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>

// Embedded single-page web UI. Kept minimal; gzipping/LittleFS is overkill
// for one page on a board that has no other web content.
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><title>OpenChessMate UI</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 body{font:14px system-ui,sans-serif;background:#1a1a1a;color:#eee;margin:0;padding:16px;max-width:640px;margin:auto}
 h1{font-size:18px;margin:0 0 12px}h2{font-size:15px;margin:18px 0 6px;color:#9cf}
 .card{background:#262626;border-radius:8px;padding:12px;margin-bottom:12px}
 label{display:block;margin:6px 0 2px;color:#bbb;font-size:12px}
 input,select,button{font:inherit;width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #444;background:#1a1a1a;color:#eee;margin-bottom:6px}
 button{background:#5d4037;border:0;cursor:pointer}button:hover{background:#795548}
 button.warn{background:#7a3a3a}button.warn:hover{background:#a04848}
 .row{display:flex;gap:8px}.row>*{flex:1}
 .ok{color:#7e7}.bad{color:#e77}.muted{color:#888;font-size:12px}
 progress{width:100%}
</style></head><body>
<h1>OpenChessMate — UI Display</h1>
<div class="card"><h2>Status</h2><div id="st">…</div></div>
<div class="card"><h2>WiFi</h2>
 <label>SSID <button onclick="scan()" style="width:auto;padding:2px 8px;float:right">Scan</button></label>
 <select id="ssid"><option value="">(scan first)</option></select>
 <label>Password</label><input id="pass" type="password">
 <button onclick="save()">Save &amp; Connect</button>
</div>
<div class="card"><h2>Firmware (OTA)</h2>
 <div id="ota">…</div>
 <div class="row">
  <button onclick="check()">Check for update</button>
  <button onclick="apply()">Apply pending</button>
 </div>
 <label><input type="checkbox" id="auto" onchange="setAuto()" style="width:auto;margin-right:6px">Auto-update on boot</label>
 <h2>Manual upload</h2>
 <input id="fw" type="file" accept=".bin">
 <button onclick="upload()">Upload firmware</button>
 <progress id="pg" value="0" max="1" style="display:none"></progress>
 <div id="upmsg" class="muted"></div>
</div>
<script>
async function j(u,o){const r=await fetch(u,o);return r.ok?r.json().catch(()=>({})):Promise.reject(r.status)}
async function refresh(){
 const s=await j('/wifi/status'); const o=await j('/ota/status');
 document.getElementById('st').innerHTML =
  (s.connected?`<span class=ok>Connected</span> to <b>${s.ssid}</b><br>IP ${s.ip} · RSSI ${s.rssi} dBm`
   : s.apMode?`<span class=bad>AP mode</span> · SSID <b>${s.ssid}</b> · IP ${s.ip}`
   : `<span class=bad>Disconnected</span>`)
  + `<br><span class=muted>Firmware v${o.version}</span>`;
 document.getElementById('ota').innerHTML = o.available
   ? `<span class=ok>Update available: v${o.latestVersion}</span>`
   : `<span class=muted>Up to date (v${o.version})</span>`;
 document.getElementById('auto').checked = !!o.autoUpdate;
}
async function scan(){
 const r=await j('/wifi/scan'); const sel=document.getElementById('ssid');
 sel.innerHTML='';for(const n of r.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=`${n.ssid}  (${n.rssi} dBm)${n.secure?' 🔒':''}`;sel.appendChild(o)}
}
async function save(){
 const ssid=document.getElementById('ssid').value, pass=document.getElementById('pass').value;
 if(!ssid){alert('Pick an SSID');return}
 const f=new FormData();f.append('ssid',ssid);f.append('pass',pass);
 await fetch('/wifi/save',{method:'POST',body:f}); alert('Saved — reconnecting in a few seconds');
}
async function check(){await j('/ota/status');await refresh()}
async function apply(){const r=await fetch('/ota/apply',{method:'POST'});alert(await r.text())}
async function setAuto(){const f=new FormData();f.append('autoUpdate',document.getElementById('auto').checked?'1':'0');await fetch('/ota/settings',{method:'POST',body:f})}
function upload(){
 const f=document.getElementById('fw').files[0];if(!f){alert('Pick a .bin');return}
 const x=new XMLHttpRequest(),pg=document.getElementById('pg'),m=document.getElementById('upmsg');
 pg.style.display='block';pg.max=f.size;
 x.upload.onprogress=e=>{pg.value=e.loaded;m.textContent=`${(e.loaded/1024)|0} / ${(f.size/1024)|0} KB`};
 x.onload=()=>{m.textContent=x.responseText||('HTTP '+x.status);pg.style.display='none'};
 x.open('POST','/ota/upload/firmware');x.setRequestHeader('Content-Type','application/octet-stream');x.send(f);
}
refresh();setInterval(refresh,5000);
</script></body></html>
)HTML";

static constexpr uint16_t  AP_PORT       = 80;
static constexpr uint32_t  STA_TIMEOUT   = 10000;   // ms
static constexpr uint32_t  AUTO_OTA_PERIOD = 6UL * 60UL * 60UL * 1000UL; // 6 h

// ---------------------------------------------------------------------------
WiFiManagerUI::WiFiManagerUI() : server(AP_PORT) {}

String WiFiManagerUI::makeApSsid() const {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char buf[24]; snprintf(buf, sizeof(buf), "OpenChessUI-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

void WiFiManagerUI::begin() {
  // Load saved credentials + auto-OTA pref from NVS
  Preferences p;
  String ssid = SECRET_SSID, pass = SECRET_PASS;
  if (p.begin("ui_wifi", true)) {
    ssid = p.getString("ssid", ssid);
    pass = p.getString("pass", pass);
    p.end();
  }
  if (p.begin("ui_ota", true)) {
    autoOta = p.getBool("autoUpdate", false);
    p.end();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (ssid.length()) {
    Serial.printf("WiFi-UI: Connecting to '%s'...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT) {
      delay(200);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    startAp();
  } else {
    Serial.printf("WiFi-UI: Connected, IP %s\n", WiFi.localIP().toString().c_str());
  }

  setupServer();
  server.begin();
  publishStatus();

  // Schedule an immediate auto-OTA check the first time update() runs
  lastOtaCheckMs = 0;
}

void WiFiManagerUI::startAp() {
  WiFi.mode(WIFI_AP);
  String ap = makeApSsid();
  WiFi.softAP(ap.c_str());
  Serial.printf("WiFi-UI: SoftAP '%s' started, IP %s\n",
                ap.c_str(), WiFi.softAPIP().toString().c_str());
}

void WiFiManagerUI::connectSta() {
  Preferences p; p.begin("ui_wifi", true);
  String ssid = p.getString("ssid", SECRET_SSID);
  String pass = p.getString("pass", SECRET_PASS);
  p.end();

  if (!ssid.length()) { startAp(); return; }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT) delay(200);
  if (WiFi.status() != WL_CONNECTED) startAp();
}

void WiFiManagerUI::publishStatus() {
  WiFiStatusUI s;
  if (WiFi.getMode() & WIFI_AP && WiFi.status() != WL_CONNECTED) {
    s.apMode = true;
    s.ssid   = makeApSsid();
    s.ip     = WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    s.connected = true;
    s.ssid      = WiFi.SSID();
    s.ip        = WiFi.localIP().toString();
    s.rssi      = WiFi.RSSI();
  }
  status = s;
  if (statusCb) statusCb(status);
}

void WiFiManagerUI::update() {
  // Apply pending credential save (after the request handler returned)
  if (reconnectPending) {
    reconnectPending = false;
    if (hasStagedCreds) {
      Preferences p; p.begin("ui_wifi", false);
      p.putString("ssid", stagedSsid);
      p.putString("pass", stagedPass);
      p.end();
      hasStagedCreds = false;
      stagedSsid = ""; stagedPass = "";
    }
    connectSta();
    publishStatus();
  }

  // Async scan completion
  if (scanInFlight) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      scanResults.clear();
      for (int i = 0; i < n; ++i) {
        scanResults.push_back({ WiFi.SSID(i), WiFi.RSSI(i),
                                WiFi.encryptionType(i) != WIFI_AUTH_OPEN });
      }
      WiFi.scanDelete();
      scanInFlight = false;
    }
  }

  // Detect connection drops while we thought we were connected
  static bool wasConn = false;
  bool conn = (WiFi.status() == WL_CONNECTED);
  if (conn != wasConn) { wasConn = conn; publishStatus(); }

  // Periodic auto-OTA check
  if (autoOta && conn) {
    unsigned long now = millis();
    if (lastOtaCheckMs == 0 || now - lastOtaCheckMs >= AUTO_OTA_PERIOD) {
      lastOtaCheckMs = now;
      ota.autoUpdate(lastOtaInfo, true);
    }
  }
}

void WiFiManagerUI::requestSaveCredentials(const String& ssid, const String& pass) {
  stagedSsid = ssid; stagedPass = pass;
  hasStagedCreds = true; reconnectPending = true;
}
void WiFiManagerUI::requestReconnect() { reconnectPending = true; }

void WiFiManagerUI::requestScan() {
  if (scanInFlight) return;
  WiFi.scanNetworks(true /*async*/, false /*hidden*/);
  scanInFlight = true;
}

OtaUpdateInfoUI WiFiManagerUI::checkOtaUpdate() {
  lastOtaInfo = ota.checkForUpdate();
  return lastOtaInfo;
}
void WiFiManagerUI::applyOtaUpdate() {
  if (lastOtaInfo.available) ota.applyFirmwareFromUrl(lastOtaInfo.firmwareUrl);
}
void WiFiManagerUI::setAutoOtaEnabled(bool e) {
  autoOta = e;
  Preferences p; p.begin("ui_ota", false); p.putBool("autoUpdate", e); p.end();
}

// ============================ HTTP endpoints ===============================

void WiFiManagerUI::setupServer() {
  server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleRoot(r); });
  server.on("/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });
  server.on("/wifi/scan",   HTTP_GET, [this](AsyncWebServerRequest* r) { handleScan(r); });
  server.on("/wifi/save",   HTTP_POST,[this](AsyncWebServerRequest* r) { handleSave(r); });
  server.on("/ota/status",  HTTP_GET, [this](AsyncWebServerRequest* r) { handleOtaStat(r); });
  server.on("/ota/settings",HTTP_POST,[this](AsyncWebServerRequest* r) { handleOtaSet(r); });
  server.on("/ota/apply",   HTTP_POST,[this](AsyncWebServerRequest* r) { handleOtaApply(r); });
  server.on("/ota/upload/firmware", HTTP_POST,
    [](AsyncWebServerRequest* r) { /* response sent from body callback */ },
    nullptr,
    [this](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
      onFirmwareUploadBody(r, data, len, index, total);
    });
  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "Not Found"); });
}

void WiFiManagerUI::handleRoot(AsyncWebServerRequest* req) {
  AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html", INDEX_HTML);
  req->send(res);
}

void WiFiManagerUI::handleStatus(AsyncWebServerRequest* req) {
  publishStatus();
  JsonDocument d;
  d["connected"] = status.connected;
  d["apMode"]    = status.apMode;
  d["ssid"]      = status.ssid;
  d["ip"]        = status.ip;
  d["rssi"]      = status.rssi;
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

void WiFiManagerUI::handleScan(AsyncWebServerRequest* req) {
  // Trigger a scan if we haven't got one in hand
  if (scanResults.empty() && !scanInFlight) requestScan();
  JsonDocument d;
  JsonArray arr = d["networks"].to<JsonArray>();
  for (auto& e : scanResults) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"]   = e.ssid;
    o["rssi"]   = e.rssi;
    o["secure"] = e.secure;
  }
  d["scanning"] = scanInFlight;
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

void WiFiManagerUI::handleSave(AsyncWebServerRequest* req) {
  if (!req->hasArg("ssid")) { req->send(400, "text/plain", "missing ssid"); return; }
  String ssid = req->arg("ssid");
  String pass = req->hasArg("pass") ? req->arg("pass") : "";
  requestSaveCredentials(ssid, pass);
  req->send(200, "text/plain", "Saved — reconnecting...");
}

void WiFiManagerUI::handleOtaStat(AsyncWebServerRequest* req) {
  // If we never got info (no internet at boot) and we're online now, retry
  if (lastOtaInfo.version.isEmpty() && WiFi.status() == WL_CONNECTED)
    lastOtaInfo = ota.checkForUpdate();
  JsonDocument d;
  d["version"]       = OtaUpdaterUI::getCurrentVersion();
  d["autoUpdate"]    = autoOta;
  d["available"]     = lastOtaInfo.available;
  d["latestVersion"] = lastOtaInfo.version;
  String out; serializeJson(d, out);
  req->send(200, "application/json", out);
}

void WiFiManagerUI::handleOtaSet(AsyncWebServerRequest* req) {
  if (req->hasArg("autoUpdate")) setAutoOtaEnabled(req->arg("autoUpdate") == "1");
  req->send(200, "text/plain", "ok");
}

void WiFiManagerUI::handleOtaApply(AsyncWebServerRequest* req) {
  if (!lastOtaInfo.available) {
    req->send(400, "text/plain", "No update available — check first");
    return;
  }
  req->send(200, "text/plain", "Applying update — device will reboot");
  // Apply on the next loop tick so the response can flush first.
  // Simpler approach: stage via reconnectPending-style flag.
  static auto* pendingInfo = new OtaUpdateInfoUI();
  *pendingInfo = lastOtaInfo;
  // Use a one-shot lambda on a FreeRTOS task — keeps the async server free.
  xTaskCreate(
    [](void* arg) {
      auto* info = static_cast<OtaUpdateInfoUI*>(arg);
      OtaUpdaterUI u;
      u.applyFirmwareFromUrl(info->firmwareUrl);  // reboots on success
      vTaskDelete(nullptr);
    },
    "ota_apply", 8192, pendingInfo, 1, nullptr);
}

void WiFiManagerUI::onFirmwareUploadBody(AsyncWebServerRequest* req,
                                         uint8_t* data, size_t len, size_t index, size_t total) {
  // Async chunk callback — Update.h needs incremental write across chunks.
  if (index == 0) {
    Serial.printf("OTA-UI: Firmware upload start (%u bytes)\n", (unsigned)total);
    if (!Update.begin(total, U_FLASH)) {
      Serial.printf("OTA-UI: Update.begin failed: %s\n", Update.errorString());
      return;
    }
  }
  if (Update.isRunning()) {
    if (Update.write(data, len) != len) {
      Serial.printf("OTA-UI: write failed: %s\n", Update.errorString());
      Update.abort();
    }
  }
  if (index + len == total) {
    if (!Update.isRunning()) {
      req->send(500, "text/plain", "Firmware update failed");
      return;
    }
    if (Update.end(true)) {
      Serial.println("OTA-UI: Upload complete — rebooting");
      req->send(200, "text/plain", "Firmware updated — rebooting");
      delay(500);
      ESP.restart();
    } else {
      req->send(500, "text/plain", String("Finalize failed: ") + Update.errorString());
    }
  }
}

#endif // !SIMULATOR
