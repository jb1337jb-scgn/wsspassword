#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

const char* WIFI_SSID = "internet";
const char* WIFI_PASS = "internet";

WebServer server(80);
WebSocketsClient ws;
Preferences prefs;

String chargeboxId = "SIM_ESP32S3_001";
String backendUrl = "wss://demo.ocpp.cc/83d29edd7b79881259e1759ed19ea569";
String backendPassword = "12345678";
bool useWss = true;
String idTag = "CAFFEE";

bool wsConnected = false;
bool ocppAccepted = false;
unsigned long heartbeatIntervalSec = 60;
unsigned long lastHeartbeatMs = 0;
unsigned long lastMeterMs = 0;
unsigned long lastWsReconnectMs = 0;
const char* ADMIN_PASSWORD = "hardware";
String ocppLog[40];
int ocppLogPos = 0;
int ocppLogCount = 0;
String chargeProfile = "manual";
String adminFaultCode = "OtherError";
bool demoRunning = false;
int demoStep = 0;
unsigned long demoLastMs = 0;
String storyText = "Ready for mission.";


int msgCounter = 1;
int transactionId = -1;
float powerKw = 11.0;
float meterWh = 0.0;
float sessionWh = 0.0;
bool plugged = false;
bool authorized = false;
bool faulted = false;
String state = "Available";
String lastEvent = "Boot";
String faultReason = "";

String isoTimestamp();

void addOcppLog(const String &line) {
  String entry = isoTimestamp() + " " + line;
  ocppLog[ocppLogPos] = entry;
  ocppLogPos = (ocppLogPos + 1) % 40;
  if (ocppLogCount < 40) ocppLogCount++;
}

bool adminOk() {
  if (!server.hasArg("pw")) return false;
  return server.arg("pw") == ADMIN_PASSWORD;
}

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 0;
const int DAYLIGHT_OFFSET_SEC = 0;

String isoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return "1970-01-01T00:00:00Z";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

bool ntpSynced() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return false;
  return timeinfo.tm_year >= 120;
}

const int LED_AVAILABLE = 16;
const int LED_PREPARING = 17;
const int LED_CHARGING  = 25;
const int LED_FAULTED   = 33;

// =============================
// Hardware-Eingaenge active LOW
// Schalter/Taster jeweils zwischen GPIO und GND
// =============================
const int PIN_IN_PLUG    = 4;   // rastender Schalter empfohlen
const int PIN_IN_AUTH    = 5;   // Taster
const int PIN_IN_START   = 6;   // Taster
const int PIN_IN_STOP    = 7;   // Taster
const int PIN_IN_FAULT   = 15;  // Schalter oder Taster
const int PIN_IN_RESET   = 18;  // Taster
const int PIN_IN_CONNECT = 8;   // Taster
const int PIN_POWER_POT  = 1;   // optional ADC: Poti 3V3 - GPIO - GND

// Ladefreigabe-Ausgang: HIGH nur bei Status Charging
const int PIN_OUT_CHARGE_ENABLE = 21;

// Eingebaute RGB-LED vieler ESP32-S3-WROOM-1 Devboards, haeufig WS2812 auf GPIO 48
const int PIN_RGB_LED = 48;
const int RGB_LED_COUNT = 1;
Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}

bool lastAuth = HIGH;
bool lastStart = HIGH;
bool lastStop = HIGH;
bool lastFault = HIGH;
bool lastReset = HIGH;
bool lastConnect = HIGH;
bool lastPlugPhysical = HIGH;
unsigned long lastInputMs = 0;
unsigned long lastPowerPotMs = 0;

String uid() { return String(msgCounter++); }

void setState(String s);
void sendBootNotification();
void sendHeartbeat();
void sendStatusNotification(const String &status, const String &errorCode = "NoError");
void sendAuthorize();
void sendStartTransaction();
void sendStopTransaction(const String &reason = "Local");
void sendMeterValues();
void connectOcpp();

void loadConfig() {
  prefs.begin("backend", true);
  chargeboxId = prefs.getString("chargeboxId", "SIM_ESP32S3_001");
  backendUrl = prefs.getString("backendUrl", "wss://demo.ocpp.cc/83d29edd7b79881259e1759ed19ea569");
  backendPassword = prefs.getString("password", "12345678");
  useWss = prefs.getBool("useWss", true);
  idTag = prefs.getString("idTag", "CAFFEE");
  prefs.end();
}

void saveConfig(String cbid, String url, String pass, bool wss, String tag) {
  prefs.begin("backend", false);
  prefs.putString("chargeboxId", cbid);
  prefs.putString("backendUrl", url);
  prefs.putString("password", pass);
  prefs.putBool("useWss", wss);
  prefs.putString("idTag", tag);
  prefs.end();
  chargeboxId = cbid; backendUrl = url; backendPassword = pass; useWss = wss; idTag = tag;
}

String fullOcppUrl() {
  String base = backendUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + "/" + chargeboxId;
}

void parseUrl(const String &url, String &host, uint16_t &port, String &path, bool &secure) {
  String u = url;
  secure = u.startsWith("wss://");
  u.replace("wss://", "");
  u.replace("ws://", "");
  int slash = u.indexOf('/');
  String hostport = slash >= 0 ? u.substring(0, slash) : u;
  path = slash >= 0 ? u.substring(slash) : "/";
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    host = hostport.substring(0, colon);
    port = hostport.substring(colon + 1).toInt();
  } else {
    host = hostport;
    port = secure ? 443 : 80;
  }
}

void wsSendCall(const String &action, JsonDocument &payload) {
  String id = uid();
  String p; serializeJson(payload, p);
  String msg = "[2,\"" + id + "\",\"" + action + "\"," + p + "]";
  Serial.println("OCPP TX: " + msg);
  ws.sendTXT(msg);
}

void wsSendCallResult(const String &id, JsonDocument &payload) {
  String p; serializeJson(payload, p);
  String msg = "[3,\"" + id + "\"," + p + "]";
  Serial.println("OCPP TX: " + msg);
  ws.sendTXT(msg);
}

void sendBootNotification() {
  storyText = "The charge point says hello to the backend.";
  StaticJsonDocument<256> doc;
  doc["chargePointVendor"] = "chargecloud-sim";
  doc["chargePointModel"] = "ESP32-S3-WebSim";
  doc["chargePointSerialNumber"] = chargeboxId;
  doc["firmwareVersion"] = "0.1.0";
  wsSendCall("BootNotification", doc);
  lastEvent = "BootNotification sent";
}

void sendHeartbeat() {
  StaticJsonDocument<8> doc;
  wsSendCall("Heartbeat", doc);
  lastHeartbeatMs = millis();
  lastEvent = "Heartbeat sent";
}

void sendStatusNotification(const String &status, const String &errorCode) {
  StaticJsonDocument<256> doc;
  doc["connectorId"] = 1;
  doc["errorCode"] = errorCode;
  doc["status"] = status;
  wsSendCall("StatusNotification", doc);
  lastEvent = "Status " + status;
}

void sendAuthorize() {
  storyText = "The driver identifies with idTag " + idTag + ".";
  StaticJsonDocument<128> doc;
  doc["idTag"] = idTag;
  wsSendCall("Authorize", doc);
  lastEvent = "Authorize sent";
}

void sendStartTransaction() {
  storyText = "The backend session starts. Energy can flow now.";
  StaticJsonDocument<256> doc;
  doc["connectorId"] = 1;
  doc["idTag"] = idTag;
  doc["meterStart"] = (int)meterWh;
  doc["timestamp"] = isoTimestamp();
  wsSendCall("StartTransaction", doc);
  lastEvent = "StartTransaction sent";
}

void sendStopTransaction(const String &reason) {
  storyText = "The charging session stops cleanly.";
  if (transactionId < 0) return;
  StaticJsonDocument<256> doc;
  doc["transactionId"] = transactionId;
  doc["idTag"] = idTag;
  doc["meterStop"] = (int)meterWh;
  doc["timestamp"] = isoTimestamp();
  doc["reason"] = reason;
  wsSendCall("StopTransaction", doc);
  lastEvent = "StopTransaction sent";
}

void sendMeterValues() {
  storyText = "MeterValues report live energy and power to the backend.";
  if (transactionId < 0 || state != "Charging") return;
  StaticJsonDocument<512> doc;
  doc["connectorId"] = 1;
  doc["transactionId"] = transactionId;
  JsonArray mv = doc.createNestedArray("meterValue");
  JsonObject v = mv.createNestedObject();
  v["timestamp"] = isoTimestamp();
  JsonArray sampled = v.createNestedArray("sampledValue");
  JsonObject e = sampled.createNestedObject();
  e["value"] = String((int)meterWh);
  e["context"] = "Sample.Periodic";
  e["measurand"] = "Energy.Active.Import.Register";
  e["unit"] = "Wh";
  JsonObject p = sampled.createNestedObject();
  p["value"] = String(powerKw, 1);
  p["context"] = "Sample.Periodic";
  p["measurand"] = "Power.Active.Import";
  p["unit"] = "kW";
  wsSendCall("MeterValues", doc);
  lastMeterMs = millis();
}

void setState(String s) {
  state = s;
  digitalWrite(LED_AVAILABLE, state == "Available");
  digitalWrite(LED_PREPARING, state == "Preparing" || state == "SuspendedEVSE");
  digitalWrite(LED_CHARGING, state == "Charging");
  digitalWrite(LED_FAULTED, state == "Faulted");
  digitalWrite(PIN_OUT_CHARGE_ENABLE, state == "Charging" ? HIGH : LOW);

  if (!wsConnected) {
    setRgb(80, 0, 80);       // violett: OCPP nicht verbunden
  } else if (state == "Available") {
    setRgb(0, 80, 0);        // gruen
  } else if (state == "Preparing") {
    setRgb(100, 70, 0);      // gelb
  } else if (state == "Charging") {
    setRgb(0, 0, 120);       // blau
  } else if (state == "SuspendedEVSE") {
    setRgb(120, 50, 0);      // orange
  } else if (state == "Faulted") {
    setRgb(120, 0, 0);       // rot
  } else {
    setRgb(30, 30, 30);      // weiss/grau
  }
}

void applyChargeProfile() {
  static unsigned long profileStart = millis();
  static unsigned long lastRnd = 0;
  static float rndPower = 11.0;
  unsigned long now = millis();
  if (chargeProfile == "manual") return;
  if (chargeProfile == "constant11") powerKw = 11.0;
  else if (chargeProfile == "ramp") {
    float t = (now - profileStart) / 1000.0;
    powerKw = min(22.0f, t * 0.4f);
  } else if (chargeProfile == "solar") {
    float t = (now / 1000.0);
    powerKw = 6.0 + 5.0 * (sin(t / 8.0) + 1.0);
  } else if (chargeProfile == "random") {
    if (now - lastRnd > 3000) { rndPower = random(20, 220) / 10.0; lastRnd = now; }
    powerKw = rndPower;
  } else if (chargeProfile == "pause0") powerKw = 0.0;
}

void updateMeter() {
  static unsigned long last = millis();
  unsigned long now = millis();
  float h = (now - last) / 3600000.0;
  last = now;
  if (state == "Charging") {
    float delta = powerKw * 1000.0 * h;
    meterWh += delta;
    sessionWh += delta;
  }
}

void handleOcppMessage(const String &msg) {
  Serial.println("OCPP RX: " + msg);
  StaticJsonDocument<2048> doc;
  auto err = deserializeJson(doc, msg);
  if (err || !doc.is<JsonArray>()) return;
  int type = doc[0];
  String id = doc[1] | "";
  if (type == 3) {
    JsonObject payload = doc[2].as<JsonObject>();
    if (payload.containsKey("status") && String(payload["status"] | "") == "Accepted") {
      ocppAccepted = true;
      lastEvent = "Accepted";
      sendStatusNotification(state);
    }
    if (payload.containsKey("interval")) heartbeatIntervalSec = payload["interval"];
    if (payload.containsKey("transactionId")) {
      transactionId = payload["transactionId"];
      setState("Charging");
      sendStatusNotification("Charging");
      lastEvent = "Transaction started";
    }
  } else if (type == 2) {
    String action = doc[2] | "";
    StaticJsonDocument<256> res;
    if (action == "RemoteStartTransaction") {
      JsonObject payload = doc[3].as<JsonObject>();
      if (payload.containsKey("idTag")) idTag = String((const char*)payload["idTag"]);
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
      plugged = true; authorized = true; setState("Preparing"); sendStartTransaction();
    } else if (action == "RemoteStopTransaction") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
      sendStopTransaction("Remote");
      transactionId = -1; authorized = false; setState(plugged ? "Preparing" : "Available");
      sendStatusNotification(state);
    } else if (action == "Reset") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
    } else if (action == "UnlockConnector") {
      res["status"] = "Unlocked";
      wsSendCallResult(id, res);
    } else if (action == "ChangeAvailability") {
      res["status"] = "Accepted";
      wsSendCallResult(id, res);
    } else {
      wsSendCallResult(id, res);
    }
  }
}

void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    wsConnected = true; ocppAccepted = false; lastEvent = "WebSocket connected"; sendBootNotification();
  } else if (type == WStype_DISCONNECTED) {
    wsConnected = false; ocppAccepted = false; lastEvent = "WebSocket disconnected";
  } else if (type == WStype_TEXT) {
    handleOcppMessage(String((char*)payload));
  }
}

void connectOcpp() {
  String url = fullOcppUrl();
  String host, path; uint16_t port; bool secure;
  parseUrl(url, host, port, path, secure);
  ws.disconnect();
  ws.setReconnectInterval(5000);
  ws.onEvent(wsEvent);
  if (backendPassword.length() > 0) {
    String auth = base64::encode(chargeboxId + ":" + backendPassword);
    String authHeader = "Basic " + auth;
    ws.setAuthorization(authHeader.c_str());
  }
  ws.setExtraHeaders("Sec-WebSocket-Protocol: ocpp1.6\r\n");
  if (secure) {
    ws.beginSSL(host.c_str(), port, path.c_str(), "");
  } else {
    ws.begin(host.c_str(), port, path.c_str());
  }
  lastEvent = "Connecting OCPP " + url;
}

String htmlPage() {
  return R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-S3 Wallbox</title><style>body{font-family:Arial,sans-serif;background:#eef3f7;margin:0;padding:24px;color:#102033}.wrap{max-width:980px;margin:auto}.card{background:#fff;border-radius:18px;padding:20px;margin-bottom:16px;box-shadow:0 10px 30px #0001}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}.metric{background:#f6f9fc;border-radius:12px;padding:12px}.label{font-size:12px;color:#607084}.value{font-size:22px;font-weight:700}.ok{color:#0a8f5a}.bad{color:#c62828}pre{background:#0b1622;color:#d8eaff;border-radius:14px;padding:14px;overflow:auto;max-height:280px}.top{display:flex;justify-content:space-between;gap:20px;align-items:flex-start}button,a.btn{display:inline-block;border:0;border-radius:12px;padding:12px 14px;font-weight:700;background:#102033;color:white;text-decoration:none}</style></head><body><div class="wrap"><div class="card top"><div><h1>ESP32-S3 Wallbox Simulator</h1><p>Public dashboard: QR code, status and live OCPP log.</p><a class="btn" href="/admin">Admin Panel</a></div><div><img id="qr" width="150" height="150"><div class="label">Scan to open this interface</div></div></div><div class="card"><h2>Status</h2><div class="grid" id="status"></div></div><div class="card"><h2>Live OCPP Log</h2><pre id="log">...</pre></div></div><script>function yn(v){return v?'Yes':'No'}async function refresh(){let s=await(await fetch('/api/state')).json();let url='http://'+location.host;qr.src='https://api.qrserver.com/v1/create-qr-code/?size=150x150&data='+encodeURIComponent(url);let items=[['State',s.state],['WiFi','Yes'],['OCPP WS',yn(s.ocpp.wsConnected)],['Boot accepted',yn(s.ocpp.accepted)],['Plugged',yn(s.plugged)],['Transaction',s.transactionId],['Power',s.powerKw.toFixed(1)+' kW'],['Charge Enable',yn(s.gpio.chargeEnableOut)]];status.innerHTML=items.map(i=>'<div class="metric"><div class="label">'+i[0]+'</div><div class="value">'+i[1]+'</div></div>').join('');log.textContent=(s.ocppLog||[]).join('\n')}setInterval(refresh,1000);refresh();</script></body></html>)HTML";
}

String adminPage() {
  return R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>chargecloud Lab Admin</title><style>:root{--bg:#07111f;--card:#0d1b2f;--card2:#102033;--line:#24405f;--blue:#26d7ff;--green:#10b981;--red:#ef4444;--yellow:#f59e0b;--txt:#e7f0f7;--muted:#91a8bd}*{box-sizing:border-box}body{font-family:Inter,Arial,sans-serif;background:radial-gradient(circle at 10% 10%,#123b5a 0,#07111f 38%,#050914 100%);margin:0;padding:24px;color:var(--txt)}.wrap{max-width:1220px;margin:auto}.hero{display:flex;justify-content:space-between;gap:18px;align-items:flex-start}.brand{font-size:24px;font-weight:900}.sub{color:var(--muted)}.card{background:linear-gradient(180deg,#102033ee,#0b1626ee);border:1px solid #2b4868;border-radius:22px;padding:20px;margin-bottom:16px;box-shadow:0 18px 60px #0008}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:14px}.btn{border:1px solid #39617f;border-radius:14px;padding:12px 16px;font-weight:900;background:#102033;color:white;cursor:pointer;margin:4px;box-shadow:inset 0 0 0 1px #ffffff0d}.btn:hover{border-color:var(--blue);box-shadow:0 0 22px #26d7ff33}.primary{background:linear-gradient(135deg,#137cbd,#26d7ff);color:#03111f}.danger{background:linear-gradient(135deg,#991b1b,#ef4444)}.ghost{background:#ffffff10}.metric,.lamp,.msg{background:#07111f;border:1px solid #1d3855;border-radius:16px;padding:14px}.label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}.value{font-size:22px;font-weight:900}.dot{display:inline-block;width:13px;height:13px;border-radius:50%;margin-right:8px;background:#7f1d1d;box-shadow:0 0 14px #ef444455}.on{background:var(--green);box-shadow:0 0 18px #10b981aa}input,select{background:#07111f;color:var(--txt);border:1px solid #31506e;border-radius:12px;padding:11px;min-width:210px}pre{background:#020812;color:#c7f9ff;border:1px solid #1d3855;border-radius:16px;padding:14px;overflow:auto;max-height:260px}.timeline{display:grid;grid-template-columns:repeat(7,1fr);gap:8px}.step{padding:12px;border-radius:14px;background:#07111f;border:1px solid #1d3855;text-align:center;color:var(--muted);font-size:12px}.step.active{border-color:var(--blue);color:white;box-shadow:0 0 22px #26d7ff33}.story{font-size:22px;line-height:1.35;color:white}.messageList{display:grid;gap:8px;max-height:260px;overflow:auto}.msg{cursor:pointer}.msg:hover{border-color:var(--blue)}.split{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:850px){.split,.hero{grid-template-columns:1fr;display:block}.timeline{grid-template-columns:1fr 1fr}}</style><script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.6/dist/chart.umd.min.js"></script></head><body><div class="wrap"><div class="card hero"><div><div class="brand">chargecloud // Lab Admin</div><div class="sub">technical UI for OCPP demo replay, message inspection and charging story mode</div></div><div><input id="pw" type="password" placeholder="admin password"><button class="btn primary" onclick="refresh()">Unlock</button></div></div><div class="card"><h2>OCPP / WSS Credentials</h2><p class="sub">Default WSS password is <b>12345678</b>. Change it here; it is saved on the ESP32.</p><input id="wssPw" type="password" placeholder="WSS password"><button class="btn primary" onclick="saveWssPassword()">Save WSS Password</button><span id="cfgMsg" class="sub"></span></div><div class="card"><h2>One-Click Demo Replay</h2><button class="btn primary" onclick="demo()">▶ Run Full Demo</button><button class="btn ghost" onclick="action('reset')">Reset</button><div class="timeline" id="timeline"></div><p class="story" id="story">Ready for mission.</p></div><div class="split"><div class="card"><h2>Controls</h2><button class="btn" onclick="action('connect')">Connect OCPP</button><button class="btn" onclick="action('plug')">Plug</button><button class="btn" onclick="action('authorize')">Auth</button><button class="btn primary" onclick="action('start')">Start</button><button class="btn ghost" onclick="action('stop')">Stop</button><button class="btn ghost" onclick="action('unplug')">Unplug</button><h3>Fault Simulator</h3><select id="faultCode"><option>EmergencyStop</option><option>GroundFailure</option><option>PowerMeterFailure</option><option>ConnectorLockFailure</option><option>OverVoltage</option><option>OtherError</option></select><button class="btn danger" onclick="setFault()">Trigger Fault</button><h3>Charging Profile</h3><select id="profile"><option value="manual">Manual / potentiometer</option><option value="constant11">Constant 11 kW</option><option value="ramp">Slow ramp-up</option><option value="solar">Solar surplus curve</option><option value="random">Random fluctuation</option><option value="pause0">0 kW pause</option></select><button class="btn" onclick="setProfile()">Apply</button></div><div class="card"><h2>Live Chart</h2><canvas id="chart" height="145"></canvas></div></div><div class="card"><h2>Diagnosis Lights</h2><div class="grid" id="lamps"></div></div><div class="split"><div class="card"><h2>OCPP Message Inspector</h2><div class="messageList" id="messages"></div></div><div class="card"><h2>Selected Message</h2><pre id="detail">Click a message in the inspector.</pre><h2>Why not charging?</h2><pre id="why">...</pre></div></div><div class="card"><h2>Raw Live OCPP Log</h2><pre id="log">...</pre></div></div><script>let chart=new Chart(document.getElementById('chart'),{type:'line',data:{labels:[],datasets:[{label:'Power kW',data:[],borderColor:'#26d7ff',backgroundColor:'#26d7ff22'},{label:'Energy kWh',data:[],borderColor:'#10b981',backgroundColor:'#10b98122'}]},options:{animation:false,plugins:{legend:{labels:{color:'#e7f0f7'}}},scales:{x:{ticks:{color:'#91a8bd'},grid:{color:'#1d3855'}},y:{beginAtZero:true,ticks:{color:'#91a8bd'},grid:{color:'#1d3855'}}}}});const steps=['Connect','Boot','Plug','Auth','Start','Meter','Stop'];function pass(){return encodeURIComponent(pw.value)}async function action(c){await fetch('/api/action?cmd='+c);refresh()}async function demo(){await fetch('/api/admin/demo?pw='+pass());refresh()}async function setFault(){await fetch('/api/admin/fault?pw='+pass()+'&code='+encodeURIComponent(faultCode.value));refresh()}async function setProfile(){await fetch('/api/admin/profile?pw='+pass()+'&profile='+encodeURIComponent(profile.value));refresh()}async function saveWssPassword(){if(!pw.value){cfgMsg.textContent=' Enter admin password first.';return}await fetch('/api/admin/ocpp-config?pw='+pass()+'&password='+encodeURIComponent(wssPw.value));cfgMsg.textContent=' Saved. Reconnect OCPP or restart if already connected.';wssPw.value='';refresh()}function lamp(n,on){return '<div class="lamp"><span class="dot '+(on?'on':'')+'"></span><b>'+n+'</b></div>'}function inspect(m){detail.textContent=m}let refreshBusy=false;function userIsTyping(){const e=document.activeElement;return e&&(e.tagName==='INPUT'||e.tagName==='SELECT'||e.tagName==='TEXTAREA')}async function refresh(){if(refreshBusy||userIsTyping())return;refreshBusy=true;try{let s=await(await fetch('/api/state')).json();if(pw.value){let a=await(await fetch('/api/admin/state?pw='+pass())).json();if(a.error){why.textContent='Wrong admin password';return}s=a}let t=new Date().toLocaleTimeString();chart.data.labels.push(t);chart.data.datasets[0].data.push(s.powerKw);chart.data.datasets[1].data.push(s.sessionWh/1000);if(chart.data.labels.length>36){chart.data.labels.shift();chart.data.datasets.forEach(d=>d.data.shift())}chart.update();timeline.innerHTML=steps.map((x,i)=>'<div class="step '+(i<=(s.demoStep||0)&&s.demoRunning?'active':'')+'">'+(i+1)+'<br><b>'+x+'</b></div>').join('');story.textContent=s.storyText||'Ready.';lamps.innerHTML=lamp('WiFi',true)+lamp('OCPP WS',s.ocpp.wsConnected)+lamp('Boot accepted',s.ocpp.accepted)+lamp('Plug',s.plugged)+lamp('Authorized',s.authorized)+lamp('Transaction',s.transactionId>=0)+lamp('MeterValues',s.lastMeterAgeSec<15)+lamp('Charge Enable',s.gpio.chargeEnableOut);let r=[];if(!s.plugged)r.push('Vehicle is not plugged in.');if(!s.authorized)r.push('Authorization is missing.');if(s.transactionId<0)r.push('Transaction has not started.');if(s.state!=='Charging')r.push('State is not Charging.');if(s.faulted)r.push('Fault is active: '+s.faultReason);why.textContent=r.length?r.join('\n'):'Charging is allowed. Charge Enable should be ON.';let logs=s.ocppLog||[];log.textContent=logs.join('\n');messages.innerHTML=logs.slice().reverse().map((m,i)=>'<div class="msg" onclick="inspect(this.dataset.m)" data-m="'+m.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;')+'"><div class="label">OCPP Frame</div>'+m.substring(0,90)+'</div>').join('')}finally{refreshBusy=false}}setInterval(refresh,1500);refresh();</script></body></html>)HTML";
}

void sendState() {
  StaticJsonDocument<1024> doc;
  doc["state"] = state; doc["plugged"] = plugged; doc["authorized"] = authorized; doc["faulted"] = faulted;
  doc["powerKw"] = powerKw; doc["meterWh"] = meterWh; doc["sessionWh"] = sessionWh; doc["transactionId"] = transactionId; doc["lastEvent"] = lastEvent; doc["faultReason"] = faultReason; doc["faulted"] = faulted; doc["authorized"] = authorized; doc["chargeProfile"] = chargeProfile; doc["lastMeterAgeSec"] = (millis()-lastMeterMs)/1000; doc["demoRunning"] = demoRunning; doc["demoStep"] = demoStep; doc["storyText"] = storyText;
  doc["wssPasswordSet"] = backendPassword.length() > 0; doc["ntpSynced"] = ntpSynced(); doc["time"] = isoTimestamp();
  JsonObject gpio = doc.createNestedObject("gpio"); gpio["plug"] = digitalRead(PIN_IN_PLUG)==LOW; gpio["auth"] = digitalRead(PIN_IN_AUTH)==LOW; gpio["start"] = digitalRead(PIN_IN_START)==LOW; gpio["stop"] = digitalRead(PIN_IN_STOP)==LOW; gpio["fault"] = digitalRead(PIN_IN_FAULT)==LOW; gpio["reset"] = digitalRead(PIN_IN_RESET)==LOW; gpio["connect"] = digitalRead(PIN_IN_CONNECT)==LOW; gpio["powerPotRaw"] = analogRead(PIN_POWER_POT); gpio["chargeEnableOut"] = digitalRead(PIN_OUT_CHARGE_ENABLE)==HIGH; gpio["rgbLedPin"] = PIN_RGB_LED;
  JsonObject b = doc.createNestedObject("backend"); b["chargeboxId"] = chargeboxId; b["backendUrl"] = backendUrl; b["useWss"] = useWss; b["idTag"] = idTag; b["fullUrl"] = fullOcppUrl(); b["passwordSet"] = backendPassword.length() > 0;
  JsonObject o = doc.createNestedObject("ocpp"); o["wsConnected"] = wsConnected; o["accepted"] = ocppAccepted; o["heartbeatIntervalSec"] = heartbeatIntervalSec; JsonArray lg = doc.createNestedArray("ocppLog"); for(int i=0;i<ocppLogCount;i++){ int idx=(ocppLogPos-ocppLogCount+i+40)%40; lg.add(ocppLog[idx]); }
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}

void handleAction() {
  String cmd = server.arg("cmd");
  if (cmd == "connect") connectOcpp();
  else if (cmd == "plug") { plugged = true; setState("Preparing"); sendStatusNotification("Preparing"); }
  else if (cmd == "unplug") { if (transactionId >= 0) sendStopTransaction("EVDisconnected"); transactionId = -1; plugged = false; authorized = false; setState("Available"); sendStatusNotification("Available"); }
  else if (cmd == "authorize") { authorized = true; sendAuthorize(); }
  else if (cmd == "start") { if (plugged) sendStartTransaction(); }
  else if (cmd == "stop") { sendStopTransaction("Local"); transactionId = -1; authorized = false; setState(plugged ? "Preparing" : "Available"); sendStatusNotification(state); }
  else if (cmd == "fault") { faulted = true; faultReason = "Manual fault"; setState("Faulted"); sendStatusNotification("Faulted", "OtherError"); }
  else if (cmd == "reset") { faulted = false; faultReason = ""; transactionId = -1; authorized = false; sessionWh = 0; setState(plugged ? "Preparing" : "Available"); sendStatusNotification(state); }
  server.send(200, "text/plain", "OK");
}

void handlePower(){ if(server.hasArg("value")){ powerKw=server.arg("value").toFloat(); if(powerKw<0)powerKw=0; if(powerKw>350)powerKw=350; if(state=="Charging" && powerKw<=0.1){setState("SuspendedEVSE"); sendStatusNotification("SuspendedEVSE");} else if(state=="SuspendedEVSE" && powerKw>0.1){setState("Charging"); sendStatusNotification("Charging");}} server.send(200,"text/plain","OK"); }

void handleBackend(){
  StaticJsonDocument<512> doc; if(deserializeJson(doc, server.arg("plain"))){server.send(400,"text/plain","Invalid JSON");return;}
  String pass = doc["backendPassword"] | ""; if(pass.length()==0) pass=backendPassword;
  saveConfig(doc["chargeboxId"] | chargeboxId, doc["backendUrl"] | backendUrl, pass, doc["useWss"] | true, doc["idTag"] | idTag);
  lastEvent="Backend config saved"; server.send(200,"text/plain","OK");
}

bool fallingEdgeDebounced(int pin, bool &lastState) {
  bool current = digitalRead(pin);
  bool edge = (lastState == HIGH && current == LOW);
  lastState = current;
  return edge;
}

void setupHardwareInputs() {
  pinMode(PIN_IN_PLUG, INPUT_PULLUP);
  pinMode(PIN_IN_AUTH, INPUT_PULLUP);
  pinMode(PIN_IN_START, INPUT_PULLUP);
  pinMode(PIN_IN_STOP, INPUT_PULLUP);
  pinMode(PIN_IN_FAULT, INPUT_PULLUP);
  pinMode(PIN_IN_RESET, INPUT_PULLUP);
  pinMode(PIN_IN_CONNECT, INPUT_PULLUP);
  analogReadResolution(12);
  lastPlugPhysical = digitalRead(PIN_IN_PLUG);
}

void handlePlugSwitch() {
  bool current = digitalRead(PIN_IN_PLUG);
  bool currentPlugged = current == LOW;
  if (current != lastPlugPhysical) {
    lastPlugPhysical = current;
    plugged = currentPlugged;
    if (plugged) {
      setState("Preparing");
      sendStatusNotification("Preparing");
      lastEvent = "Hardware plug detected";
    } else {
      if (transactionId >= 0) sendStopTransaction("EVDisconnected");
      transactionId = -1;
      authorized = false;
      setState("Available");
      sendStatusNotification("Available");
      lastEvent = "Hardware unplug detected";
    }
  }
}

void handlePowerPot() {
  if (millis() - lastPowerPotMs < 500) return;
  lastPowerPotMs = millis();
  int raw = analogRead(PIN_POWER_POT);
  float newPower = round((raw / 4095.0) * 500.0) / 10.0; // 0.0 - 50.0 kW
  if (fabs(newPower - powerKw) >= 0.5) {
    powerKw = newPower;
    if(state=="Charging" && powerKw<=0.1){setState("SuspendedEVSE"); sendStatusNotification("SuspendedEVSE");}
    else if(state=="SuspendedEVSE" && powerKw>0.1){setState("Charging"); sendStatusNotification("Charging");}
  }
}

void handleHardwareInputs() {
  if (millis() - lastInputMs < 40) return;
  lastInputMs = millis();

  handlePlugSwitch();

  if (fallingEdgeDebounced(PIN_IN_CONNECT, lastConnect)) {
    connectOcpp();
    lastEvent = "Hardware OCPP connect";
  }
  if (fallingEdgeDebounced(PIN_IN_AUTH, lastAuth)) {
    authorized = true;
    sendAuthorize();
    lastEvent = "Hardware authorize";
  }
  if (fallingEdgeDebounced(PIN_IN_START, lastStart)) {
    if (plugged) {
      sendStartTransaction();
      lastEvent = "Hardware start";
    }
  }
  if (fallingEdgeDebounced(PIN_IN_STOP, lastStop)) {
    sendStopTransaction("Local");
    transactionId = -1;
    authorized = false;
    setState(plugged ? "Preparing" : "Available");
    sendStatusNotification(state);
    lastEvent = "Hardware stop";
  }
  if (fallingEdgeDebounced(PIN_IN_FAULT, lastFault)) {
    faulted = true;
    faultReason = "Hardware fault input";
    setState("Faulted");
    sendStatusNotification("Faulted", "OtherError");
    lastEvent = "Hardware fault";
  }
  if (fallingEdgeDebounced(PIN_IN_RESET, lastReset)) {
    faulted = false;
    faultReason = "";
    transactionId = -1;
    authorized = false;
    sessionWh = 0;
    setState(plugged ? "Preparing" : "Available");
    sendStatusNotification(state);
    lastEvent = "Hardware reset";
  }

  handlePowerPot();
}

void startDemoReplay() {
  demoRunning = true; demoStep = 0; demoLastMs = 0;
  addOcppLog("DEMO replay started");
  storyText = "Demo mode: watch a full charging story unfold.";
}

void handleDemoReplay() {
  if (!demoRunning) return;
  unsigned long now = millis();
  if (demoLastMs != 0 && now - demoLastMs < 2500) return;
  demoLastMs = now;
  switch (demoStep) {
    case 0: connectOcpp(); storyText = "Step 1: Connect to the OCPP backend."; break;
    case 1: plugged = true; setState("Preparing"); sendStatusNotification("Preparing"); storyText = "Step 2: The car is plugged in. Status becomes Preparing."; break;
    case 2: authorized = true; sendAuthorize(); storyText = "Step 3: The driver is authorized."; break;
    case 3: sendStartTransaction(); storyText = "Step 4: Charging starts. Charge Enable turns on."; break;
    case 4: powerKw = 11.0; sendMeterValues(); storyText = "Step 5: MeterValues are sent to the backend."; break;
    case 5: sendStopTransaction("Local"); transactionId = -1; authorized = false; setState("Preparing"); sendStatusNotification(state); storyText = "Step 6: StopTransaction ends the session."; break;
    case 6: plugged = false; setState("Available"); sendStatusNotification("Available"); storyText = "Step 7: Unplug. The charge point is Available again."; demoRunning = false; addOcppLog("DEMO replay finished"); break;
  }
  demoStep++;
}

void sendAdminState() {
  if (!adminOk()) { server.send(403,"application/json","{\"error\":\"forbidden\"}"); return; }
  sendState();
}

void handleAdminFault() {
  if (!adminOk()) { server.send(403,"text/plain","Forbidden"); return; }
  String code = server.hasArg("code") ? server.arg("code") : "OtherError";
  faulted = true; faultReason = code; setState("Faulted"); sendStatusNotification("Faulted", code); addOcppLog("ADMIN fault " + code);
  server.send(200,"text/plain","OK");
}

void handleAdminDemo() {
  if (!adminOk()) { server.send(403,"text/plain","Forbidden"); return; }
  startDemoReplay();
  server.send(200,"text/plain","OK");
}

void handleAdminProfile() {
  if (!adminOk()) { server.send(403,"text/plain","Forbidden"); return; }
  chargeProfile = server.hasArg("profile") ? server.arg("profile") : "manual";
  addOcppLog("ADMIN profile " + chargeProfile);
  server.send(200,"text/plain","OK");
}

void setup(){
  Serial.begin(115200); loadConfig();
  pinMode(LED_AVAILABLE,OUTPUT); pinMode(LED_PREPARING,OUTPUT); pinMode(LED_CHARGING,OUTPUT); pinMode(LED_FAULTED,OUTPUT); pinMode(PIN_OUT_CHARGE_ENABLE, OUTPUT); digitalWrite(PIN_OUT_CHARGE_ENABLE, LOW); rgbLed.begin(); rgbLed.clear(); rgbLed.show(); setupHardwareInputs(); setState("Available");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS); Serial.print("WiFi"); while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");} Serial.println(); Serial.println(WiFi.localIP());
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print("NTP sync");
  for (int i = 0; i < 20 && !ntpSynced(); i++) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.println("Time: " + isoTimestamp());
  server.on("/", [](){server.send(200,"text/html",htmlPage());}); server.on("/admin", [](){server.send(200,"text/html",adminPage());}); server.on("/api/state", sendState); server.on("/api/admin/state", sendAdminState); server.on("/api/admin/fault", handleAdminFault); server.on("/api/admin/profile", handleAdminProfile); server.on("/api/admin/demo", handleAdminDemo); server.on("/api/action", handleAction); server.on("/api/power", handlePower); server.on("/api/backend", HTTP_POST, handleBackend); server.begin();
  connectOcpp();
}

void loop(){
  server.handleClient(); ws.loop(); handleHardwareInputs(); handleDemoReplay(); applyChargeProfile(); updateMeter();
  if(wsConnected && ocppAccepted && millis()-lastHeartbeatMs > heartbeatIntervalSec*1000UL) sendHeartbeat();
  if(wsConnected && state=="Charging" && millis()-lastMeterMs > 10000) sendMeterValues();
}
