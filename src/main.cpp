#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

const char* WIFI_SSID = "internet";
const char* WIFI_PASS = "internet";
const char* AP_SSID = "Wallbox-Simulator";
const char* AP_PASS = "12345678";

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
const int PIN_INTERNAL_RGB_LED = 48;

Adafruit_NeoPixel internalRgb(1, PIN_INTERNAL_RGB_LED, NEO_GRB + NEO_KHZ800);
WebServer server(80);
WebSocketsClient ws;
Preferences prefs;

const char ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIID7zCCAtegAwIBAgIBADANBgkqhkiG9w0BAQsFADCBmDELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxJTAjBgNVBAoT
HFN0YXJmaWVsZCBUZWNobm9sb2dpZXMsIEluYy4xOzA5BgNVBAMTMlN0YXJmaWVs
ZCBTZXJ2aWNlcyBSb290IENlcnRpZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5
MDkwMTAwMDAwMFoXDTM3MTIzMTIzNTk1OVowgZgxCzAJBgNVBAYTAlVTMRAwDgYD
VQQIEwdBcml6b25hMRMwEQYDVQQHEwpTY290dHNkYWxlMSUwIwYDVQQKExxTdGFy
ZmllbGQgVGVjaG5vbG9naWVzLCBJbmMuMTswOQYDVQQDEzJTdGFyZmllbGQgU2Vy
dmljZXMgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBANUMOsQq+U7i9b4Zl1+OiFOxHz/Lz58gE20p
OsgPfTz3a3Y4Y9k2YKibXlwAgLIvWX/2h/klQ4bnaRtSmpDhcePYLQ1Ob/bISdm2
8xpWriu2dBTrz/sm4xq6HZYuajtYlIlHVv8loJNwU4PahHQUw2eeBGg6345AWh1K
Ts9DkTvnVtYAcMtS7nt9rjrnvDH5RfbCYM8TWQIrgMw0R9+53pBlbQLPLJGmpufe
hRhJfGZOozptqbXuNC66DQO4M99H67FrjSXZm86B0UVGMpZwh94CDklDhbZsc7tk
6mFBrMnUVN+HL8cisibMn1lUaJ/8viovxFUcdUBgF4UCVTmLfwUCAwEAAaNCMEAw
DwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFJxfAN+q
AdcwKziIorhtSpzyEZGDMA0GCSqGSIb3DQEBCwUAA4IBAQBLNqaEd2ndOxmfZyMI
bw5hyf2E3F/YNoHN2BtBLZ9g3ccaaNnRbobhiCPPE95Dz+I0swSdHynVv/heyNXB
ve6SbzJ08pGCL72CQnqtKrcgfU28elUSwhXqvfdqlS5sdJ/PHLTyxQGjhdByPq1z
qwubdQxtRbeOlKyWN7Wg0I8VRw7j6IPdj/3vQQF3zCepYoUz8jcI73HPdwbeyBkd
iEDPfUYd/x7H4c7/I9vG+o1VTqkC50cRRj70/b17KSa7qWFiNyi2LSr2EIZkyXCn
0q23KXB56jzaYyWf/Wi3MOxw+3WKt21gZ7IeyLnp2KhvAotnDU0mV3HaIPzBSlCN
sSi6
-----END CERTIFICATE-----
)EOF";


String chargeboxId = "SIM_ESP32S3_001";
String backendUrl = "wss://demo.ocpp.cc/83d29edd7b79881259e1759ed19ea569";
String wssPassword = "12345678";
bool basicAuthEnabled = false;
bool appendChargeboxId = true;
String idTag = "CAFFEE";

bool wsConnected = false;
bool ocppAccepted = false;
bool plugged = false;
bool authorized = false;
bool faulted = false;
bool transactionActive = false;
String state = "Available";
String lastReportedStatus = "";
String lastEvent = "Boot";
String connectPhase = "idle";
String connectError = "none";
String lastBootUid = "";
String lastAuthorizeUid = "";
String lastStartUid = "";
String lastStopUid = "";
int msgCounter = 1;
int transactionId = -1;
float powerKw = 0.0;
float sessionWh = 0.0;
unsigned long heartbeatIntervalSec = 300;
unsigned long lastHeartbeatMs = 0;
unsigned long lastMeterMs = 0;

bool lastAuth = HIGH, lastStart = HIGH, lastStop = HIGH, lastReset = HIGH, lastConnect = HIGH, lastPlug = HIGH, lastFault = HIGH;
String ocppLog[40]; int ocppLogPos = 0; int ocppLogCount = 0;

String isoTimestamp(){ struct tm t; if(getLocalTime(&t,50)){ char b[25]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&t); return String(b);} return "2026-01-01T00:00:00Z"; }
void setPhase(const String& ph, const String& err="none"){ connectPhase=ph; connectError=err; }
void addLog(const String& line){ ocppLog[ocppLogPos]=isoTimestamp()+" "+line; ocppLogPos=(ocppLogPos+1)%40; if(ocppLogCount<40)ocppLogCount++; Serial.println(line); }
void rgb(uint8_t r,uint8_t g,uint8_t b){ internalRgb.setPixelColor(0, internalRgb.Color(r,g,b)); internalRgb.show(); }
void updateRgb(){ if(faulted) rgb(80,0,0); else if(wsConnected&&ocppAccepted) rgb(0,80,0); else if(wsConnected) rgb(0,0,80); else rgb(0,0,0); }

String makeUid(){ time_t now=time(nullptr); if(now<100000) now=millis()/1000; return String((unsigned long)now)+"-"+String(random(100000000,999999999))+"-"+String(msgCounter++); }

void initPins(){
  int ins[]={PIN_PLUG_SWITCH,PIN_AUTH_BUTTON,PIN_START_BUTTON,PIN_STOP_BUTTON,PIN_FAULT_SWITCH,PIN_RESET_BUTTON,PIN_CONNECT_BTN};
  for(int p:ins) pinMode(p, INPUT_PULLUP);
  int outs[]={PIN_LED_AVAILABLE,PIN_LED_PREPARING,PIN_LED_CHARGING,PIN_LED_FAULTED,PIN_CHARGE_ENABLE,PIN_DEBUG_LED,PIN_RELAY_SIM,PIN_BUZZER};
  for(int p:outs) pinMode(p, OUTPUT);
}
void deriveState(){ if(faulted) state="Faulted"; else if(transactionActive) state="Charging"; else if(plugged) state="Preparing"; else state="Available"; }
void updateOutputs(){ digitalWrite(PIN_LED_AVAILABLE,state=="Available"); digitalWrite(PIN_LED_PREPARING,state=="Preparing"); digitalWrite(PIN_LED_CHARGING,state=="Charging"); digitalWrite(PIN_LED_FAULTED,state=="Faulted"); digitalWrite(PIN_CHARGE_ENABLE,state=="Charging"); digitalWrite(PIN_DEBUG_LED,WiFi.status()==WL_CONNECTED); digitalWrite(PIN_RELAY_SIM,transactionActive); digitalWrite(PIN_BUZZER,faulted); }

void loadConfig(){ prefs.begin("wallbox",false); chargeboxId=prefs.getString("cbid","SIM_ESP32S3_001"); backendUrl=prefs.getString("url",backendUrl); wssPassword=prefs.getString("wssPw","12345678"); basicAuthEnabled=prefs.getBool("auth",false); appendChargeboxId=prefs.getBool("append",true); }
void saveConfig(JsonDocument& d){ chargeboxId=d["chargeboxId"]|chargeboxId; backendUrl=d["backendUrl"]|backendUrl; wssPassword=d["wssPassword"]|wssPassword; basicAuthEnabled=d["basicAuthEnabled"]|basicAuthEnabled; appendChargeboxId=d["appendChargeboxId"]|appendChargeboxId; prefs.putString("cbid",chargeboxId); prefs.putString("url",backendUrl); prefs.putString("wssPw",wssPassword); prefs.putBool("auth",basicAuthEnabled); prefs.putBool("append",appendChargeboxId); }
String fullOcppUrl(){ String u=backendUrl; if(appendChargeboxId && !u.endsWith("/"+chargeboxId)){ if(!u.endsWith("/")) u+="/"; u+=chargeboxId; } return u; }
bool parseUrl(const String& url,String& host,uint16_t& port,String& path,bool& secure){ secure=url.startsWith("wss://"); bool plain=url.startsWith("ws://"); if(!secure&&!plain)return false; int start=secure?6:5; int slash=url.indexOf('/',start); String hp=slash>=0?url.substring(start,slash):url.substring(start); path=slash>=0?url.substring(slash):"/"; int colon=hp.indexOf(':'); if(colon>=0){host=hp.substring(0,colon); port=hp.substring(colon+1).toInt();} else {host=hp; port=secure?443:80;} return host.length()>0; }

String sendOcpp(const String& action, JsonDocument& payload){ if(!wsConnected) return ""; String uid=makeUid(); JsonDocument arr; arr.add(2); arr.add(uid); arr.add(action); arr.add(payload.as<JsonVariant>()); String body; serializeJson(arr,body); ws.sendTXT(body); addLog("TX "+body); return uid; }
void sendBootNotification(){ JsonDocument p; p["chargePointVendor"]="session."; p["chargePointModel"]="DC_CHARGER-13KW"; p["chargePointSerialNumber"]="Sessioncharge"; p["chargeBoxSerialNumber"]=""; p["firmwareVersion"]="DCCHARGER_CC_V30.64_1218;UULN;DW_T5_L10"; p["iccid"]=""; p["imsi"]=""; p["meterType"]="DC-Meter"; p["meterSerialNumber"]="000000000001"; lastBootUid=sendOcpp("BootNotification",p); }
void sendStatusNotification(const String& st){ JsonDocument p; p["connectorId"]=1; p["errorCode"]=faulted?"OtherError":"NoError"; p["status"]=st; sendOcpp("StatusNotification",p); }
void reportStatusIfChanged(){ if(!ocppAccepted) return; if(state!=lastReportedStatus){ sendStatusNotification(state); lastReportedStatus=state; } }
void sendHeartbeat(){ JsonDocument p; sendOcpp("Heartbeat",p); lastHeartbeatMs=millis(); }
void sendAuthorize(){ JsonDocument p; p["idTag"]=idTag; lastAuthorizeUid=sendOcpp("Authorize",p); }
void sendStartTransaction(){ JsonDocument p; p["connectorId"]=1; p["idTag"]=idTag; p["meterStart"]=(int)sessionWh; p["timestamp"]=isoTimestamp(); lastStartUid=sendOcpp("StartTransaction",p); }
void sendStopTransaction(){ JsonDocument p; p["meterStop"]=(int)sessionWh; p["timestamp"]=isoTimestamp(); p["transactionId"]=transactionId>0?transactionId:1; lastStopUid=sendOcpp("StopTransaction",p); }
void sendMeterValues(){ if(transactionId<0) return; JsonDocument p; p["connectorId"]=1; p["transactionId"]=transactionId; JsonArray mv=p["meterValue"].to<JsonArray>(); JsonObject item=mv.add<JsonObject>(); item["timestamp"]=isoTimestamp(); JsonArray sv=item["sampledValue"].to<JsonArray>(); JsonObject e=sv.add<JsonObject>(); e["value"]=String(sessionWh/1000.0,3); e["measurand"]="Energy.Active.Import.Register"; e["unit"]="kWh"; JsonObject pow=sv.add<JsonObject>(); pow["value"]=String(powerKw,1); pow["measurand"]="Power.Active.Import"; pow["unit"]="kW"; sendOcpp("MeterValues",p); }

void handleCallResult(const String& uid, JsonVariant payload){
  if(uid==lastBootUid){ String status=payload["status"]|""; heartbeatIntervalSec=payload["interval"]|300; ocppAccepted=(status=="Accepted"); if(ocppAccepted){ setPhase("bootAccepted"); lastEvent="Boot accepted"; lastReportedStatus=""; sendStatusNotification(state); lastReportedStatus=state; lastHeartbeatMs=millis(); } }
  else if(uid==lastStartUid){ transactionId=payload["transactionId"]|1; transactionActive=true; lastEvent="Transaction ID "+String(transactionId); }
  else if(uid==lastAuthorizeUid){ authorized=true; lastEvent="Authorized accepted"; }
  else if(uid==lastStopUid){ transactionActive=false; transactionId=-1; lastEvent="Stopped"; }
}

void wsEvent(WStype_t type,uint8_t* payload,size_t length){
  if(type==WStype_CONNECTED){ wsConnected=true; ocppAccepted=false; lastReportedStatus=""; setPhase("wsConnected"); addLog("WS connected"); sendBootNotification(); setPhase("bootSent"); }
  else if(type==WStype_DISCONNECTED){ wsConnected=false; ocppAccepted=false; if(connectPhase!="bootAccepted") setPhase("disconnected","Backend closed connection or TLS/Auth/Subprotocol failed"); addLog("WS disconnected"); }
  else if(type==WStype_TEXT){ String msg=String((char*)payload).substring(0,length); addLog("RX "+msg); JsonDocument d; if(deserializeJson(d,msg)==DeserializationError::Ok){ int mt=d[0]|0; if(mt==3){ String uid=d[1]|""; handleCallResult(uid,d[2]); } } }
}

void connectOcpp(){ String url=fullOcppUrl(); String host,path; uint16_t port; bool secure; if(!parseUrl(url,host,port,path,secure)){ setPhase("urlError","Invalid BackendURL. Use ws:// or wss://"); addLog("Invalid BackendURL"); return; } ws.disconnect(); ws.onEvent(wsEvent); ws.setReconnectInterval(5000); String headers="Sec-WebSocket-Protocol: ocpp1.6\r\n"; if(basicAuthEnabled && wssPassword.length()>0){ headers="Authorization: Basic "+base64::encode(chargeboxId+":"+wssPassword)+"\r\n"+headers; } ws.setExtraHeaders(headers.c_str()); if(secure) ws.beginSSL(host.c_str(),port,path.c_str(),ROOT_CA); else ws.begin(host.c_str(),port,path.c_str()); setPhase("connecting"); addLog("Connecting "+url+" auth="+String(basicAuthEnabled?"on":"off")); }

String htmlPage(){ return R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wallbox Simulator</title><style>body{font-family:Arial,sans-serif;background:#eef3f7;color:#102033;margin:0;padding:20px}.wrap{max-width:1050px;margin:auto}.card{background:white;border-radius:18px;padding:18px;margin:14px 0;box-shadow:0 10px 30px #0001}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.metric{background:#f6f9fc;border-radius:12px;padding:12px}.label{font-size:12px;color:#607084}.value{font-size:18px;font-weight:800;overflow-wrap:anywhere}input{width:100%;padding:10px;border:1px solid #ccd7e2;border-radius:10px;margin:4px 0 10px}button{border:0;border-radius:12px;padding:12px 14px;font-weight:800;background:#102033;color:white;margin:6px 6px 0 0}.row{display:flex;gap:16px;flex-wrap:wrap}.check{display:flex;align-items:center;gap:8px}.leds{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.led{display:flex;align-items:center;gap:10px;background:#f6f9fc;border-radius:12px;padding:12px;font-weight:800}.dot{width:18px;height:18px;border-radius:50%;background:#cbd5e1;box-shadow:inset 0 0 0 2px #94a3b8}.dot.on{background:#10b981;box-shadow:0 0 16px #10b981aa}.dot.warn{background:#f59e0b;box-shadow:0 0 16px #f59e0baa}.dot.bad{background:#ef4444;box-shadow:0 0 16px #ef4444aa}pre{background:#0b1622;color:#d8eaff;border-radius:14px;padding:14px;overflow:auto;max-height:280px}</style></head><body><div class="wrap"><h1>ESP32-S3 OCPP Wallbox Simulator</h1><div class="card"><h2>WSS Connection LEDs</h2><div class="leds" id="leds"></div><p id="hint"></p></div><div class="card"><h2>Status</h2><div class="grid" id="status"></div></div><div class="card"><h2>OCPP Configuration</h2><label>ChargeboxID</label><input id="cbid"><label>BackendURL</label><input id="url"><label>WSS Password</label><input id="pw" type="password"><div class="row"><label class="check"><input id="auth" type="checkbox" style="width:auto"> Basic Auth enabled</label><label class="check"><input id="append" type="checkbox" style="width:auto"> Append ChargeboxID to URL</label></div><button onclick="saveCfg()">Save configuration</button><button onclick="connectOcpp()">Connect OCPP</button><p id="msg"></p></div><div class="card"><h2>Live Log</h2><pre id="log">...</pre></div></div><script>let editing=false;document.addEventListener('focusin',e=>{if(['INPUT','TEXTAREA','SELECT'].includes(e.target.tagName))editing=true});document.addEventListener('focusout',e=>{editing=false});function yn(v){return v?'Yes':'No'}async function load(){let s=await(await fetch('/api/state')).json();let led=(name,on,cls='on')=>'<div class="led"><span class="dot '+(on?cls:'')+'"></span>'+name+'</div>';leds.innerHTML=led('WLAN',s.net.wifi)+led('URL gültig',s.diagnosis.urlOk)+led('WSS/TLS verbunden',s.ocpp.wsConnected)+led('BootNotification gesendet',['bootSent','bootAccepted'].includes(s.diagnosis.phase),'warn')+led('Backend accepted',s.ocpp.accepted)+led('Fehler',s.diagnosis.phase==='disconnected'||s.diagnosis.phase==='urlError','bad');hint.textContent=s.diagnosis.error==='none'?'OK / kein Fehler gemeldet':s.diagnosis.error;let items=[['STA IP',s.net.staIp],['AP IP',s.net.apIp],['Full URL',s.config.fullUrl],['Auth',yn(s.config.basicAuthEnabled)],['State',s.state],['OCPP WS',yn(s.ocpp.wsConnected)],['Boot accepted',yn(s.ocpp.accepted)],['Heartbeat',s.ocpp.heartbeatIntervalSec+'s'],['Last status',s.ocpp.lastStatus],['Transaction',s.transactionId],['Power',s.powerKw.toFixed(1)+' kW']];status.innerHTML=items.map(i=>'<div class="metric"><div class="label">'+i[0]+'</div><div class="value">'+i[1]+'</div></div>').join('');log.textContent=(s.log||[]).join('\n');if(!editing){cbid.value=s.config.chargeboxId;url.value=s.config.backendUrl;pw.value=s.config.wssPassword;auth.checked=s.config.basicAuthEnabled;append.checked=s.config.appendChargeboxId}}async function saveCfg(){await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({chargeboxId:cbid.value,backendUrl:url.value,wssPassword:pw.value,basicAuthEnabled:auth.checked,appendChargeboxId:append.checked})});msg.textContent='Saved.';load()}async function connectOcpp(){await fetch('/api/connect',{method:'POST'});msg.textContent='Connecting...';load()}setInterval(load,1500);load();</script></body></html>)HTML"; }

void handleState(){ JsonDocument doc; JsonObject net=doc["net"].to<JsonObject>(); net["wifi"]=WiFi.status()==WL_CONNECTED; net["staIp"]=WiFi.localIP().toString(); net["apIp"]=WiFi.softAPIP().toString(); doc["state"]=state; doc["plugged"]=plugged; doc["authorized"]=authorized; doc["transactionActive"]=transactionActive; doc["transactionId"]=transactionId; doc["faulted"]=faulted; doc["powerKw"]=powerKw; JsonObject diag=doc["diagnosis"].to<JsonObject>(); diag["phase"]=connectPhase; diag["error"]=connectError; diag["urlOk"]=fullOcppUrl().startsWith("ws://")||fullOcppUrl().startsWith("wss://"); JsonObject ocpp=doc["ocpp"].to<JsonObject>(); ocpp["wsConnected"]=wsConnected; ocpp["accepted"]=ocppAccepted; ocpp["heartbeatIntervalSec"]=heartbeatIntervalSec; ocpp["lastStatus"]=lastReportedStatus; JsonObject cfg=doc["config"].to<JsonObject>(); cfg["chargeboxId"]=chargeboxId; cfg["backendUrl"]=backendUrl; cfg["fullUrl"]=fullOcppUrl(); cfg["wssPassword"]=wssPassword; cfg["basicAuthEnabled"]=basicAuthEnabled; cfg["appendChargeboxId"]=appendChargeboxId; JsonArray logs=doc["log"].to<JsonArray>(); for(int i=0;i<ocppLogCount;i++){ int idx=(ocppLogPos-ocppLogCount+i+40)%40; logs.add(ocppLog[idx]); } String out; serializeJson(doc,out); server.send(200,"application/json",out); }
void setupWeb(){ server.on("/",[](){server.send(200,"text/html",htmlPage());}); server.on("/api/state",HTTP_GET,handleState); server.on("/api/config",HTTP_POST,[](){ JsonDocument d; deserializeJson(d,server.arg("plain")); saveConfig(d); server.send(200,"application/json","{\"ok\":true}"); }); server.on("/api/connect",HTTP_POST,[](){ connectOcpp(); server.send(200,"application/json","{\"ok\":true}"); }); server.begin(); }
void setupNetwork(){ WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID,AP_PASS); WiFi.begin(WIFI_SSID,WIFI_PASS); configTime(0,0,"pool.ntp.org"); Serial.println("AP http://"+WiFi.softAPIP().toString()); }

void readInputs(){
  plugged=digitalRead(PIN_PLUG_SWITCH)==LOW; faulted=digitalRead(PIN_FAULT_SWITCH)==LOW; int raw=analogRead(PIN_POWER_POT); powerKw=map(raw,0,4095,0,220)/10.0;
  bool auth=digitalRead(PIN_AUTH_BUTTON), start=digitalRead(PIN_START_BUTTON), stop=digitalRead(PIN_STOP_BUTTON), reset=digitalRead(PIN_RESET_BUTTON), connect=digitalRead(PIN_CONNECT_BTN);
  if(lastPlug!=digitalRead(PIN_PLUG_SWITCH)){ lastPlug=digitalRead(PIN_PLUG_SWITCH); }
  if(lastAuth==HIGH && auth==LOW){ authorized=true; sendAuthorize(); }
  if(lastStart==HIGH && start==LOW && plugged && authorized && !faulted){ transactionActive=true; if(transactionId<0) transactionId=1; sendStartTransaction(); }
  if(lastStop==HIGH && stop==LOW){ sendStopTransaction(); transactionActive=false; transactionId=-1; }
  if(lastReset==HIGH && reset==LOW){ authorized=false; transactionActive=false; transactionId=-1; faulted=false; sessionWh=0; lastReportedStatus=""; }
  if(lastConnect==HIGH && connect==LOW){ connectOcpp(); }
  lastAuth=auth; lastStart=start; lastStop=stop; lastReset=reset; lastConnect=connect;
}

void setup(){ Serial.begin(115200); randomSeed(esp_random()); internalRgb.begin(); internalRgb.clear(); internalRgb.show(); initPins(); loadConfig(); setupNetwork(); setupWeb(); addLog("Ready. Webinterface on AP http://192.168.4.1"); }
void loop(){ server.handleClient(); ws.loop(); readInputs(); deriveState(); reportStatusIfChanged(); updateOutputs(); updateRgb(); if(ocppAccepted && millis()-lastHeartbeatMs > heartbeatIntervalSec*1000UL) sendHeartbeat(); if(transactionActive && transactionId>0 && millis()-lastMeterMs>10000){ sessionWh += powerKw*1000.0/360.0; sendMeterValues(); lastMeterMs=millis(); } }
