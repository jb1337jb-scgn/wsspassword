#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>
#include <Adafruit_NeoPixel.h>

// =============================
// ESP32-S3-N16R8 Wallbox Simulator Basic
// Network: AP + STA
// =============================

const char* WIFI_SSID = "internet";
const char* WIFI_PASS = "internet";
const char* AP_SSID = "Wallbox-Simulator";
const char* AP_PASS = "12345678";

// GPIOs on the selected 3V3-side header
const int PIN_PLUG_SWITCH   = 4;
const int PIN_AUTH_BUTTON   = 5;
const int PIN_START_BUTTON  = 6;
const int PIN_STOP_BUTTON   = 7;
const int PIN_FAULT_SWITCH  = 15;
const int PIN_RESET_BUTTON  = 16;
const int PIN_CONNECT_BTN   = 17;
const int PIN_POWER_POT     = 3;

const int PIN_LED_AVAILABLE = 18;
const int PIN_LED_PREPARING = 8;
const int PIN_LED_CHARGING  = 9;
const int PIN_LED_FAULTED   = 10;
const int PIN_CHARGE_ENABLE = 11;
const int PIN_DEBUG_LED     = 12;
const int PIN_RELAY_SIM     = 13;
const int PIN_BUZZER        = 14;
const int PIN_RESERVED_46   = 46;

// Internal NeoPixel / WS2812 RGB LED
#define USE_INTERNAL_RGB_LED 1
const int PIN_INTERNAL_RGB_LED = 48;
const int INTERNAL_RGB_LED_COUNT = 1;
Adafruit_NeoPixel internalRgb(INTERNAL_RGB_LED_COUNT, PIN_INTERNAL_RGB_LED, NEO_GRB + NEO_KHZ800);

WebServer server(80);
WebSocketsClient ws;
Preferences prefs;

String chargeboxId = "SIM_ESP32S3_001";
String backendUrl = "wss://example.com/ocpp";
String wssPassword = "12345678";
String idTag = "CAFFEE";

bool wsConnected = false;
bool ocppAccepted = false;
bool plugged = false;
bool authorized = false;
bool faulted = false;
bool transactionActive = false;
String state = "Available";
String lastEvent = "Boot";
int msgCounter = 1;
float powerKw = 0.0;
float sessionWh = 0.0;
unsigned long lastMeterMs = 0;

bool lastAuth = HIGH;
bool lastStart = HIGH;
bool lastStop = HIGH;
bool lastReset = HIGH;
bool lastConnect = HIGH;

String ocppLog[24];
int ocppLogPos = 0;
int ocppLogCount = 0;

void addLog(const String& line) {
  ocppLog[ocppLogPos] = line;
  ocppLogPos = (ocppLogPos + 1) % 24;
  if (ocppLogCount < 24) ocppLogCount++;
  Serial.println(line);
}

void setInternalRgb(uint8_t r, uint8_t g, uint8_t b) {
#if USE_INTERNAL_RGB_LED
  internalRgb.setPixelColor(0, internalRgb.Color(r, g, b));
  internalRgb.show();
#endif
}

void updateInternalRgbStatus() {
#if USE_INTERNAL_RGB_LED
  if (wsConnected && ocppAccepted) setInternalRgb(0, 80, 0);      // green = backend accepted
  else if (wsConnected) setInternalRgb(0, 0, 80);                 // blue = websocket connected
  else setInternalRgb(0, 0, 0);                                   // off = no backend
#endif
}

void initPins() {
  pinMode(PIN_PLUG_SWITCH, INPUT_PULLUP);
  pinMode(PIN_AUTH_BUTTON, INPUT_PULLUP);
  pinMode(PIN_START_BUTTON, INPUT_PULLUP);
  pinMode(PIN_STOP_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FAULT_SWITCH, INPUT_PULLUP);
  pinMode(PIN_RESET_BUTTON, INPUT_PULLUP);
  pinMode(PIN_CONNECT_BTN, INPUT_PULLUP);

  pinMode(PIN_LED_AVAILABLE, OUTPUT);
  pinMode(PIN_LED_PREPARING, OUTPUT);
  pinMode(PIN_LED_CHARGING, OUTPUT);
  pinMode(PIN_LED_FAULTED, OUTPUT);
  pinMode(PIN_CHARGE_ENABLE, OUTPUT);
  pinMode(PIN_DEBUG_LED, OUTPUT);
  pinMode(PIN_RELAY_SIM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
}

void updateOutputs() {
  digitalWrite(PIN_LED_AVAILABLE, state == "Available");
  digitalWrite(PIN_LED_PREPARING, state == "Preparing");
  digitalWrite(PIN_LED_CHARGING, state == "Charging");
  digitalWrite(PIN_LED_FAULTED, state == "Faulted");
  digitalWrite(PIN_CHARGE_ENABLE, state == "Charging");
  digitalWrite(PIN_DEBUG_LED, WiFi.status() == WL_CONNECTED);
  digitalWrite(PIN_RELAY_SIM, transactionActive);
  digitalWrite(PIN_BUZZER, faulted);
}

void deriveState() {
  if (faulted) state = "Faulted";
  else if (transactionActive) state = "Charging";
  else if (plugged) state = "Preparing";
  else state = "Available";
}

void loadConfig() {
  prefs.begin("wallbox", false);
  chargeboxId = prefs.getString("cbid", "SIM_ESP32S3_001");
  backendUrl = prefs.getString("url", "wss://example.com/ocpp");
  wssPassword = prefs.getString("wssPw", "12345678");
  idTag = prefs.getString("idTag", "CAFFEE");
}

void saveConfig(String cbid, String url, String pw) {
  chargeboxId = cbid;
  backendUrl = url;
  wssPassword = pw;
  prefs.putString("cbid", chargeboxId);
  prefs.putString("url", backendUrl);
  prefs.putString("wssPw", wssPassword);
}

String fullOcppUrl() {
  String url = backendUrl;
  if (!url.endsWith("/" + chargeboxId)) {
    if (!url.endsWith("/")) url += "/";
    url += chargeboxId;
  }
  return url;
}

bool parseUrl(const String& url, String& host, uint16_t& port, String& path, bool& secure) {
  secure = url.startsWith("wss://");
  bool plain = url.startsWith("ws://");
  if (!secure && !plain) return false;
  int start = secure ? 6 : 5;
  int slash = url.indexOf('/', start);
  String hp = slash >= 0 ? url.substring(start, slash) : url.substring(start);
  path = slash >= 0 ? url.substring(slash) : "/";
  int colon = hp.indexOf(':');
  if (colon >= 0) {
    host = hp.substring(0, colon);
    port = hp.substring(colon + 1).toInt();
  } else {
    host = hp;
    port = secure ? 443 : 80;
  }
  return host.length() > 0;
}

void sendOcpp(const String& action, JsonDocument& payload) {
  if (!wsConnected) return;
  String uid = String(msgCounter++);
  String body;
  JsonDocument arr;
  arr.add(2);
  arr.add(uid);
  arr.add(action);
  arr.add(payload.as<JsonVariant>());
  serializeJson(arr, body);
  ws.sendTXT(body);
  addLog("TX " + body);
}

void sendBootNotification() {
  JsonDocument p;
  p["chargePointVendor"] = "chargecloud";
  p["chargePointModel"] = "ESP32-S3-N16R8 Simulator";
  p["chargePointSerialNumber"] = chargeboxId;
  sendOcpp("BootNotification", p);
}

void sendStatusNotification(const String& st) {
  JsonDocument p;
  p["connectorId"] = 1;
  p["errorCode"] = faulted ? "OtherError" : "NoError";
  p["status"] = st;
  sendOcpp("StatusNotification", p);
}

void sendAuthorize() {
  JsonDocument p;
  p["idTag"] = idTag;
  sendOcpp("Authorize", p);
}

void sendStartTransaction() {
  JsonDocument p;
  p["connectorId"] = 1;
  p["idTag"] = idTag;
  p["meterStart"] = (int)sessionWh;
  p["timestamp"] = "2026-01-01T00:00:00Z";
  sendOcpp("StartTransaction", p);
}

void sendStopTransaction() {
  JsonDocument p;
  p["meterStop"] = (int)sessionWh;
  p["timestamp"] = "2026-01-01T00:00:00Z";
  p["transactionId"] = 1;
  sendOcpp("StopTransaction", p);
}

void sendMeterValues() {
  JsonDocument p;
  p["connectorId"] = 1;
  p["transactionId"] = 1;
  JsonArray mv = p["meterValue"].to<JsonArray>();
  JsonObject item = mv.add<JsonObject>();
  item["timestamp"] = "2026-01-01T00:00:00Z";
  JsonArray sampled = item["sampledValue"].to<JsonArray>();
  JsonObject e = sampled.add<JsonObject>();
  e["value"] = String(sessionWh / 1000.0, 3);
  e["measurand"] = "Energy.Active.Import.Register";
  e["unit"] = "kWh";
  JsonObject pwr = sampled.add<JsonObject>();
  pwr["value"] = String(powerKw, 1);
  pwr["measurand"] = "Power.Active.Import";
  pwr["unit"] = "kW";
  sendOcpp("MeterValues", p);
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    wsConnected = true;
    ocppAccepted = false;
    addLog("WS connected");
    sendBootNotification();
  } else if (type == WStype_DISCONNECTED) {
    wsConnected = false;
    ocppAccepted = false;
    addLog("WS disconnected");
  } else if (type == WStype_TEXT) {
    String msg = String((char*)payload).substring(0, length);
    addLog("RX " + msg);
    if (msg.indexOf("BootNotification") >= 0 || msg.indexOf("Accepted") >= 0) {
      ocppAccepted = true;
      lastEvent = "Boot accepted";
    }
  }
}

void connectOcpp() {
  String url = fullOcppUrl();
  String host, path;
  uint16_t port;
  bool secure;
  if (!parseUrl(url, host, port, path, secure)) {
    lastEvent = "Invalid BackendURL";
    addLog(lastEvent);
    return;
  }
  ws.disconnect();
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(5000);
  if (wssPassword.length() > 0) {
    String auth = base64::encode(chargeboxId + ":" + wssPassword);
    String header = "Authorization: Basic " + auth + "\r\nSec-WebSocket-Protocol: ocpp1.6\r\n";
    ws.setExtraHeaders(header.c_str());
  } else {
    ws.setExtraHeaders("Sec-WebSocket-Protocol: ocpp1.6\r\n");
  }
  if (secure) ws.beginSSL(host.c_str(), port, path.c_str(), "");
  else ws.begin(host.c_str(), port, path.c_str());
  lastEvent = "Connecting " + url;
  addLog(lastEvent);
}

String htmlPage() {
  return R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wallbox Simulator</title><style>body{font-family:Arial,sans-serif;background:#eef3f7;color:#102033;margin:0;padding:20px}.wrap{max-width:980px;margin:auto}.card{background:white;border-radius:18px;padding:18px;margin:14px 0;box-shadow:0 10px 30px #0001}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.metric{background:#f6f9fc;border-radius:12px;padding:12px}.label{font-size:12px;color:#607084}.value{font-size:20px;font-weight:800}input{width:100%;padding:10px;border:1px solid #ccd7e2;border-radius:10px}button{border:0;border-radius:12px;padding:12px 14px;font-weight:800;background:#102033;color:white;margin-top:10px}pre{background:#0b1622;color:#d8eaff;border-radius:14px;padding:14px;overflow:auto;max-height:250px}.ok{color:#0a8f5a}.bad{color:#c62828}</style></head><body><div class="wrap"><h1>ESP32-S3 Wallbox Simulator</h1><div class="card"><h2>Status</h2><div class="grid" id="status"></div></div><div class="card"><h2>OCPP Configuration</h2><label>ChargeboxID</label><input id="cbid"><label>BackendURL</label><input id="url"><label>WSS Password</label><input id="pw" type="password" placeholder="default: 12345678"><button onclick="saveCfg()">Save configuration</button><button onclick="connectOcpp()">Connect OCPP</button><p id="msg"></p></div><div class="card"><h2>Live Log</h2><pre id="log">...</pre></div></div><script>let editing=false;document.addEventListener('focusin',e=>{if(['INPUT','TEXTAREA','SELECT'].includes(e.target.tagName))editing=true});document.addEventListener('focusout',e=>{editing=false});function yn(v){return v?'Yes':'No'}async function load(){let s=await(await fetch('/api/state')).json();let items=[['STA IP',s.net.staIp],['AP IP',s.net.apIp],['WiFi',yn(s.net.wifi)],['State',s.state],['OCPP WS',yn(s.ocpp.wsConnected)],['Boot accepted',yn(s.ocpp.accepted)],['Plugged',yn(s.plugged)],['Power',s.powerKw.toFixed(1)+' kW']];status.innerHTML=items.map(i=>'<div class="metric"><div class="label">'+i[0]+'</div><div class="value">'+i[1]+'</div></div>').join('');log.textContent=(s.log||[]).join('\n');if(!editing){cbid.value=s.config.chargeboxId;url.value=s.config.backendUrl;pw.value=s.config.wssPassword}}async function saveCfg(){await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({chargeboxId:cbid.value,backendUrl:url.value,wssPassword:pw.value})});msg.textContent='Saved. Connect OCPP again to use the new values.';load()}async function connectOcpp(){await fetch('/api/connect',{method:'POST'});msg.textContent='Connecting...';load()}setInterval(load,1500);load();</script></body></html>)HTML";
}

void handleState() {
  JsonDocument doc;
  JsonObject net = doc["net"].to<JsonObject>();
  net["wifi"] = WiFi.status() == WL_CONNECTED;
  net["staIp"] = WiFi.localIP().toString();
  net["apIp"] = WiFi.softAPIP().toString();
  doc["state"] = state;
  doc["plugged"] = plugged;
  doc["authorized"] = authorized;
  doc["transactionActive"] = transactionActive;
  doc["faulted"] = faulted;
  doc["powerKw"] = powerKw;
  JsonObject ocpp = doc["ocpp"].to<JsonObject>();
  ocpp["wsConnected"] = wsConnected;
  ocpp["accepted"] = ocppAccepted;
  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["chargeboxId"] = chargeboxId;
  cfg["backendUrl"] = backendUrl;
  cfg["wssPassword"] = wssPassword;
  JsonArray logs = doc["log"].to<JsonArray>();
  for (int i = 0; i < ocppLogCount; i++) {
    int idx = (ocppLogPos - ocppLogCount + i + 24) % 24;
    logs.add(ocppLog[idx]);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setupWeb() {
  server.on("/", [](){ server.send(200, "text/html", htmlPage()); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_POST, [](){
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    saveConfig(doc["chargeboxId"] | chargeboxId, doc["backendUrl"] | backendUrl, doc["wssPassword"] | wssPassword);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/connect", HTTP_POST, [](){ connectOcpp(); server.send(200, "application/json", "{\"ok\":true}"); });
  server.begin();
}

void setupNetwork() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("AP started: " + String(AP_SSID));
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  Serial.print("Connecting STA to "); Serial.println(WIFI_SSID);
}

void readInputs() {
  plugged = digitalRead(PIN_PLUG_SWITCH) == LOW;
  faulted = digitalRead(PIN_FAULT_SWITCH) == LOW;
  int raw = analogRead(PIN_POWER_POT);
  powerKw = map(raw, 0, 4095, 0, 220) / 10.0;

  bool auth = digitalRead(PIN_AUTH_BUTTON);
  bool start = digitalRead(PIN_START_BUTTON);
  bool stop = digitalRead(PIN_STOP_BUTTON);
  bool reset = digitalRead(PIN_RESET_BUTTON);
  bool connect = digitalRead(PIN_CONNECT_BTN);

  if (lastAuth == HIGH && auth == LOW) { authorized = true; sendAuthorize(); lastEvent = "Authorized"; }
  if (lastStart == HIGH && start == LOW && plugged && authorized && !faulted) { transactionActive = true; sendStartTransaction(); lastEvent = "Start"; }
  if (lastStop == HIGH && stop == LOW) { transactionActive = false; sendStopTransaction(); lastEvent = "Stop"; }
  if (lastReset == HIGH && reset == LOW) { authorized = false; transactionActive = false; faulted = false; sessionWh = 0; lastEvent = "Reset"; }
  if (lastConnect == HIGH && connect == LOW) { connectOcpp(); }

  lastAuth = auth;
  lastStart = start;
  lastStop = stop;
  lastReset = reset;
  lastConnect = connect;
}

void setup() {
  Serial.begin(115200);
#if USE_INTERNAL_RGB_LED
  internalRgb.begin();
  internalRgb.clear();
  internalRgb.show();
#endif
  initPins();
  loadConfig();
  setupNetwork();
  setupWeb();
  addLog("Ready. Webinterface on AP http://192.168.4.1");
}

void loop() {
  server.handleClient();
  ws.loop();
  readInputs();
  deriveState();
  updateOutputs();
  updateInternalRgbStatus();

  if (transactionActive && millis() - lastMeterMs > 10000) {
    float whPer10s = powerKw * 1000.0 / 360.0;
    sessionWh += whPer10s;
    sendMeterValues();
    lastMeterMs = millis();
  }
}
