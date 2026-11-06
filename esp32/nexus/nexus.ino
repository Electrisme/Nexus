// NEXUS device firmware (ESP32)
// Copyright (c) 2026 Electrisme - MIT License
// Created by Electrisme: https://www.youtube.com/@Electrisme
//
// Arduino IDE 1.8 / 2.x
//   Board:    ESP32 Dev Module (esp32 core 2.0.17)
//   Library:  ArduinoJson 7.x
//
// ⚠️ BEFORE YOU PUSH TO GITHUB: clear the values in the SETTINGS block below
//    (put the placeholder values back). Never commit real Wi-Fi or tokens.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_wifi.h>

// ============ SETTINGS (fill these in) ============
const char* WIFI_SSID       = "YOUR_WIFI_SSID";        // first Wi-Fi network (you can add more from the panel)
const char* WIFI_PASS       = "YOUR_WIFI_PASSWORD";
const char* DEVICE_TOKEN    = "PUT_DEVICE_TOKEN_HERE"; // must match DEVICE_TOKEN in Cloudflare
const char* POLL_URL        = "https://nexus-hub.YOUR-SUBDOMAIN.workers.dev/poll";
const char* AP_PASS_DEFAULT = "12345678";              // default hotspot password (change it in the panel)
// ==================================================

const char* AP_SSID = "Nexus";                         // hotspot name

// Must match ALL_PINS in the Worker (src/index.js)
const uint8_t ALLOWED_PINS[] = {13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

const uint8_t  STATUS_LED_PIN   = 2;
const uint8_t  BUTTON_PIN       = 4;
const uint8_t  MAX_RELAYS       = 12;
const uint8_t  MAX_ID_BYTES     = 7;
const uint8_t  NAME_BUF         = 101;     // room for 24 characters including emoji
const uint8_t  MAX_ACKS         = 16;
const uint8_t  MAX_SAVED_WIFI   = 5;
const uint8_t  MAX_SCAN         = 15;
const uint16_t HTTP_TIMEOUT_MS  = 30000;   // server holds a poll up to 10 s; slow internet needs more room
const uint32_t RETRY_MS         = 1000;
const uint32_t WIFI_CONN_MS     = 12000;
const uint32_t WATCHDOG_MS      = 120000;  // Wi-Fi up but no server reply → restart the link
const uint32_t NET_STALE_MS     = 15000;
const uint32_t BLINK_MS         = 1000;
const uint32_t AP_LEAVE_MS      = 20000;   // connected to Wi-Fi and the panel has been idle → hotspot off
const uint32_t AP_IDLE_MS       = 120000;  // nobody connected to the hotspot → off after this
const uint32_t DEBOUNCE_MS      = 50;
const uint32_t HEARTBEAT_MS     = 10000;
const uint8_t  MIN_AP_PASS      = 8;
const uint8_t  MAX_AP_PASS      = 63;

#define CS_IDLE       0
#define CS_CONNECTING 1
#define CS_OK         2
#define CS_FAIL       3

struct Relay {
  char    id[8];
  char    name[NAME_BUF];
  uint8_t pin;
  bool    activeLow;
  bool    on;
};

struct WifiCred {
  char    ssid[33];
  char    pass[65];
  uint8_t ch;          // channel of the last successful join (faster next time)
  uint8_t bssid[6];
  bool    hasHint;
};

struct NetInfo {
  char    ssid[33];
  int8_t  rssi;
  bool    secure;
  uint8_t ch;
  uint8_t bssid[6];
};

Relay    relays[MAX_RELAYS];
uint8_t  relayCount = 0;
uint32_t cfgVer = 0;

struct PendingAck { char id[40]; bool ok; };
PendingAck acks[MAX_ACKS];
uint8_t    ackCount = 0;

char    recentIds[8][40];
uint8_t recentIdx = 0;

WifiCred saved[MAX_SAVED_WIFI];
uint8_t  savedCount = 0;

WifiCred         connReq;
volatile bool    connPending = false;
volatile uint8_t connState   = CS_IDLE;
char             connMsg[120] = "";

NetInfo          scanRes[MAX_SCAN];
uint8_t          scanCount = 0;
volatile bool    scanPending = false;
uint32_t         scanAt = 0;

char             apPass[MAX_AP_PASS + 2] = "";
volatile bool    apOffPending = false;
volatile bool    apRestartPending = false;
bool             apHadClient = false;   // did any client connect since the hotspot started?

SemaphoreHandle_t mu;
#define LOCK()   xSemaphoreTake(mu, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mu)

Preferences prefs;
WiFiClientSecure secureClient;
HTTPClient http;
WebServer server(80);

uint32_t lastPollOkAt   = 0;
bool     everPolled     = false;
bool     apOn           = false;
uint32_t apLastActivity = 0;
uint32_t nextPollAt     = 0;
uint32_t nextWifiAt     = 0;
uint32_t hbAt           = 0;
uint32_t pollCount      = 0;
uint32_t failCount      = 0;
uint32_t wifiDrops      = 0;
uint8_t  wifiFails      = 0;
bool     wasConn        = false;

// ================= basic helpers =================

bool pinOk(int p) {
  for (uint8_t i = 0; i < sizeof(ALLOWED_PINS); i++) {
    if (ALLOWED_PINS[i] == p) return true;
  }
  return false;
}

// Electrical level that turns the relay on or off
inline int levelFor(const Relay& r, bool on) {
  return (on == r.activeLow) ? LOW : HIGH;
}

// Internal pull matches the relay logic so the relay stays off at boot:
//   LOW (active-low)  → pull-up   (idle HIGH = off)
//   HIGH (active-high)→ pull-down (idle LOW  = off)
inline uint8_t pullModeFor(const Relay& r) {
  return r.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN;
}

// Safe output start: input with pull → set level → output (no glitch)
void safeOutput(const Relay& r, bool on) {
  pinMode(r.pin, pullModeFor(r));
  digitalWrite(r.pin, levelFor(r, on));
  pinMode(r.pin, OUTPUT);
}

// Call only while holding the lock
void setRelayRaw(Relay& r, bool on) {
  digitalWrite(r.pin, levelFor(r, on));
  r.on = on;
}

int findRelay(const char* id) {
  for (uint8_t i = 0; i < relayCount; i++) {
    if (strcmp(relays[i].id, id) == 0) return i;
  }
  return -1;
}

void bssidToStr(const uint8_t* b, char* out, size_t n) {
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

bool strToBssid(const char* s, uint8_t* b) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (uint8_t i = 0; i < 6; i++) b[i] = (uint8_t)v[i];
  return true;
}

// ================= relay config in NVS =================

void saveConfigLocked() {
  JsonDocument d;
  d["ver"] = cfgVer;
  JsonArray arr = d["relays"].to<JsonArray>();
  for (uint8_t i = 0; i < relayCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]   = relays[i].id;
    o["name"] = relays[i].name;
    o["pin"]  = relays[i].pin;
    o["low"]  = relays[i].activeLow;
  }
  String s;
  serializeJson(d, s);
  prefs.putString("cfg", s);
}

// At boot every relay is set OFF before anything else runs
void loadConfig() {
  relayCount = 0;
  cfgVer = 0;
  String s = prefs.getString("cfg", "");
  if (!s.length()) return;

  JsonDocument d;
  if (deserializeJson(d, s)) return;
  cfgVer = d["ver"] | 0;

  for (JsonObject o : d["relays"].as<JsonArray>()) {
    if (relayCount >= MAX_RELAYS) break;
    int pin = o["pin"] | 0;
    if (!pinOk(pin)) continue;

    const char* id = o["id"]   | "";
    const char* nm = o["name"] | "";
    if (strlen(id) == 0 || strlen(id) > MAX_ID_BYTES) continue;

    Relay& r = relays[relayCount];
    strlcpy(r.id, id, sizeof(r.id));
    strlcpy(r.name, strlen(nm) ? nm : id, sizeof(r.name));
    r.pin = pin;
    r.activeLow = o["low"] | true;
    r.on = false;

    safeOutput(r, false);
    relayCount++;
  }
}

// Apply a new config from the server. Invalid input changes nothing.
// Call only while holding the lock.
bool applyConfig(JsonArray arr, uint32_t ver) {
  static Relay next[MAX_RELAYS];
  uint8_t n = 0;

  for (JsonObject o : arr) {
    if (n >= MAX_RELAYS) return false;
    const char* id = o["id"]    | "";
    const char* nm = o["name"]  | "";
    const char* lg = o["logic"] | "L";
    int pin        = o["pin"]   | 0;

    if (strlen(id) == 0 || strlen(id) > MAX_ID_BYTES) return false;
    if (!pinOk(pin)) return false;

    for (uint8_t j = 0; j < n; j++) {
      if (strcmp(next[j].id, id) == 0) return false;
      if (next[j].pin == pin) return false;
    }

    Relay r{};
    strlcpy(r.id, id, sizeof(r.id));
    strlcpy(r.name, strlen(nm) ? nm : id, sizeof(r.name));
    r.pin = pin;
    r.activeLow = (strcmp(lg, "H") != 0);
    r.on = false;

    int old = findRelay(id);
    if (old >= 0 && relays[old].pin == pin && relays[old].activeLow == r.activeLow) {
      r.on = relays[old].on;   // keep the state of unchanged relays
    }
    next[n++] = r;
  }

  // Turn off relays that were removed or whose pin/logic changed
  for (uint8_t i = 0; i < relayCount; i++) {
    bool keep = false;
    for (uint8_t j = 0; j < n; j++) {
      if (strcmp(relays[i].id, next[j].id) == 0 &&
          relays[i].pin == next[j].pin &&
          relays[i].activeLow == next[j].activeLow) keep = true;
    }
    if (!keep && relays[i].on) setRelayRaw(relays[i], false);
  }

  for (uint8_t j = 0; j < n; j++) safeOutput(next[j], next[j].on);

  for (uint8_t j = 0; j < n; j++) relays[j] = next[j];
  relayCount = n;
  cfgVer = ver;
  saveConfigLocked();
  return true;
}

// ================= duplicate commands and ACK queue =================
// Network task only

bool isRecent(const char* id) {
  for (uint8_t i = 0; i < 8; i++) {
    if (strcmp(recentIds[i], id) == 0) return true;
  }
  return false;
}

void remember(const char* id) {
  strlcpy(recentIds[recentIdx], id, sizeof(recentIds[0]));
  recentIdx = (recentIdx + 1) % 8;
}

void pushAck(const char* id, bool ok) {
  if (ackCount >= MAX_ACKS) return;
  strlcpy(acks[ackCount].id, id, sizeof(acks[0].id));
  acks[ackCount].ok = ok;
  ackCount++;
}

void handleCommand(const char* id, const char* target, const char* state) {
  if (strlen(id) == 0) return;
  if (isRecent(id)) { pushAck(id, true); return; }

  bool on    = strcmp(state, "ON") == 0;
  bool valid = on || strcmp(state, "OFF") == 0;

  LOCK();
  int idx = findRelay(target);
  bool ok = valid && idx >= 0;
  if (ok) setRelayRaw(relays[idx], on);
  UNLOCK();

  if (ok) remember(id);
  pushAck(id, ok);
  Serial.printf("cmd %s -> %s : %s\n", target, state, ok ? "ok" : "fail");
}

void handleConfig(const char* id, uint32_t ver, JsonArray arr) {
  if (strlen(id) == 0) return;
  if (isRecent(id)) { pushAck(id, true); return; }

  LOCK();
  bool ok = applyConfig(arr, ver);
  UNLOCK();

  if (ok) remember(id);
  pushAck(id, ok);
  Serial.printf("config v%lu : %s\n", (unsigned long)ver, ok ? "applied" : "rejected");
}

// ================= network status =================

bool netOkNow() {
  bool ok;
  LOCK();
  ok = everPolled && (millis() - lastPollOkAt) < NET_STALE_MS;
  UNLOCK();
  return ok && WiFi.status() == WL_CONNECTED;
}

// ================= saved Wi-Fi networks =================

// Call only while holding the lock
void saveWifiLocked() {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (uint8_t i = 0; i < savedCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["s"] = saved[i].ssid;
    o["p"] = saved[i].pass;
    o["c"] = saved[i].ch;
    if (saved[i].hasHint) {
      char b[20];
      bssidToStr(saved[i].bssid, b, sizeof(b));
      o["b"] = b;
    }
  }
  String s;
  serializeJson(d, s);
  prefs.putString("wifi", s);
}

void loadWifi() {
  savedCount = 0;
  String s = prefs.getString("wifi", "");
  if (s.length()) {
    JsonDocument d;
    if (!deserializeJson(d, s)) {
      for (JsonObject o : d.as<JsonArray>()) {
        if (savedCount >= MAX_SAVED_WIFI) break;
        WifiCred& c = saved[savedCount];
        memset(&c, 0, sizeof(c));
        strlcpy(c.ssid, o["s"] | "", sizeof(c.ssid));
        strlcpy(c.pass, o["p"] | "", sizeof(c.pass));
        c.ch = o["c"] | 0;
        const char* bs = o["b"] | "";
        c.hasHint = (c.ch > 0) && strToBssid(bs, c.bssid);
        if (strlen(c.ssid)) savedCount++;
      }
    }
  }
  // First boot: add the Wi-Fi from the SETTINGS block (if it was filled in)
  if (!savedCount && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
    memset(&saved[0], 0, sizeof(WifiCred));
    strlcpy(saved[0].ssid, WIFI_SSID, sizeof(saved[0].ssid));
    strlcpy(saved[0].pass, WIFI_PASS, sizeof(saved[0].pass));
    savedCount = 1;
    LOCK();
    saveWifiLocked();
    UNLOCK();
  }
}

// Call only while holding the lock: puts this network first
void addSavedFrontLocked(const WifiCred& c) {
  WifiCred tmp[MAX_SAVED_WIFI];
  uint8_t n = 0;
  tmp[n++] = c;
  for (uint8_t i = 0; i < savedCount && n < MAX_SAVED_WIFI; i++) {
    if (strcmp(saved[i].ssid, c.ssid) == 0) continue;
    tmp[n++] = saved[i];
  }
  for (uint8_t i = 0; i < n; i++) saved[i] = tmp[i];
  savedCount = n;
  saveWifiLocked();
}

// Call only while holding the lock: saves channel/BSSID only if they changed
void updateSavedHintLocked(const WifiCred& c) {
  for (uint8_t i = 0; i < savedCount; i++) {
    if (strcmp(saved[i].ssid, c.ssid) != 0) continue;
    bool same = saved[i].hasHint == c.hasHint && saved[i].ch == c.ch &&
                memcmp(saved[i].bssid, c.bssid, 6) == 0;
    if (same) return;
    saved[i].ch = c.ch;
    saved[i].hasHint = c.hasHint;
    memcpy(saved[i].bssid, c.bssid, 6);
    saveWifiLocked();
    return;
  }
}

// Call only while holding the lock
bool removeSavedLocked(const char* ssid) {
  uint8_t n = 0;
  bool found = false;
  WifiCred tmp[MAX_SAVED_WIFI];
  for (uint8_t i = 0; i < savedCount; i++) {
    if (strcmp(saved[i].ssid, ssid) == 0) { found = true; continue; }
    tmp[n++] = saved[i];
  }
  for (uint8_t i = 0; i < n; i++) saved[i] = tmp[i];
  savedCount = n;
  if (found) saveWifiLocked();
  return found;
}

// ================= Wi-Fi (main loop only) =================

const char* wifiStatusText(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "network not found";
    case WL_CONNECT_FAILED: return "connect failed (wrong password?)";
    case WL_DISCONNECTED:   return "disconnected";
    case WL_IDLE_STATUS:    return "idle";
    default:                return "unknown";
  }
}

const char* uiReason(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "Network not found. Check the name or move closer.";
    case WL_CONNECT_FAILED: return "Wrong password, or the network refused the connection.";
    default:                return "Could not connect. Try again.";
  }
}

// One join attempt. With channel and BSSID it is faster.
bool tryJoin(const char* ssid, const char* pass, uint8_t ch, const uint8_t* bssid, uint32_t timeoutMs) {
  WiFi.disconnect(false, false);   // STA only; the hotspot is not affected
  delay(150);
  if (ch && bssid) WiFi.begin(ssid, pass, ch, bssid);
  else             WiFi.begin(ssid, pass);

  uint32_t end = millis() + timeoutMs;
  while ((int32_t)(millis() - end) < 0 && WiFi.status() != WL_CONNECTED) delay(150);

  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("  '%s' failed: %s\n", ssid, wifiStatusText(WiFi.status()));
  return false;
}

bool joinWithHints(const WifiCred& c) {
  if (c.hasHint && tryJoin(c.ssid, c.pass, c.ch, c.bssid, 6000)) return true;
  return tryJoin(c.ssid, c.pass, 0, nullptr, WIFI_CONN_MS);
}

bool connectSaved() {
  WifiCred list[MAX_SAVED_WIFI];
  uint8_t n;
  LOCK();
  n = savedCount;
  for (uint8_t i = 0; i < n; i++) list[i] = saved[i];
  UNLOCK();

  for (uint8_t i = 0; i < n; i++) {
    Serial.printf("trying saved '%s'\n", list[i].ssid);
    if (joinWithHints(list[i])) {
      WifiCred upd = list[i];
      upd.ch = (uint8_t)WiFi.channel();
      memcpy(upd.bssid, WiFi.BSSID(), 6);
      upd.hasHint = upd.ch > 0;
      LOCK();
      updateSavedHintLocked(upd);
      UNLOCK();
      return true;
    }
  }
  return false;
}

void setMsgLocked(const char* m) {
  strlcpy(connMsg, m, sizeof(connMsg));
}

void handleConnRequest() {
  WifiCred req;
  LOCK();
  req = connReq;
  connPending = false;
  connState = CS_CONNECTING;
  setMsgLocked("Connecting…");
  UNLOCK();

  Serial.printf("connect request: '%s'\n", req.ssid);
  if (joinWithHints(req)) {
    WifiCred done = req;
    done.ch = (uint8_t)WiFi.channel();
    memcpy(done.bssid, WiFi.BSSID(), 6);
    done.hasHint = done.ch > 0;
    LOCK();
    addSavedFrontLocked(done);
    connState = CS_OK;
    setMsgLocked("Connected and saved.");
    UNLOCK();
    Serial.println("connect request OK");
  } else {
    LOCK();
    connState = CS_FAIL;
    setMsgLocked(uiReason(WiFi.status()));
    UNLOCK();
    nextWifiAt = 0;   // fall back to the saved networks
  }
}

// Scan (main loop only), strongest signal first
void doScan() {
  scanPending = false;
  int n = WiFi.scanNetworks(false, false);
  NetInfo tmp[MAX_SCAN];
  uint8_t c = 0;

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s.length() > 32) continue;
    int rssi = WiFi.RSSI(i);

    int dupAt = -1;
    for (uint8_t j = 0; j < c; j++) {
      if (strcmp(tmp[j].ssid, s.c_str()) == 0) { dupAt = j; break; }
    }
    if (dupAt >= 0) {
      if (rssi > tmp[dupAt].rssi) {
        tmp[dupAt].rssi = (int8_t)rssi;
        tmp[dupAt].ch = (uint8_t)WiFi.channel(i);
        memcpy(tmp[dupAt].bssid, WiFi.BSSID(i), 6);
      }
      continue;
    }
    if (c >= MAX_SCAN) break;

    NetInfo& x = tmp[c];
    memset(&x, 0, sizeof(x));
    strlcpy(x.ssid, s.c_str(), sizeof(x.ssid));
    x.rssi = (int8_t)rssi;
    x.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    x.ch = (uint8_t)WiFi.channel(i);
    memcpy(x.bssid, WiFi.BSSID(i), 6);
    c++;
  }
  WiFi.scanDelete();

  for (uint8_t i = 1; i < c; i++) {
    NetInfo key = tmp[i];
    int j = (int)i - 1;
    while (j >= 0 && tmp[j].rssi < key.rssi) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }

  LOCK();
  for (uint8_t i = 0; i < c; i++) scanRes[i] = tmp[i];
  scanCount = c;
  UNLOCK();
  scanAt = millis();
}

// ================= poll (main loop only) =================

String buildPollBody() {
  JsonDocument b;
  b["v"] = 1;
  b["info"]["device"] = "nexus-01";
  b["info"]["fw"]     = "v1.0.0-beta";
  b["info"]["ssid"]   = WiFi.SSID();
  b["info"]["rssi"]   = WiFi.RSSI();
  b["info"]["uptime"] = millis() / 1000;
  b["info"]["heap"]   = ESP.getFreeHeap();

  LOCK();
  b["info"]["cfg"] = cfgVer;
  JsonObject st = b["st"].to<JsonObject>();
  for (uint8_t i = 0; i < relayCount; i++) {
    st[relays[i].id] = relays[i].on ? "ON" : "OFF";
  }
  UNLOCK();

  JsonArray arr = b["acks"].to<JsonArray>();
  for (uint8_t i = 0; i < ackCount; i++) {
    JsonObject a = arr.add<JsonObject>();
    a["id"] = acks[i].id;
    a["ok"] = acks[i].ok;
  }

  String out;
  serializeJson(b, out);
  return out;
}

bool doPoll() {
  String out = buildPollBody();

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(true);
  uint32_t t0 = millis();
  pollCount++;

  if (!http.begin(secureClient, POLL_URL)) {
    failCount++;
    Serial.println("poll: http.begin failed");
    secureClient.stop();
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  int code = http.POST(out);
  String resp = (code == 200) ? http.getString() : "";
  http.end();
  float sec = (millis() - t0) / 1000.0;

  if (code != 200) {
    failCount++;
    Serial.printf("poll FAIL code %d after %.1fs (fails %lu)\n",
                  code, sec, (unsigned long)failCount);
    secureClient.stop();
    return false;   // keep the ACKs and send them again next time
  }

  ackCount = 0;     // the server accepted the ACKs
  LOCK();
  lastPollOkAt = millis();
  everPolled = true;
  UNLOCK();

  JsonDocument d;
  if (deserializeJson(d, resp)) {
    failCount++;
    Serial.println("poll: bad JSON from server");
    secureClient.stop();
    return false;
  }

  const char* type = d["type"] | "NOOP";
  if (strcmp(type, "NOOP") != 0) Serial.printf("poll OK: %s (%.1fs)\n", type, sec);

  if (strcmp(type, "COMMAND") == 0) {
    handleCommand(d["id"] | "", d["target"] | "", d["state"] | "");
  } else if (strcmp(type, "CONFIG") == 0) {
    handleConfig(d["id"] | "", (uint32_t)(d["ver"] | 0), d["relays"].as<JsonArray>());
  }
  return true;
}

// ================= hotspot, button and local panel (own task) =================

void touch() { apLastActivity = millis(); }

void apStart() {
  if (apOn) return;
  if (strlen(apPass) < MIN_AP_PASS) {
    Serial.println("AP start skipped: password is shorter than 8 characters");
    return;
  }

  // Only switch the mode when needed (switching often can show a default name)
  if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);

  // The hotspot must use the router's channel while connected
  int ch = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 1;

  bool ok = false;
  for (uint8_t tries = 0; tries < 3 && !ok; tries++) {
    ok = WiFi.softAP(AP_SSID, apPass, ch, 0, 4);
    if (ok && WiFi.softAPSSID() != String(AP_SSID)) {
      Serial.println("AP name mismatch, retrying");
      WiFi.softAPdisconnect(false);
      ok = false;
    }
    if (!ok) delay(300);
  }
  if (!ok) {
    Serial.println("AP start FAILED");
    return;
  }

  server.begin();
  apOn = true;
  apHadClient = false;
  touch();
  Serial.printf("AP on: %s  ch %d  IP %s\n", AP_SSID, ch, WiFi.softAPIP().toString().c_str());
}

void apStop() {
  if (!apOn) return;
  server.stop();
  WiFi.softAPdisconnect(false);   // keeps the mode; only the hotspot goes down
  apOn = false;
  apHadClient = false;
  Serial.println("AP off");
}

void handleButton() {
  static bool lastRaw = HIGH;
  static bool stable  = HIGH;
  static uint32_t changeAt = 0;

  bool raw = digitalRead(BUTTON_PIN);
  if (raw != lastRaw) {
    lastRaw = raw;
    changeAt = millis();
  }
  if ((uint32_t)(millis() - changeAt) >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable == LOW) {           // button pressed
      if (!apOn) apStart();
      else touch();
    }
  }
}

// Hotspot turns off:
//  - right after the last client leaves (if a client had connected)
//  - when connected to Wi‑Fi and the panel has been idle for AP_LEAVE_MS
//  - when nobody has connected for AP_IDLE_MS
void handleApIdle() {
  if (!apOn) return;

  uint8_t stations = WiFi.softAPgetStationNum();
  if (stations > 0) apHadClient = true;

  if (apHadClient && stations == 0) {
    Serial.println("AP: client left, turning off");
    apStop();
    return;
  }

  uint32_t idle = millis() - apLastActivity;
  bool sta = WiFi.status() == WL_CONNECTED;

  if (sta && idle > AP_LEAVE_MS) { apStop(); return; }
  if (!apHadClient && idle > AP_IDLE_MS) apStop();
}

const char PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0e14">
<title>Nexus</title>
<style>
:root{--bg:#0b0e14;--card:#121826;--card2:#0d131e;--line:#1f2a40;--txt:#e8ecf3;--mut:#8b96ab;--acc:#4f7cff;--ok:#2ecc71;--bad:#ff5b6e;--warn:#f5b942}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
[hidden]{display:none!important}
html,body{margin:0;background:var(--bg);color:var(--txt);font-family:system-ui,Segoe UI,Roboto,sans-serif;font-size:15px}
body{padding:14px 12px 96px;min-height:100vh}
.wrap{max-width:480px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:center;gap:10px;margin:4px 2px 16px}
.brand{display:flex;align-items:center;gap:10px}
.logo{width:36px;height:36px;border-radius:11px;background:linear-gradient(135deg,#4f7cff,#7b5cff);display:grid;place-items:center;font-weight:800}
h1{font-size:18px;margin:0;letter-spacing:.4px}
.sub{font-size:12px;color:var(--mut);margin-top:2px}
.pill{font-size:12px;padding:6px 11px;border-radius:99px;background:var(--card);border:1px solid var(--line);color:var(--mut);white-space:nowrap}
.pill.ok{color:var(--ok);border-color:#1f4d33}
.pill.bad{color:var(--bad);border-color:#5a2530}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:14px;margin-bottom:12px}
.head{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:10px}
.head h2{font-size:13px;margin:0;color:var(--mut);font-weight:600}
.relays{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.relay{background:var(--card2);border:1px solid var(--line);border-radius:16px;padding:14px;color:var(--txt);text-align:start;cursor:pointer;width:100%;font:inherit}
.relay .n{font-weight:700;font-size:15px;word-break:break-word}
.relay .s{font-size:12px;color:var(--mut);margin-top:9px;direction:ltr;text-align:start;display:flex;align-items:center;gap:6px}
.dot{width:8px;height:8px;border-radius:50%;background:#34405c;display:inline-block;flex:none}
.relay.on{background:linear-gradient(135deg,#2b5cff,#4f7cff);border-color:#4f7cff}
.relay.on .s{color:#e4ebff}
.relay.on .dot{background:#fff;box-shadow:0 0 8px #fff}
.relay:disabled{opacity:.55;cursor:wait}
.empty{color:var(--mut);font-size:13px;text-align:center;padding:16px 0}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:11px 0;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}
.row .k{color:var(--mut);font-size:13px}
.row .v{font-weight:600;direction:ltr;text-align:end;word-break:break-all}
.item{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:11px 2px;border-bottom:1px solid var(--line)}
.item:last-child{border-bottom:0}
.item .nm{font-weight:600;word-break:break-all}
.item .meta{font-size:12px;color:var(--mut);direction:ltr;margin-top:3px}
.tag{font-size:11px;background:#15311f;color:var(--ok);padding:3px 8px;border-radius:99px;margin-inline-start:6px;white-space:nowrap}
.btn{background:#1a2338;border:1px solid var(--line);color:var(--txt);border-radius:11px;padding:9px 13px;font:inherit;cursor:pointer;white-space:nowrap}
.btn:disabled{opacity:.5;cursor:default}
.btn.danger{color:var(--bad)}
.btn.ghost{background:transparent;border-color:transparent;color:var(--acc);padding:6px 4px}
.btn.primary{background:var(--acc);border-color:var(--acc);font-weight:700;width:100%;padding:14px;font-size:15px}
.btn.wide{width:100%;padding:13px}
label{display:block;font-size:12px;color:var(--mut);margin:12px 2px 6px}
.field{position:relative}
input{width:100%;background:var(--card2);border:1px solid var(--line);color:var(--txt);border-radius:13px;padding:13px 14px;font:inherit;direction:ltr;text-align:start}
input:focus{outline:none;border-color:var(--acc)}
input[readonly]{color:var(--mut)}
.eye{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:0;color:var(--mut);cursor:pointer;font:inherit;padding:8px;font-size:13px}
.msg{font-size:13px;min-height:20px;margin:10px 2px 0;color:var(--mut);line-height:1.6}
.msg.ok{color:var(--ok)}
.msg.bad{color:var(--bad)}
.msg.busy{color:var(--warn)}
.hint{font-size:12px;color:var(--mut);line-height:1.8;margin-top:10px}
.spin{display:inline-block;width:12px;height:12px;border:2px solid currentColor;border-top-color:transparent;border-radius:50%;animation:sp .8s linear infinite;vertical-align:-2px;margin-inline-start:6px}
@keyframes sp{to{transform:rotate(360deg)}}
nav{position:fixed;left:0;right:0;bottom:0;background:rgba(11,14,20,.94);backdrop-filter:blur(8px);border-top:1px solid var(--line);display:flex;justify-content:space-around;padding:8px 6px calc(8px + env(safe-area-inset-bottom,0px))}
nav button{flex:1;background:none;border:0;color:var(--mut);font:inherit;font-size:13px;padding:8px 4px;border-radius:12px;cursor:pointer}
nav button.active{color:var(--txt);background:var(--card)}
nav button.active b{color:var(--acc)}
.sheet-bg{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;align-items:flex-end;justify-content:center;z-index:10}
.sheet{width:100%;max-width:480px;background:var(--card);border:1px solid var(--line);border-radius:22px 22px 0 0;padding:16px 16px calc(18px + env(safe-area-inset-bottom,0px))}
.sheet-h{display:flex;justify-content:space-between;align-items:center;margin-bottom:4px}
.sheet-h b{font-size:16px}
.x{background:none;border:0;color:var(--mut);font-size:18px;cursor:pointer;padding:4px 8px}
</style></head><body>
<div class="wrap">
  <header>
    <div class="brand">
      <div class="logo">N</div>
      <div><h1>NEXUS</h1><div class="sub" id="sub">Connecting…</div></div>
    </div>
    <div class="pill" id="netpill">—</div>
  </header>

  <!-- ================= Relays ================= -->
  <section id="pane-relays">
    <div class="card">
      <div class="head"><h2>Relays</h2><span class="sub" id="rcount"></span></div>
      <div class="relays" id="grid"></div>
    </div>
  </section>

  <!-- ================= Network ================= -->
  <section id="pane-net" hidden>
    <div class="card">
      <div class="head"><h2>Current connection</h2><span class="pill" id="curpill">—</span></div>
      <div class="row"><span class="k">Network</span><span class="v" id="cur">—</span></div>
      <div class="row"><span class="k">IP address</span><span class="v" id="ip">—</span></div>
      <div class="row"><span class="k">Signal</span><span class="v" id="rssi">—</span></div>
    </div>

    <div class="card">
      <div class="head"><h2>Saved networks</h2><span class="sub">top = first choice</span></div>
      <div id="saved"></div>
    </div>

    <div class="card">
      <div class="head">
        <h2>Add a network</h2>
        <button class="btn ghost" id="scanBtn" onclick="scan()">Scan</button>
      </div>
      <div id="nets"></div>
      <div class="hint" id="scanNote"></div>
      <div style="margin-top:8px"><button class="btn wide" onclick="openManual()">Hidden network (enter the name)</button></div>
    </div>
  </section>

  <!-- ================= Device ================= -->
  <section id="pane-dev" hidden>
    <div class="card">
      <div class="head"><h2>Device</h2></div>
      <div class="row"><span class="k">Firmware</span><span class="v" id="fw">—</span></div>
      <div class="row"><span class="k">Last server contact</span><span class="v" id="lastpoll">—</span></div>
      <div class="row"><span class="k">Uptime</span><span class="v" id="uptime">—</span></div>
      <div class="row"><span class="k">Free memory</span><span class="v" id="heap">—</span></div>
      <div class="row"><span class="k">Wi‑Fi drops</span><span class="v" id="drops">—</span></div>
    </div>

    <div class="card">
      <div class="head"><h2>Hotspot</h2></div>
      <div class="row"><span class="k">Name</span><span class="v" id="apname">Nexus</span></div>
      <label for="newpass">New hotspot password</label>
      <div class="field">
        <input id="newpass" type="password" autocomplete="new-password" placeholder="8 to 63 characters">
        <button class="eye" type="button" onclick="eye('newpass',this)">Show</button>
      </div>
      <div style="height:10px"></div>
      <button class="btn primary" onclick="savePass()">Save password</button>
      <div class="msg" id="passmsg"></div>
      <div style="height:10px"></div>
      <button class="btn wide danger" onclick="apOff()">Turn hotspot off</button>
      <div class="hint">The hotspot turns off when you leave this page, and comes back when you press the button on the device.</div>
    </div>
  </section>
</div>

<nav>
  <button class="tabbtn active" data-t="relays" onclick="tab('relays')"><b>Relays</b></button>
  <button class="tabbtn" data-t="net" onclick="tab('net')"><b>Network</b></button>
  <button class="tabbtn" data-t="dev" onclick="tab('dev')"><b>Device</b></button>
</nav>

<!-- ================= Connect sheet ================= -->
<div class="sheet-bg" id="sheet" hidden onclick="if(event.target===this)closeSheet()">
  <div class="sheet">
    <div class="sheet-h"><b id="sh-title">Connect to network</b><button class="x" onclick="closeSheet()">✕</button></div>
    <label for="sh-ssid">Network name</label>
    <input id="sh-ssid" autocomplete="off" autocapitalize="off" spellcheck="false">
    <div id="sh-passrow">
      <label for="sh-pass">Password</label>
      <div class="field">
        <input id="sh-pass" type="password" autocomplete="off">
        <button class="eye" type="button" onclick="eye('sh-pass',this)">Show</button>
      </div>
    </div>
    <div style="height:14px"></div>
    <button class="btn primary" id="sh-go" onclick="doConnect()">Connect</button>
    <div class="msg" id="sh-msg"></div>
  </div>
</div>

<script>
const $=id=>document.getElementById(id);
let sheetSecure=true, busy=false, watchTimer=null, lastSaved='', lastNets='';

async function call(url,opts){
  try{
    const r=await fetch(url,Object.assign({cache:'no-store'},opts||{}));
    let j={}; try{ j=await r.json(); }catch(e){}
    return {ok:r.ok,status:r.status,data:j};
  }catch(e){ return {ok:false,status:0,data:{msg:'Connection to the device was lost'}}; }
}
function form(obj){ return Object.keys(obj).map(k=>k+'='+encodeURIComponent(obj[k])).join('&'); }
function post(url,obj){ return call(url,{method:'POST',headers:{'content-type':'application/x-www-form-urlencoded'},body:form(obj||{})}); }

function tab(name){
  ['relays','net','dev'].forEach(n=>{ $('pane-'+n).hidden = (n!==name); });
  document.querySelectorAll('.tabbtn').forEach(b=>b.classList.toggle('active', b.dataset.t===name));
  if(name==='net') loadWifi();
  if(name==='dev') loadDevice();
}

async function loadState(){
  const r=await call('/api/state');
  if(!r.ok && !r.data.relays){
    $('sub').textContent=r.data.msg||'Connection lost';
    const p=$('netpill'); p.textContent='Offline'; p.className='pill bad'; return;
  }
  const d=r.data;
  $('sub').textContent=d.net?'Internet connected':'No internet';
  const p=$('netpill');
  p.textContent=d.net?'Online':'Offline';
  p.className='pill '+(d.net?'ok':'bad');
  renderRelays(d.relays||[]);
}

function renderRelays(list){
  const g=$('grid');
  $('rcount').textContent=list.length?(list.filter(r=>r.on).length+' on of '+list.length):'';
  g.textContent='';
  if(!list.length){ const e=document.createElement('div'); e.className='empty'; e.textContent='No relays yet'; g.appendChild(e); return; }
  for(const r of list){
    const b=document.createElement('button');
    b.className='relay'+(r.on?' on':'');
    const n=document.createElement('div'); n.className='n'; n.textContent=r.name;
    const s=document.createElement('div'); s.className='s';
    const d=document.createElement('span'); d.className='dot';
    const t=document.createElement('span'); t.textContent=(r.on?'ON':'OFF')+' · pin '+r.pin;
    s.appendChild(d); s.appendChild(t);
    b.appendChild(n); b.appendChild(s);
    b.onclick=()=>toggle(r.id,!r.on,b);
    g.appendChild(b);
  }
}

async function toggle(id,on,b){
  b.disabled=true;
  await post('/api/set',{id:id,on:on?'1':'0'});
  b.disabled=false;
  loadState();
}

async function loadWifi(){
  const r=await call('/api/wifi');
  if(!r.ok) return;
  const d=r.data;
  $('cur').textContent=d.current||'—';
  $('ip').textContent=d.ip||'—';
  $('rssi').textContent=d.rssi?(d.rssi+' dBm'):'—';
  const cp=$('curpill');
  cp.textContent=d.current?'Connected':'Not connected';
  cp.className='pill '+(d.current?'ok':'bad');
  renderSaved(d.saved||[]);
  renderNets(d.nets||[], d.current||'');
  $('scanBtn').disabled=!!d.scanning;
  $('scanBtn').innerHTML=d.scanning?'Scanning<span class="spin"></span>':'Scan';
  $('scanNote').textContent=d.scanning?'':(d.nets&&d.nets.length?'':'Tap Scan to see nearby networks.');
  if(d.state===1) $('sh-msg').textContent=d.msg||'';
}

function renderSaved(list){
  const key=JSON.stringify(list);
  if(key===lastSaved) return;
  lastSaved=key;
  const box=$('saved'); box.textContent='';
  if(!list.length){ const e=document.createElement('div'); e.className='empty'; e.textContent='No saved networks'; box.appendChild(e); return; }
  list.forEach(n=>{
    const row=document.createElement('div'); row.className='item';
    const left=document.createElement('div');
    const nm=document.createElement('div'); nm.className='nm'; nm.textContent=n.ssid;
    if(n.active){ const t=document.createElement('span'); t.className='tag'; t.textContent='connected'; nm.appendChild(t); }
    const meta=document.createElement('div'); meta.className='meta';
    meta.textContent=n.prio?'first choice':'';
    left.appendChild(nm); left.appendChild(meta);
    const del=document.createElement('button'); del.className='btn danger'; del.textContent='Remove';
    del.onclick=()=>forget(n.ssid);
    row.appendChild(left); row.appendChild(del);
    box.appendChild(row);
  });
}

function renderNets(list, cur){
  const key=JSON.stringify([list,cur]);
  if(key===lastNets) return;
  lastNets=key;
  const box=$('nets'); box.textContent='';
  list.forEach(n=>{
    const row=document.createElement('div'); row.className='item';
    const left=document.createElement('div');
    const nm=document.createElement('div'); nm.className='nm'; nm.textContent=n.ssid;
    const meta=document.createElement('div'); meta.className='meta';
    meta.textContent=(n.secure?'🔒 ':'')+n.rssi+' dBm';
    left.appendChild(nm); left.appendChild(meta);
    const go=document.createElement('button'); go.className='btn'; go.textContent='Connect';
    go.onclick=()=>openSheet(n.ssid,n.secure,false);
    row.appendChild(left); row.appendChild(go);
    box.appendChild(row);
  });
}

async function scan(){
  let r=await call('/api/scan');
  if(!r.ok && r.status===409){
    if(!confirm('Scanning can briefly drop the current connection. Continue?')) return;
    r=await call('/api/scan?force=1');
  }
  setTimeout(loadWifi,1200);
  setTimeout(loadWifi,4000);
}

function openSheet(ssid,secure,manual){
  sheetSecure=secure;
  $('sh-title').textContent=manual?'Add a hidden network':'Connect to network';
  $('sh-ssid').value=ssid||'';
  $('sh-ssid').readOnly=!manual;
  $('sh-pass').value='';
  $('sh-passrow').hidden=!secure && !manual;
  $('sh-msg').textContent='';
  $('sh-msg').className='msg';
  $('sh-go').disabled=false;
  $('sh-go').textContent='Connect';
  $('sheet').hidden=false;
  if(!$('sh-passrow').hidden) setTimeout(()=>$('sh-pass').focus(),50);
}
function openManual(){ openSheet('',true,true); }
function closeSheet(){
  if(watchTimer){ clearInterval(watchTimer); watchTimer=null; }
  $('sheet').hidden=true;
  busy=false;
}

function eye(id,b){
  const p=$(id);
  p.type=p.type==='password'?'text':'password';
  b.textContent=p.type==='password'?'Show':'Hide';
}

async function doConnect(){
  if(busy) return;
  const ssid=$('sh-ssid').value.trim();
  const pass=$('sh-pass').value;
  const msg=$('sh-msg');
  if(!ssid){ msg.className='msg bad'; msg.textContent='Enter the network name.'; return; }
  if(!$('sh-passrow').hidden && pass.length && (pass.length<8 || pass.length>63)){
    msg.className='msg bad'; msg.textContent='Password must be 8 to 63 characters.'; return;
  }
  if(!$('sh-passrow').hidden && sheetSecure && !pass.length){
    msg.className='msg bad'; msg.textContent='This network needs a password.'; return;
  }
  busy=true;
  $('sh-go').disabled=true;
  msg.className='msg busy'; msg.textContent='Sending…';
  const r=await post('/api/wifi/connect',{ssid:ssid,pass:pass});
  if(!r.ok){ msg.className='msg bad'; msg.textContent=r.data.msg||'Error'; busy=false; $('sh-go').disabled=false; return; }
  msg.className='msg busy';
  msg.textContent='Connecting… If your phone drops the Nexus network, wait a few seconds and reconnect.';
  watchConnect();
}

function watchConnect(){
  if(watchTimer) clearInterval(watchTimer);
  let ticks=0;
  watchTimer=setInterval(async()=>{
    ticks++;
    const r=await call('/api/wifi');
    const msg=$('sh-msg');
    if(!r.ok){
      if(ticks>40){ clearInterval(watchTimer); watchTimer=null; busy=false; $('sh-go').disabled=false;
        msg.className='msg bad'; msg.textContent='Connection lost. Reconnect to Nexus and check the status.'; }
      return;
    }
    const d=r.data;
    if(d.state===2){
      clearInterval(watchTimer); watchTimer=null; busy=false;
      msg.className='msg ok'; msg.textContent=d.msg||'Connected';
      lastSaved=''; loadWifi();
      setTimeout(closeSheet,1400);
    } else if(d.state===3){
      clearInterval(watchTimer); watchTimer=null; busy=false; $('sh-go').disabled=false;
      msg.className='msg bad'; msg.textContent=d.msg||'Connection failed';
    } else if(ticks>40){
      clearInterval(watchTimer); watchTimer=null; busy=false; $('sh-go').disabled=false;
      msg.className='msg bad'; msg.textContent='Timed out. Try again.';
    }
  },1000);
}

async function forget(ssid){
  if(!confirm('Remove "'+ssid+'" from the list?')) return;
  await post('/api/wifi/forget',{ssid:ssid});
  lastSaved='';
  loadWifi();
}

async function loadDevice(){
  const r=await call('/api/device');
  if(!r.ok) return;
  const d=r.data;
  $('fw').textContent=d.fw||'—';
  $('lastpoll').textContent=d.lastPollAge>=0?(d.lastPollAge+' s ago'):'never';
  $('uptime').textContent=fmtUp(d.uptime);
  $('heap').textContent=Math.round((d.heap||0)/1024)+' KB';
  $('drops').textContent=(d.drops||0)+'';
  $('apname').textContent=d.apName||'Nexus';
}
function fmtUp(s){
  s=s||0; const h=Math.floor(s/3600), m=Math.floor((s%3600)/60);
  return h>0?(h+' h '+m+' min'):(m+' min');
}

async function savePass(){
  const p=$('newpass').value;
  const msg=$('passmsg');
  if(p.length<8 || p.length>63){ msg.className='msg bad'; msg.textContent='Password must be 8 to 63 characters.'; return; }
  const r=await post('/api/ap/pass',{pass:p});
  if(r.ok){ msg.className='msg ok'; msg.textContent='Saved. The hotspot restarts with the new password. Reconnect if you were connected.'; $('newpass').value=''; }
  else { msg.className='msg bad'; msg.textContent=r.data.msg||'Error'; }
}

async function apOff(){
  if(!confirm('Turn the hotspot off?')) return;
  await post('/api/ap/off',{});
}

loadState();
setInterval(loadState,2000);
setInterval(()=>{
  if(!$('pane-net').hidden) loadWifi();
  if(!$('pane-dev').hidden) loadDevice();
},3000);
</script></body></html>)HTML";

void onRoot() {
  touch();
  server.send(200, "text/html; charset=utf-8", PAGE);
}

void onState() {
  touch();
  JsonDocument d;
  d["ssid"] = WiFi.SSID();
  d["net"]  = netOkNow();

  JsonArray arr = d["relays"].to<JsonArray>();
  LOCK();
  for (uint8_t i = 0; i < relayCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]   = relays[i].id;
    o["name"] = relays[i].name;
    o["pin"]  = relays[i].pin;
    o["on"]   = relays[i].on;
  }
  UNLOCK();

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void onSet() {
  touch();
  String id = server.arg("id");
  bool on = server.arg("on") == "1";
  bool ok = false;

  if (id.length() > 0) {
    LOCK();
    int idx = findRelay(id.c_str());
    if (idx >= 0) {
      setRelayRaw(relays[idx], on);
      ok = true;
    }
    UNLOCK();
  }
  server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void onWifi() {
  touch();
  JsonDocument d;
  bool conn = WiFi.status() == WL_CONNECTED;
  String cur = conn ? WiFi.SSID() : String("");
  d["current"]  = cur;
  d["ip"]       = conn ? WiFi.localIP().toString() : String("");
  d["rssi"]     = conn ? WiFi.RSSI() : 0;
  d["scanning"] = (bool)scanPending;

  LOCK();
  d["state"] = (int)connState;
  d["msg"]   = connMsg;
  JsonArray sv = d["saved"].to<JsonArray>();
  for (uint8_t i = 0; i < savedCount; i++) {
    JsonObject o = sv.add<JsonObject>();
    o["ssid"]   = saved[i].ssid;
    o["prio"]   = (i == 0);
    o["active"] = conn && (cur == String(saved[i].ssid));
  }
  JsonArray nets = d["nets"].to<JsonArray>();
  for (uint8_t i = 0; i < scanCount; i++) {
    JsonObject o = nets.add<JsonObject>();
    o["ssid"]   = scanRes[i].ssid;
    o["rssi"]   = scanRes[i].rssi;
    o["secure"] = scanRes[i].secure;
  }
  UNLOCK();

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Scan request. While connected, the client must send force=1 (scanning can drop the link).
void onScan() {
  touch();
  if (WiFi.status() == WL_CONNECTED && server.arg("force") != "1") {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Scanning may drop the current connection\"}");
    return;
  }
  scanPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void onWifiConnect() {
  touch();
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (ssid.length() < 1 || ssid.length() > 32) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Network name must be 1 to 32 characters\"}");
    return;
  }
  if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Password must be 8 to 63 characters, or empty\"}");
    return;
  }

  WifiCred req;
  memset(&req, 0, sizeof(req));
  strlcpy(req.ssid, ssid.c_str(), sizeof(req.ssid));
  strlcpy(req.pass, pass.c_str(), sizeof(req.pass));

  LOCK();
  if (connPending || connState == CS_CONNECTING) {
    UNLOCK();
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Another connection is in progress\"}");
    return;
  }
  for (uint8_t i = 0; i < scanCount; i++) {
    if (strcmp(scanRes[i].ssid, req.ssid) == 0 && scanRes[i].ch > 0) {
      req.ch = scanRes[i].ch;
      memcpy(req.bssid, scanRes[i].bssid, 6);
      req.hasHint = true;
      break;
    }
  }
  connReq = req;
  connPending = true;
  connState = CS_CONNECTING;
  setMsgLocked("Connecting…");
  UNLOCK();

  server.send(200, "application/json", "{\"ok\":true}");
}

void onWifiForget() {
  touch();
  String ssid = server.arg("ssid");
  bool ok;
  LOCK();
  ok = removeSavedLocked(ssid.c_str());
  UNLOCK();
  server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void onDevice() {
  touch();
  JsonDocument d;
  d["fw"]          = "v1.0.0-beta";
  d["uptime"]      = millis() / 1000;
  d["heap"]        = ESP.getFreeHeap();
  d["drops"]       = wifiDrops;
  d["lastPollAge"] = everPolled ? (int32_t)((millis() - lastPollOkAt) / 1000) : -1;
  d["apName"]      = AP_SSID;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Change the hotspot password. The hotspot restarts in localTask (not inside this handler).
void onApPass() {
  touch();
  String p = server.arg("pass");
  if (p.length() < MIN_AP_PASS || p.length() > MAX_AP_PASS) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Password must be 8 to 63 characters\"}");
    return;
  }
  LOCK();
  strlcpy(apPass, p.c_str(), sizeof(apPass));
  prefs.putString("appass", p);
  UNLOCK();
  if (apOn) apRestartPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void onApOff() {
  touch();
  apOffPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void onNotFound() {
  server.send(404, "text/plain", "not found");
}

// LED: slow blink while there is no internet, solid when connected
void updateIndicator() {
  static uint32_t blinkAt = 0;
  static bool st = false;

  if (netOkNow()) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    return;
  }
  if ((uint32_t)(millis() - blinkAt) >= BLINK_MS) {
    blinkAt = millis();
    st = !st;
    digitalWrite(STATUS_LED_PIN, st ? HIGH : LOW);
  }
}

void localTask(void*) {
  for (;;) {
    handleButton();
    if (apOffPending) { apOffPending = false; apStop(); }
    if (apRestartPending) {
      apRestartPending = false;
      if (apOn) { apStop(); apStart(); }
    }
    handleApIdle();
    if (apOn) server.handleClient();
    updateIndicator();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================= setup and loop =================

void setup() {
  // First: relays OFF with the right pull, before anything else
  prefs.begin("nexus", false);
  loadConfig();

  Serial.begin(115200);
  Serial.println("boot: relays set OFF");

  mu = xSemaphoreCreateMutex();
  loadWifi();
  Serial.printf("saved wifi networks: %u\n", (unsigned)savedCount);

  strlcpy(apPass, prefs.getString("appass", AP_PASS_DEFAULT).c_str(), sizeof(apPass));

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Start in STA mode so an old hotspot is not raised at boot.
  // persistent(false): the SDK does not write Wi‑Fi data to flash (we store it ourselves).
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_country_code("EU", true);   // allow channels 1-13 (many routers use 12 or 13)
  WiFi.setAutoReconnect(false);            // we manage reconnects ourselves
  WiFi.setSleep(false);                    // no power save: more stable link
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  secureClient.setInsecure();              // see docs/known-limitations.md (TLS certificate check)

  server.on("/", HTTP_GET, onRoot);
  server.on("/api/state", HTTP_GET, onState);
  server.on("/api/set", HTTP_POST, onSet);
  server.on("/api/wifi", HTTP_GET, onWifi);
  server.on("/api/scan", HTTP_GET, onScan);
  server.on("/api/wifi/connect", HTTP_POST, onWifiConnect);
  server.on("/api/wifi/forget", HTTP_POST, onWifiForget);
  server.on("/api/device", HTTP_GET, onDevice);
  server.on("/api/ap/pass", HTTP_POST, onApPass);
  server.on("/api/ap/off", HTTP_POST, onApOff);
  server.onNotFound(onNotFound);

  xTaskCreatePinnedToCore(localTask, "local", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  if ((uint32_t)(millis() - hbAt) >= HEARTBEAT_MS) {
    hbAt = millis();
    bool wok = WiFi.status() == WL_CONNECTED;
    Serial.printf("hb: wifi=%s ip=%s ap=%s relays=%u cfg=%lu polls=%lu fails=%lu drops=%lu\n",
                  wok ? "yes" : "no",
                  wok ? WiFi.localIP().toString().c_str() : "-",
                  apOn ? "on" : "off",
                  (unsigned)relayCount,
                  (unsigned long)cfgVer,
                  (unsigned long)pollCount,
                  (unsigned long)failCount,
                  (unsigned long)wifiDrops);
  }

  bool conn = WiFi.status() == WL_CONNECTED;
  if (conn && !wasConn) {
    lastPollOkAt = millis();   // start the watchdog from now
    wifiFails = 0;
  }
  if (!conn && wasConn) {
    wifiDrops++;
    Serial.println("wifi: link lost, reconnecting now");
    nextWifiAt = 0;
  }
  wasConn = conn;

  // Scans and connects run only here, never in the web task
  if (scanPending) doScan();
  if (connPending) handleConnRequest();
  conn = WiFi.status() == WL_CONNECTED;

  if (!conn) {
    if ((int32_t)(millis() - nextWifiAt) >= 0) {
      Serial.println("WiFi connecting...");
      if (connectSaved()) {
        Serial.println("WiFi OK, IP " + WiFi.localIP().toString());
        wifiFails = 0;
        nextWifiAt = 0;
      } else {
        if (wifiFails < 10) wifiFails++;
        uint32_t back = 2000UL << (wifiFails > 4 ? 4 : wifiFails);   // 2s, 4s, 8s, 16s, 32s
        if (back > 30000UL) back = 30000UL;
        nextWifiAt = millis() + back;
        Serial.printf("WiFi failed, retry in %lus\n", (unsigned long)(back / 1000));
      }
    }
    delay(100);
    return;
  }

  // Watchdog: connected to Wi‑Fi, but the server has not answered for a long time
  if (everPolled && (uint32_t)(millis() - lastPollOkAt) > WATCHDOG_MS) {
    Serial.println("watchdog: no server reply, restarting the Wi‑Fi link");
    wifiDrops++;
    lastPollOkAt = millis();
    WiFi.disconnect(false, false);
    nextWifiAt = 0;
    delay(100);
    return;
  }

  if ((int32_t)(millis() - nextPollAt) >= 0) {
    bool ok = doPoll();
    nextPollAt = millis() + (ok ? 0 : RETRY_MS);
  }
  delay(5);
}
