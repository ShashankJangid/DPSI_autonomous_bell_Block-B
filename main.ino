/*
 * ============================================================
 *  DPSI AUTONOMOUS BELL — V31.0 With AI
 *  Platform : ESP32
 *  Designed & Developed by Ai Lab [Shashank Jangid]
 * ============================================================
 */


#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <WiFiManager.h>
#include "time.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"




// ── Credentials & config ──────────────────────────────────────────────────────
const char* DEFAULT_SSID   = "Dpsi-Bell";
const char* DEFAULT_PASS   = "admindps";
const char* AP_SSID        = "Bell_Configuration(12345678)";
const char* AP_PASS        = "12345678";
const char* WEB_USER       = "admin";
const char* WEB_PASS       = "admindps";
const char* NTP_SERVER_1   = "pool.ntp.org";
const char* NTP_SERVER_2   = "time.google.com";
const long  GMT_OFFSET_SEC = 19800;   // IST = UTC+5:30
const char* HOSTNAME       = "dpsi-bell";




// ── Pin & timing constants ────────────────────────────────────────────────────
#define RELAY_PIN             26
#define BUTTON_PIN             4
#define WDT_TIMEOUT_SEC       45
#define IP_DISPLAY_MS      30000


// Button — tuned to prevent ghost triggers
#define BTN_DEBOUNCE_MS      50     // must be LOW this long to count as press
#define BTN_COOLDOWN_MS     500     // ignore button for this long after a ring
#define BTN_LONG_PRESS_MS  5000


#define MAX_BELLS             30
#define MAX_HISTORY           20
#define RING_RATE_LIMIT_MS  2000
#define MAX_BODY_BYTES      4096
#define WIFI_CHECK_MS      30000    // check WiFi every 30 s


// ── Offline time constants ────────────────────────────────────────────────────
#define NTP_RESYNC_INTERVAL_MS  (6UL * 3600UL * 1000UL)
#define TIME_SAVE_INTERVAL_MS   (60UL * 1000UL)
#define SAVED_TIME_FILE         "/lasttime.txt"




// ── Peripherals ───────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);




// ── Schedule state ────────────────────────────────────────────────────────────
DynamicJsonDocument scheduleDoc(2048);
JsonArray           scheduleCache;
String              scheduleJson = "[]";




// ── History state ─────────────────────────────────────────────────────────────
DynamicJsonDocument historyDoc(4096);
String              historyJson  = "[]";




// ── Runtime flags ─────────────────────────────────────────────────────────────
bool bellActive   = false;
bool holidayMode  = false;
bool ntpSynced    = false;
bool showIP       = true;




// ── Bell timing (protected — nothing should touch these except triggerBell/loop) ─
unsigned long bellStartMs      = 0;
unsigned long bellDurationMs   = 0;


// ── Other timers ──────────────────────────────────────────────────────────────
unsigned long wifiConnectedMs  = 0;
unsigned long lastRingMs       = 0;
unsigned long lastWifiCheckMs  = 0;
unsigned long lastNtpSyncMs    = 0;
unsigned long lastTimeSaveMs   = 0;


// ── Button state machine ──────────────────────────────────────────────────────
// We track raw LOW transitions, not rely on delay() or bellActive flag
enum BtnState { BTN_IDLE, BTN_DEBOUNCING, BTN_HELD, BTN_WAIT_RELEASE };
BtnState      btnState       = BTN_IDLE;
unsigned long btnStateMs     = 0;   // when we entered current state
unsigned long btnCooldownMs  = 0;   // absolute time after which button is re-armed


// ── WiFi reconnect state (non-blocking) ───────────────────────────────────────
bool          wifiReconnecting  = false;
unsigned long wifiReconnectMs   = 0;
int           wifiFailCount     = 0;
#define WIFI_RETRY_MAX          5
#define WIFI_RECONNECT_WAIT_MS  12000   // wait up to 12 s for reconnect before giving up


// ── Schedule guard ────────────────────────────────────────────────────────────
String lastTriggeredMin = "";




// ── Forward declarations ──────────────────────────────────────────────────────
struct NextBellInfo { String timeStr; String label; int minutesLeft; };
NextBellInfo getNextBell(struct tm ti);
bool         requireAuth();
void         triggerBell(int durSec, const String& source);
void         saveHistory();
void         logEvent(const String& msg);
String       lcdPad(String s, int len = 16);
void         saveCurrentTime();
bool         loadSavedTime();
void         checkNtp();
void         handleButton();
void         checkWifi();




// ═══════════════════════════════════════════════════════════════════════════════
//  WATCHDOG
// ═══════════════════════════════════════════════════════════════════════════════


void wdtInit() {
  esp_task_wdt_deinit();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { .timeout_ms = (uint32_t)WDT_TIMEOUT_SEC * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
}


void wdtReinit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { .timeout_ms = (uint32_t)WDT_TIMEOUT_SEC * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
}




// ═══════════════════════════════════════════════════════════════════════════════
//  OFFLINE TIME PERSISTENCE
// ═══════════════════════════════════════════════════════════════════════════════


void saveCurrentTime() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%04d %02d %02d %02d %02d %02d %d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_wday);
  File f = LittleFS.open(SAVED_TIME_FILE, "w");
  if (f) { f.print(buf); f.close(); }
}


bool loadSavedTime() {
  if (!LittleFS.exists(SAVED_TIME_FILE)) return false;
  File f = LittleFS.open(SAVED_TIME_FILE, "r");
  if (!f) return false;
  String raw = f.readString(); f.close();
  raw.trim();
  if (raw.length() < 10) return false;


  int yr, mo, dy, hr, mn, sc, wd;
  if (sscanf(raw.c_str(), "%d %d %d %d %d %d %d", &yr, &mo, &dy, &hr, &mn, &sc, &wd) != 7) return false;
  if (yr < 2024) return false;


  struct tm ti = {};
  ti.tm_year  = yr - 1900; ti.tm_mon   = mo - 1; ti.tm_mday  = dy;
  ti.tm_hour  = hr;        ti.tm_min   = mn;      ti.tm_sec   = sc;
  ti.tm_wday  = wd;        ti.tm_isdst = 0;


  time_t t = mktime(&ti);
  if (t < 0) return false;
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  logEvent("Loaded saved time: " + raw);
  return true;
}




// ═══════════════════════════════════════════════════════════════════════════════
//  NTP SYNC  (never blocks longer than 5 s — only called when WiFi is up)
// ═══════════════════════════════════════════════════════════════════════════════


void checkNtp() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(GMT_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
  struct tm ti;
  if (getLocalTime(&ti, 5000)) {
    ntpSynced     = true;
    lastNtpSyncMs = millis();
    saveCurrentTime();
    logEvent("NTP sync OK");
  } else {
    logEvent("NTP sync failed — using saved/RTC time");
  }
}




// ═══════════════════════════════════════════════════════════════════════════════
//  NON-BLOCKING WIFI WATCHDOG
//  Called every loop() iteration — NEVER uses delay() or blocks.
//  State machine:
//    IDLE        → wifiFailCount tracks failures over time
//    RECONNECTING → WiFi.reconnect() called; we wait WIFI_RECONNECT_WAIT_MS
//                   feeding the WDT, then accept success or failure and move on.
// ═══════════════════════════════════════════════════════════════════════════════


void checkWifi() {
  unsigned long cm = millis();


  // ── If we are mid-reconnect, check if it succeeded or timed out ──────────
  if (wifiReconnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiReconnecting = false;
      wifiFailCount    = 0;
      logEvent("WiFi reconnected! IP: " + WiFi.localIP().toString());
      checkNtp();
      return;
    }
    if (cm - wifiReconnectMs >= WIFI_RECONNECT_WAIT_MS) {
      // Timed out — give up this attempt, schedule next check
      wifiReconnecting  = false;
      lastWifiCheckMs   = cm;   // reset interval so we don't immediately retry
      wifiFailCount++;
      if (wifiFailCount >= WIFI_RETRY_MAX) {
        logEvent("WiFi unavailable after " + String(WIFI_RETRY_MAX) +
                 " attempts — running offline. Bell schedule continues.");
        wifiFailCount = 0;
      } else {
        logEvent("WiFi reconnect attempt timed out (" + String(wifiFailCount) + "/" + String(WIFI_RETRY_MAX) + ")");
      }
    }
    // Still waiting — return immediately, DO NOT BLOCK
    return;
  }


  // ── Periodic check every WIFI_CHECK_MS ───────────────────────────────────
  if (cm - lastWifiCheckMs < WIFI_CHECK_MS) return;
  lastWifiCheckMs = cm;


  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0;
    return;
  }


  // WiFi is down — start a non-blocking reconnect attempt
  logEvent("WiFi lost! Starting reconnect attempt " + String(wifiFailCount + 1) + "/" + String(WIFI_RETRY_MAX));
  WiFi.disconnect(false);
  delay(20);  // tiny settle — does NOT affect bell timing at 20 ms
  WiFi.reconnect();
  wifiReconnecting = true;
  wifiReconnectMs  = cm;
}




// ═══════════════════════════════════════════════════════════════════════════════
//  BUTTON STATE MACHINE  (replaces delay-based debounce)
//
//  States:
//  BTN_IDLE        → waiting for first LOW
//  BTN_DEBOUNCING  → saw LOW, waiting BTN_DEBOUNCE_MS to confirm it's real
//  BTN_HELD        → confirmed press; fire short ring on release OR long-press portal
//  BTN_WAIT_RELEASE→ ring fired; wait for button to go HIGH before re-arming
// ═══════════════════════════════════════════════════════════════════════════════


void handleButton() {
  unsigned long cm   = millis();
  bool          pinLow = (digitalRead(BUTTON_PIN) == LOW);


  // Honour cooldown — if the bell was just triggered, ignore button completely
  if (cm < btnCooldownMs) return;


  switch (btnState) {


    case BTN_IDLE:
      if (pinLow) {
        btnState   = BTN_DEBOUNCING;
        btnStateMs = cm;
      }
      break;


    case BTN_DEBOUNCING:
      if (!pinLow) {
        // Bounced — reset
        btnState = BTN_IDLE;
      } else if (cm - btnStateMs >= BTN_DEBOUNCE_MS) {
        // Stable LOW confirmed → real press
        btnState   = BTN_HELD;
        btnStateMs = cm;
      }
      break;


    case BTN_HELD:
      if (!pinLow) {
        // Released before long-press threshold → short press → ring 3 s
        triggerBell(3, "PHYSICAL_BUTTON");
        btnCooldownMs = cm + BTN_COOLDOWN_MS;
        btnState      = BTN_WAIT_RELEASE;
      } else if (cm - btnStateMs >= BTN_LONG_PRESS_MS) {
        // Long press → open WiFi config portal
        logEvent("Long press → WiFi config portal");
        digitalWrite(RELAY_PIN, LOW); bellActive = false;
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("WiFi Config     ");
        lcd.setCursor(0, 1); lcd.print("Connect to AP.. ");
        WiFiManager wm;
        wm.startConfigPortal(AP_SSID, AP_PASS);
        btnState      = BTN_WAIT_RELEASE;
        btnCooldownMs = millis() + BTN_COOLDOWN_MS;
      }
      break;


    case BTN_WAIT_RELEASE:
      if (!pinLow) btnState = BTN_IDLE;
      break;
  }
}




// ═══════════════════════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════════════════════


String lcdPad(String s, int len) {
  while ((int)s.length() < len) s += ' ';
  return s.substring(0, len);
}


void logEvent(const String& msg) {
  struct tm ti;
  if (getLocalTime(&ti))
    Serial.printf("[%02d:%02d:%02d] %s\n", ti.tm_hour, ti.tm_min, ti.tm_sec, msg.c_str());
  else
    Serial.println("[SYS] " + msg);
}


bool requireAuth() {
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
    server.requestAuthentication(); return false;
  }
  return true;
}


void saveHistory() {
  File f = LittleFS.open("/history.json", "w");
  if (f) { serializeJson(historyDoc, f); f.close(); }
}


// ── triggerBell: records start time FIRST, then activates relay ──────────────
// This means even if there's a tiny delay from a web handler, the duration
// is calculated from when we set bellStartMs — not from relay activation.
void triggerBell(int durSec, const String& source) {
  unsigned long cm = millis();
  if (cm - lastRingMs < RING_RATE_LIMIT_MS) {
    logEvent("Ring rate-limited (too soon)");
    return;
  }
  lastRingMs = cm;
  if (durSec < 1)  durSec = 1;
  if (durSec > 60) durSec = 60;


  // Record time BEFORE relay so duration is always accurate
  bellStartMs    = cm;
  bellDurationMs = (unsigned long)durSec * 1000UL;
  bellActive     = true;
  digitalWrite(RELAY_PIN, HIGH);


  struct tm ti;
  if (getLocalTime(&ti)) {
    char ts[20];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d",
            ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
    JsonObject entry = historyDoc.createNestedObject();
    entry["time"]   = String(ts);
    entry["source"] = source;
    entry["dur"]    = durSec;
    while ((int)historyDoc.size() > MAX_HISTORY) historyDoc.remove(0);
    saveHistory();
    serializeJson(historyDoc, historyJson);
  }
  logEvent("RING START | src=" + source + " | dur=" + String(durSec) + "s");
}


NextBellInfo getNextBell(struct tm ti) {
  NextBellInfo res = { "--:--", "", 9999 };
  if (scheduleCache.isNull() || scheduleCache.size() == 0) return res;
  int curMins = ti.tm_hour * 60 + ti.tm_min;
  int dayIdx  = (ti.tm_wday == 0) ? 6 : ti.tm_wday - 1;


  for (int ahead = 0; ahead < 7; ahead++) {
    int scanDay   = (dayIdx + ahead) % 7;
    int minOffset = ahead * 24 * 60;
    int bestMins  = 9999;
    String bestTime = "";
    for (JsonObject item : scheduleCache) {
      int dayEnabled = item["d"][scanDay].as<int>();
      if (dayEnabled != 1) continue;
      String t = item["t"].as<String>();
      if (t.length() == 4) t = "0" + t;
      int bellMins = t.substring(0, 2).toInt() * 60 + t.substring(3).toInt();
      if (ahead == 0 && bellMins <= curMins) continue;
      if (bellMins < bestMins) { bestMins = bellMins; bestTime = t; }
    }
    if (bestTime.length() > 0) {
      res.timeStr     = bestTime;
      res.label       = (ahead == 0) ? "today" : (ahead == 1 ? "tomorrow" : "in " + String(ahead) + " days");
      res.minutesLeft = (minOffset - curMins) + bestMins;
      return res;
    }
  }
  return res;
}




// ═══════════════════════════════════════════════════════════════════════════════
//  HTML DASHBOARD  (identical to V30 — no changes needed)
// ═══════════════════════════════════════════════════════════════════════════════


const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DPSI Autonomous Bell</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=Syne:wght@700;800&display=swap" rel="stylesheet">
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.1/css/all.min.css">
<style>
:root {
  --bg:#07090f; --accent:#f5c518; --accent2:#3b82f6;
  --danger:#ef4444; --success:#22c55e; --text:#e2e8f0; --muted:#64748b;
  --radius:14px; --r8:8px;
  --gb:rgba(255,255,255,0.04); --gborder:rgba(255,255,255,0.10);
  --gblur:18px; --gshadow:0 8px 32px rgba(0,0,0,0.45);
}
*{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:'Space Mono',monospace;color:var(--text);
  min-height:100vh;padding:0 0 60px;background:var(--bg);
  background-image:
    radial-gradient(ellipse 60% 50% at 15% 20%,rgba(245,197,24,.13) 0%,transparent 70%),
    radial-gradient(ellipse 50% 60% at 85% 75%,rgba(59,130,246,.11) 0%,transparent 70%),
    radial-gradient(ellipse 40% 40% at 50% 50%,rgba(34,197,94,.07) 0%,transparent 70%),
    radial-gradient(ellipse 70% 30% at 80% 10%,rgba(245,197,24,.06) 0%,transparent 70%);
}
header{
  background:rgba(7,9,15,0.6);
  backdrop-filter:blur(24px) saturate(180%);
  -webkit-backdrop-filter:blur(24px) saturate(180%);
  border-bottom:1px solid var(--gborder);
  padding:14px 28px;
  display:flex;align-items:center;justify-content:space-between;
  position:sticky;top:0;z-index:100;
}
.logo{display:flex;align-items:center;gap:10px}
.logo i{color:var(--accent);font-size:1.5rem;filter:drop-shadow(0 0 8px rgba(245,197,24,.5))}
.logo h1{font-family:'Syne',sans-serif;font-size:1.3rem;letter-spacing:.06em}
.logo span{color:var(--accent)}
#statusPill{
  display:flex;align-items:center;gap:8px;
  background:rgba(255,255,255,0.05);
  backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);
  border:1px solid var(--gborder);border-radius:99px;
  padding:5px 14px;font-size:.75rem;color:var(--muted);
}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;flex-shrink:0}
.dot.green{background:var(--success);box-shadow:0 0 8px var(--success)}
.dot.red{background:var(--danger);box-shadow:0 0 8px var(--danger)}
.dot.orange{background:#f97316;box-shadow:0 0 8px #f97316}
.wrap{max-width:960px;margin:0 auto;padding:28px 18px}
.top-row{display:flex;gap:20px;align-items:stretch;margin-bottom:20px}
.clock-panel{
  flex:1 1 auto;border-radius:var(--radius);padding:28px 32px;
  background:rgba(255,255,255,0.04);
  backdrop-filter:blur(var(--gblur)) saturate(160%);
  -webkit-backdrop-filter:blur(var(--gblur)) saturate(160%);
  border:1px solid var(--gborder);
  box-shadow:var(--gshadow),inset 0 1px 0 rgba(255,255,255,0.07);
}
.next-box{
  flex:0 0 220px;border-radius:var(--radius);padding:18px 22px;
  display:flex;flex-direction:column;justify-content:center;
  background:rgba(245,197,24,0.06);
  backdrop-filter:blur(var(--gblur)) saturate(180%);
  -webkit-backdrop-filter:blur(var(--gblur)) saturate(180%);
  border:1px solid rgba(245,197,24,.22);
  box-shadow:0 8px 32px rgba(0,0,0,.4),inset 0 1px 0 rgba(245,197,24,.12);
}
@media(max-width:640px){.top-row{flex-direction:column}.next-box{flex:none;width:100%}}
#clock{
  font-family:'Syne',sans-serif;
  font-size:clamp(1.5rem,9vw,5rem);
  font-weight:800;line-height:1;
  color:var(--accent);letter-spacing:-.02em;
  text-shadow:0 0 40px rgba(245,197,24,.35);
}
#clock .sep{animation:blink 1s step-end infinite;opacity:.6}
@keyframes blink{0%,100%{opacity:.6}50%{opacity:0}}
#date{color:var(--muted);font-size:.82rem;margin-top:6px}
.next-label{font-size:.68rem;color:var(--muted);text-transform:uppercase;letter-spacing:.12em;margin-bottom:5px}
#nextBellTime{
  font-family:'Syne',sans-serif;font-size:2.5rem;font-weight:800;
  color:var(--accent);text-shadow:0 0 28px rgba(245,197,24,.4);
}
#countdown{font-size:.82rem;color:var(--muted);margin-top:3px}
#nextBellLabel{font-size:.72rem;color:var(--muted);margin-top:1px}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:20px}
@media(max-width:500px){.actions{grid-template-columns:1fr}}
.btn-ring{
  background:var(--accent);color:#000;border:none;
  border-radius:var(--radius);padding:18px 22px;
  font-family:'Syne',sans-serif;font-size:1rem;font-weight:800;
  cursor:pointer;display:flex;align-items:center;justify-content:center;gap:9px;
  box-shadow:0 4px 24px rgba(245,197,24,.35);
  transition:filter .15s,transform .1s,box-shadow .15s;min-height:44px;
}
.btn-ring:hover{filter:brightness(1.12);box-shadow:0 6px 32px rgba(245,197,24,.5)}
.btn-ring:active{transform:scale(.97)}
.btn-ring.ringing{background:#ff6b35;color:#fff;animation:pulse .6s ease-in-out infinite alternate}
@keyframes pulse{from{box-shadow:0 4px 20px rgba(255,107,53,.4)}to{box-shadow:0 4px 36px rgba(255,107,53,.8)}}
.holiday-box{
  border-radius:var(--radius);padding:18px 22px;
  display:flex;align-items:center;justify-content:space-between;
  background:rgba(255,255,255,0.04);
  backdrop-filter:blur(var(--gblur)) saturate(160%);
  -webkit-backdrop-filter:blur(var(--gblur)) saturate(160%);
  border:1px solid var(--gborder);box-shadow:var(--gshadow);
}
.holiday-box span{font-size:.9rem}
.toggle{position:relative;display:inline-block;width:50px;height:26px}
.toggle input{opacity:0;width:0;height:0}
.slider{
  position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;
  background:rgba(255,255,255,0.1);backdrop-filter:blur(4px);
  border-radius:34px;transition:.3s;border:1px solid rgba(255,255,255,0.12);
}
.slider:before{
  position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;
  background:#fff;border-radius:50%;transition:.3s;box-shadow:0 2px 8px rgba(0,0,0,.4);
}
input:checked+.slider{background:rgba(34,197,94,.35);border-color:rgba(34,197,94,.5)}
input:checked+.slider:before{transform:translateX(24px)}
.card{
  border-radius:var(--radius);overflow:hidden;
  background:rgba(255,255,255,0.04);
  backdrop-filter:blur(var(--gblur)) saturate(160%);
  -webkit-backdrop-filter:blur(var(--gblur)) saturate(160%);
  border:1px solid var(--gborder);
  box-shadow:var(--gshadow),inset 0 1px 0 rgba(255,255,255,0.06);
}
.tabs{display:flex;border-bottom:1px solid var(--gborder);overflow-x:auto}
.tab-btn{
  background:none;border:none;color:var(--muted);
  font-family:'Space Mono',monospace;font-size:.82rem;
  padding:14px 24px;cursor:pointer;white-space:nowrap;
  border-bottom:2px solid transparent;
  transition:color .2s,border-color .2s;min-height:44px;
}
.tab-btn.active{color:var(--accent);border-bottom-color:var(--accent)}
.tab-content{padding:22px;display:none}
.tab-content.active{display:block}
#bellList{display:flex;flex-direction:column;gap:10px}
.bell-row{
  border-radius:var(--radius);padding:14px 18px;
  background:rgba(255,255,255,0.035);
  backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);
  border:1px solid rgba(255,255,255,0.08);
  box-shadow:0 2px 12px rgba(0,0,0,.25);
  transition:border-color .2s,box-shadow .2s,opacity .3s,transform .3s;
  animation:slideIn .25s ease;
}
@keyframes slideIn{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:translateY(0)}}
.bell-row.removing{opacity:0;transform:translateY(-6px)}
.bell-row:hover{border-color:rgba(255,255,255,0.14);box-shadow:0 4px 20px rgba(0,0,0,.35)}
.bell-row-top{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
input[type="time"]{
  background:rgba(255,255,255,0.06);backdrop-filter:blur(8px);
  border:1px solid rgba(255,255,255,0.10);border-radius:var(--r8);
  color:var(--text);font-family:'Space Mono',monospace;font-size:1.05rem;
  padding:7px 12px;outline:none;min-width:120px;
  transition:border-color .2s,box-shadow .2s;
}
input[type="time"]:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(245,197,24,.15)}
input[type="number"]{
  background:rgba(255,255,255,0.06);backdrop-filter:blur(8px);
  border:1px solid rgba(255,255,255,0.10);border-radius:var(--r8);
  color:var(--text);font-family:'Space Mono',monospace;font-size:.95rem;
  padding:7px 8px;width:66px;text-align:center;outline:none;
  transition:border-color .2s,box-shadow .2s;
}
input[type="number"]:focus{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(59,130,246,.15)}
.dur-wrap{display:flex;align-items:center;gap:5px}
.dur-wrap label{font-size:.72rem;color:var(--muted)}
.btn-del{
  margin-left:auto;background:rgba(239,68,68,.07);backdrop-filter:blur(6px);
  border:1px solid rgba(239,68,68,.25);color:var(--danger);
  border-radius:var(--r8);width:36px;height:36px;
  cursor:pointer;font-size:.95rem;
  display:flex;align-items:center;justify-content:center;
  transition:background .2s,border-color .2s;flex-shrink:0;
}
.btn-del:hover{background:rgba(239,68,68,.18);border-color:rgba(239,68,68,.5)}
.days-row{display:flex;gap:7px;margin-top:10px;flex-wrap:wrap}
.day-chip{display:flex;flex-direction:column;align-items:center;gap:2px;font-size:.62rem;color:var(--muted)}
.day-chip input[type="checkbox"]{display:none}
.day-chip span{
  width:32px;height:32px;background:rgba(255,255,255,0.04);
  border:1px solid rgba(255,255,255,0.09);border-radius:7px;
  display:flex;align-items:center;justify-content:center;
  cursor:pointer;font-size:.72rem;font-weight:700;
  transition:background .15s,border-color .15s,color .15s,box-shadow .15s;
  user-select:none;
}
.day-chip input:checked~span{
  background:rgba(245,197,24,.15);border-color:rgba(245,197,24,.5);
  color:var(--accent);box-shadow:0 0 10px rgba(245,197,24,.2);
}
.sched-btns{display:flex;gap:10px;margin-top:18px;flex-wrap:wrap}
.btn-add{
  flex:1;min-width:120px;
  background:rgba(255,255,255,0.06);backdrop-filter:blur(8px);
  border:1px solid var(--gborder);color:var(--text);
  border-radius:var(--radius);padding:13px;
  font-family:'Space Mono',monospace;font-size:.85rem;
  cursor:pointer;transition:background .2s,border-color .2s;min-height:44px;
}
.btn-add:hover{background:rgba(255,255,255,0.10);border-color:rgba(255,255,255,0.18)}
.btn-save{
  flex:1;min-width:auto;
  background:rgba(34,197,94,.25);backdrop-filter:blur(8px);
  border:1px solid rgba(34,197,94,.4);color:#86efac;
  border-radius:var(--radius);padding:13px;
  font-family:'Syne',sans-serif;font-size:.95rem;font-weight:800;
  cursor:pointer;box-shadow:0 4px 20px rgba(34,197,94,.2);
  transition:filter .15s,transform .1s,box-shadow .15s;min-height:44px;
  display:flex;align-items:center;justify-content:center;gap:8px;
}
.btn-save:hover{filter:brightness(1.15);box-shadow:0 6px 28px rgba(34,197,94,.35)}
.btn-save:active{transform:scale(.97)}
.btn-save.saving{opacity:.7;pointer-events:none}
#dirtyBadge{
  display:none;background:rgba(245,197,24,.1);backdrop-filter:blur(8px);
  border:1px solid rgba(245,197,24,.3);color:var(--accent);font-size:.68rem;
  padding:4px 11px;border-radius:99px;margin-bottom:10px;
}
#dirtyBadge.show{display:inline-block}
#historyTable{width:100%;border-collapse:collapse;font-size:.8rem}
#historyTable th{
  text-align:left;padding:9px 11px;color:var(--muted);font-weight:400;
  border-bottom:1px solid var(--gborder);font-size:.7rem;
  text-transform:uppercase;letter-spacing:.1em;
}
#historyTable td{padding:11px;border-bottom:1px solid rgba(255,255,255,0.05)}
#historyTable tr:last-child td{border-bottom:none}
#historyTable tr:hover td{background:rgba(255,255,255,0.03)}
.src-chip{display:inline-flex;align-items:center;gap:5px;padding:2px 9px;border-radius:99px;font-size:.7rem}
.src-sched{background:rgba(59,130,246,.15);color:#7dd3fc}
.src-web{background:rgba(245,197,24,.15);color:var(--accent)}
.src-btn{background:rgba(34,197,94,.15);color:#86efac}
.src-test{background:rgba(168,85,247,.15);color:#d8b4fe}
#chatMessages{
  height:340px;overflow-y:auto;
  border:1px solid var(--gborder);border-radius:var(--radius);
  padding:16px;margin-bottom:14px;
  display:flex;flex-direction:column;gap:12px;
  scrollbar-width:thin;scrollbar-color:var(--gborder) transparent;
  background:rgba(0,0,0,.15);
}
#chatMessages::-webkit-scrollbar{width:4px}
#chatMessages::-webkit-scrollbar-thumb{background:var(--gborder);border-radius:2px}
.msg{max-width:84%;padding:10px 14px;border-radius:12px;font-size:.82rem;line-height:1.55;word-break:break-word;white-space:pre-wrap}
.msg.user{align-self:flex-end;background:rgba(245,197,24,.14);border:1px solid rgba(245,197,24,.28);color:var(--text);border-bottom-right-radius:3px}
.msg.ai{align-self:flex-start;background:rgba(59,130,246,.10);border:1px solid rgba(59,130,246,.22);color:var(--text);border-bottom-left-radius:3px}
.msg.err{align-self:flex-start;background:rgba(239,68,68,.10);border:1px solid rgba(239,68,68,.22);color:#fca5a5;border-bottom-left-radius:3px}
.msg-meta{font-size:.62rem;color:var(--muted);margin-top:5px}
.typing-dots{display:flex;align-items:center;gap:5px;padding:4px 0}
.typing-dots span{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--accent2);animation:tdot .9s infinite}
.typing-dots span:nth-child(2){animation-delay:.18s}
.typing-dots span:nth-child(3){animation-delay:.36s}
@keyframes tdot{0%,80%,100%{transform:scale(.65);opacity:.4}40%{transform:scale(1);opacity:1}}
.chat-input-row{display:flex;gap:10px}
#chatInput{flex:1;background:rgba(255,255,255,0.06);backdrop-filter:blur(8px);border:1px solid var(--gborder);border-radius:var(--r8);color:var(--text);font-family:'Space Mono',monospace;font-size:.85rem;padding:10px 14px;outline:none;min-height:44px;transition:border-color .2s,box-shadow .2s}
#chatInput:focus{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(59,130,246,.15)}
.btn-send{background:rgba(59,130,246,.22);border:1px solid rgba(59,130,246,.38);color:#93c5fd;border-radius:var(--r8);padding:8px 15px;font-family:'Syne',sans-serif;font-weight:800;font-size:.9rem;cursor:pointer;transition:filter .15s,transform .1s;white-space:nowrap;min-height:44px;display:flex;align-items:center;gap:7px;flex-direction:row-reverse}
.btn-send:hover{filter:brightness(1.2)}
.btn-send:active{transform:scale(.97)}
.btn-send:disabled{opacity:.45;pointer-events:none}
.ai-notice{font-size:.68rem;color:var(--muted);margin-top:9px;text-align:center;line-height:1.5}
#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);backdrop-filter:blur(16px);padding:11px 26px;border-radius:99px;font-family:'Syne',sans-serif;font-weight:700;font-size:.9rem;transition:transform .35s cubic-bezier(.34,1.56,.64,1);z-index:9999;pointer-events:none;background:rgba(34,197,94,.25);border:1px solid rgba(34,197,94,.45);color:#86efac;box-shadow:0 8px 32px rgba(34,197,94,.25)}
#toast.show{transform:translateX(-50%) translateY(0)}
#toast.err{background:rgba(239,68,68,.25);border-color:rgba(239,68,68,.45);color:#fca5a5;box-shadow:0 8px 32px rgba(239,68,68,.2)}
footer{text-align:center;color:var(--muted);font-size:.68rem;margin-top:36px;line-height:1.9}
footer span{color:var(--accent)}
</style>
</head>
<body>


<header>
  <div class="logo">
    <i class="fa-solid fa-bell"></i>
    <h1>DPSI AUTO<span>NOMOUS BELL</span></h1>
  </div>
  <div id="statusPill">
    <span class="dot red" id="ntpDot"></span>
    <span id="statusText">Connecting…</span>
  </div>
</header>


<div class="wrap">
  <div class="top-row">
    <div class="clock-panel">
      <div id="clock">--<span class="sep">:</span>--<span class="sep">:</span>--</div>
      <div id="date">…</div>
    </div>
    <div class="next-box">
      <div class="next-label"><i class="fa-solid fa-clock-rotate-left" style="margin-right:4px"></i>Next Bell</div>
      <div id="nextBellTime">--:--</div>
      <div id="countdown"></div>
      <div id="nextBellLabel"></div>
    </div>
  </div>


  <div class="actions">
    <button class="btn-ring" id="ringBtn" onclick="ringNow()">
      <i class="fa-solid fa-bell"></i> INSTANT RING
    </button>
    <div class="holiday-box">
      <span><i class="fa-solid fa-umbrella-beach" style="margin-right:7px;color:var(--muted)"></i>Holiday Mode</span>
      <label class="toggle">
        <input type="checkbox" id="holidayToggle" onchange="toggleHoliday()">
        <span class="slider"></span>
      </label>
    </div>
  </div>


  <div class="card">
    <div class="tabs">
      <button class="tab-btn active" onclick="switchTab('sched',this)">SCHEDULE</button>
      <button class="tab-btn" onclick="switchTab('hist',this)">HISTORY</button>
      <button class="tab-btn" onclick="switchTab('ai',this)">AI ASSISTANT</button>
    </div>


    <div id="tab-sched" class="tab-content active">
      <div id="dirtyBadge">⚠ Unsaved changes — auto-refresh paused</div>
      <div id="bellList"></div>
      <div class="sched-btns">
        <button class="btn-add" onclick="addRow()">+ ADD BELL</button>
        <button class="btn-save" id="saveBtn" onclick="save()">
          <i class="fa-solid fa-floppy-disk"></i> SAVE SCHEDULE
        </button>
      </div>
    </div>


    <div id="tab-hist" class="tab-content">
      <div style="overflow-x:auto">
        <table id="historyTable">
          <thead>
            <tr>
              <th>Date / Time</th>
              <th>Source</th>
              <th style="text-align:right">Duration</th>
            </tr>
          </thead>
          <tbody id="historyBody"></tbody>
        </table>
      </div>
    </div>


    <div id="tab-ai" class="tab-content">
      <div id="chatMessages">
        <div class="msg ai">👋 Hi! I'm your DPSI Bell AI Assistant, powered by Ai Lab. I know everything about this project — hardware, schedule, API, OTA, WiFi, history, holiday mode — ask me anything!
          <div class="msg-meta">DPSI BELL AI · Ready</div>
        </div>
      </div>
      <div class="chat-input-row">
        <input type="text" id="chatInput" placeholder="Ask about scheduling, settings, troubleshooting…">
        <button class="btn-send" id="sendBtn" onclick="sendChat()">
          <i class="fa-solid fa-paper-plane"></i> SEND
        </button>
      </div>
      <div class="ai-notice">
        Designed in <span style="color:var(--accent2)">Ai Lab</span> ·
        Calls go from your browser → Groq AI · No data stored on device
      </div>
    </div>
  </div>


  <footer>
    V31 [AI] &bull; Designed &amp; Developed in <span>Ai Lab [Shashank Jangid]</span><br>
    IP: <span id="ipLabel">…</span> &bull; <span>dpsi-bell.local</span> &bull; SSID: <span id="ssidLabel">…</span>
  </footer>
</div>


<div id="toast">✓ Done!</div>


<script>
const GROQ_KEY   = '*************************************************';
const GROQ_MODEL = 'llama-3.3-70b-versatile';
const GROQ_URL   = 'https://api.groq.com/openai/v1/chat/completions';


const SYSTEM_PROMPT = `You are the official AI assistant for the DPSI Autonomous Bell System — a smart school/institute bell controller built on an ESP32 microcontroller. You have complete and accurate knowledge of every aspect of this project.


=== HARDWARE ===
- MCU: ESP32 (dual-core 240 MHz, 520 KB RAM, 4 MB flash)
- Relay: GPIO 26 — controls the physical bell (HIGH = ring)
- Button: GPIO 4 (INPUT_PULLUP) — short press rings 3s, hold 5s opens WiFi portal
- LCD: 16x2 I2C at address 0x27 — shows time, next bell, IP, OTA progress, ringing status
- Brown-out detection: disabled


=== FIRMWARE V31 ===
- Key improvements over V30: fully non-blocking WiFi reconnect (loop never stalls), 4-state button debounce state machine (no ghost triggers), bell duration protected from WiFi events
- Language: C++ Arduino framework for ESP32
- Libraries: WiFi, WebServer, LittleFS, ArduinoJson v6, LiquidCrystal_I2C, ESPmDNS, ArduinoOTA, WiFiManager, esp_task_wdt
- WiFi: WiFiManager auto-connects; AP fallback SSID = Bell_Configuration(12345678), pass = 12345678
- Hostname: dpsi-bell → dpsi-bell.local (mDNS)
- Timezone: IST UTC+5:30
- NTP: pool.ntp.org + time.google.com; falls back to saved RTC time if unavailable
- Offline time: saved to /lasttime.txt every 60s; loaded on boot
- Watchdog: 45s WDT
- WiFi watchdog: NON-BLOCKING state machine — no delay() in reconnect path; never restarts
- Storage: LittleFS — /sched.json, /history.json, /holiday.txt, /lasttime.txt


=== WEB DASHBOARD ===
- Auth: HTTP Basic — username: admin, password: admindps
- URL: http://<ip>/ or http://dpsi-bell.local/


=== API ENDPOINTS (all require auth) ===
- GET  /status    → {time, ntpSynced, wifiOk, holidayMode, bellActive, ip, ssid, nextBell, nextBellLabel, nextBellMins}
- GET  /list      → JSON array of schedule
- GET  /ring?dur=N → trigger bell N seconds
- POST /save      → save schedule JSON
- GET  /setHoliday?val=0|1
- GET  /history
- GET  /backup
- POST /restore
- GET  /testbell?dur=N


=== SCHEDULE FORMAT ===
[{"t":"08:00","d":[1,1,1,1,1,0,0],"s":4}, ...] max 30 entries


Be concise, practical, and accurate.`;


const chatHistory = [];
const IDLE_BEFORE_REFRESH = 15000;
let lastInteractionMs = 0;
let scheduleLoaded    = false;
let pendingSchedule   = null;


function markInteraction() {
  lastInteractionMs = Date.now();
  document.getElementById('dirtyBadge').classList.add('show');
}
function isEditorIdle() { return (Date.now() - lastInteractionMs) > IDLE_BEFORE_REFRESH; }


const bellListEl = document.getElementById('bellList');
['focusin','keydown','mousedown','change'].forEach(evt => bellListEl.addEventListener(evt, markInteraction));


function switchTab(id, btn) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
  btn.classList.add('active');
  document.getElementById('tab-' + id).classList.add('active');
  if (id === 'hist') fetchHistory();
  if (id === 'ai')   document.getElementById('chatInput').focus();
}


function tickClock() {
  const n = new Date(), p = x => String(x).padStart(2,'0');
  document.getElementById('clock').innerHTML =
    `${p(n.getHours())}<span class="sep">:</span>${p(n.getMinutes())}<span class="sep">:</span>${p(n.getSeconds())}`;
  document.getElementById('date').textContent =
    n.toLocaleDateString('en-IN', {weekday:'long',day:'numeric',month:'long',year:'numeric'});
}
setInterval(tickClock, 1000); tickClock();


async function pollStatus() {
  try {
    const res = await fetch('/status');
    if (!res.ok) return;
    const data = await res.json();
    const dot = document.getElementById('ntpDot');
    if (data.ntpSynced && data.wifiOk)       dot.className = 'dot green';
    else if (data.ntpSynced && !data.wifiOk) dot.className = 'dot orange';
    else                                      dot.className = 'dot red';
    let lbl = data.time;
    if (!data.wifiOk)    lbl += ' · OFFLINE';
    if (data.holidayMode) lbl += ' · HOLIDAY';
    if (data.bellActive)  lbl += ' · 🔔 RINGING';
    document.getElementById('statusText').textContent = lbl;
    document.getElementById('ipLabel').textContent    = data.ip   || '—';
    document.getElementById('ssidLabel').textContent  = data.ssid || '—';
    document.getElementById('holidayToggle').checked  = !!data.holidayMode;
    const btn = document.getElementById('ringBtn');
    if (data.bellActive) {
      btn.classList.add('ringing');
      btn.innerHTML = '<i class="fa-solid fa-bell fa-shake"></i> RINGING…';
    } else {
      btn.classList.remove('ringing');
      btn.innerHTML = '<i class="fa-solid fa-bell"></i> INSTANT RING';
    }
    if (data.nextBell && data.nextBell !== '--:--') {
      document.getElementById('nextBellTime').textContent  = data.nextBell;
      document.getElementById('nextBellLabel').textContent = data.nextBellLabel || '';
      const m = parseInt(data.nextBellMins);
      if (!isNaN(m) && m >= 0) {
        if (m === 0)     document.getElementById('countdown').textContent = 'ringing now!';
        else if (m < 60) document.getElementById('countdown').textContent = `in ${m} min`;
        else { const h = Math.floor(m/60), r = m%60; document.getElementById('countdown').textContent = `in ${h}h${r ? ` ${r}m` : ''}`; }
      } else document.getElementById('countdown').textContent = '';
    } else {
      document.getElementById('nextBellTime').textContent  = '--:--';
      document.getElementById('countdown').textContent     = data.holidayMode ? 'Holiday mode' : 'No bells';
      document.getElementById('nextBellLabel').textContent = '';
    }
  } catch(e) {}
}


async function fetchSchedule(force = false) {
  try {
    const res  = await fetch('/list');
    const json = await res.text();
    if (!force && !isEditorIdle()) return;
    if (!force && scheduleLoaded && json === pendingSchedule) return;
    pendingSchedule = json;
    renderList(JSON.parse(json));
    scheduleLoaded = true;
    document.getElementById('dirtyBadge').classList.remove('show');
  } catch(e) {}
}


async function fetchHistory() {
  try {
    const data = await (await fetch('/history')).json();
    const tbody = document.getElementById('historyBody');
    if (!data || !data.length) {
      tbody.innerHTML = `<tr><td colspan="3" style="color:var(--muted);text-align:center;padding:24px">No history yet</td></tr>`;
      return;
    }
    tbody.innerHTML = data.slice().reverse().map(h => {
      const src = h.source || '';
      let chip = `<span class="src-chip src-web">🖱 Web</span>`;
      if (src.includes('SCHEDULE'))    chip = `<span class="src-chip src-sched">📅 Schedule</span>`;
      else if (src.includes('BUTTON')) chip = `<span class="src-chip src-btn">🔘 Button</span>`;
      else if (src.includes('TEST'))   chip = `<span class="src-chip src-test">🧪 Test</span>`;
      const dc = h.dur >= 10 ? 'var(--danger)' : h.dur >= 5 ? 'var(--accent)' : 'var(--success)';
      return `<tr><td>${h.time}</td><td>${chip}</td><td style="text-align:right;font-variant-numeric:tabular-nums;color:${dc}">${h.dur}s</td></tr>`;
    }).join('');
  } catch(e) {}
}


function renderList(data) {
  document.getElementById('bellList').innerHTML = '';
  rowId = 0;
  if (data && data.length) data.forEach(item => addRow(item));
}


const DAY_LABELS  = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
const DAY_LETTERS = ['M','T','W','T','F','S','S'];
let rowId = 0;


function addRow(data) {
  const def = data || { t:'08:00', d:[1,1,1,1,1,0,0], s:4 };
  const div = document.createElement('div');
  div.className = 'bell-row';
  let daysHtml = '';
  DAY_LABELS.forEach((lbl, i) => {
    daysHtml += `<label class="day-chip" title="${lbl}"><input type="checkbox" class="day-cb" data-idx="${i}" ${def.d[i] ? 'checked' : ''}><span>${DAY_LETTERS[i]}</span></label>`;
  });
  div.innerHTML = `
    <div class="bell-row-top">
      <input type="time" class="time-val" value="${def.t.length===4?'0'+def.t:def.t}">
      <div class="dur-wrap"><input type="number" class="dur-val" value="${def.s||4}" min="1" max="60"><label>sec</label></div>
      <button class="btn-del" onclick="removeRow(this)" title="Remove"><i class="fa-solid fa-xmark"></i></button>
    </div>
    <div class="days-row">${daysHtml}</div>`;
  document.getElementById('bellList').appendChild(div);
  rowId++;
}


function removeRow(btn) {
  const row = btn.closest('.bell-row');
  row.classList.add('removing');
  setTimeout(() => row.remove(), 280);
  markInteraction();
}


async function save() {
  const rows = [];
  document.querySelectorAll('.bell-row').forEach(row => {
    const d = [];
    row.querySelectorAll('.day-cb').forEach(cb => d.push(cb.checked ? 1 : 0));
    rows.push({ t: row.querySelector('.time-val').value, d: d, s: parseInt(row.querySelector('.dur-val').value) || 4 });
  });
  const body = JSON.stringify(rows);
  const btn  = document.getElementById('saveBtn');
  btn.classList.add('saving');
  btn.innerHTML = '<i class="fa-solid fa-spinner fa-spin"></i> Saving…';
  try {
    const res = await fetch('/save', { method:'POST', headers:{'Content-Type':'application/json'}, body });
    if (res.ok) {
      pendingSchedule = body; lastInteractionMs = 0;
      document.getElementById('dirtyBadge').classList.remove('show');
      showToast('✓ Schedule saved!', false);
    } else showToast('✗ Save failed', true);
  } catch(e) { showToast('✗ Network error', true); }
  btn.classList.remove('saving');
  btn.innerHTML = '<i class="fa-solid fa-floppy-disk"></i> SAVE SCHEDULE';
}


function ringNow()       { fetch('/ring?dur=3').catch(()=>{}); }
function toggleHoliday() { fetch('/setHoliday?val=' + (document.getElementById('holidayToggle').checked ? 1 : 0)).catch(()=>{}); }


function showToast(msg, isErr) {
  const t = document.getElementById('toast');
  t.textContent = msg; t.className = 'show' + (isErr ? ' err' : '');
  clearTimeout(t._tid); t._tid = setTimeout(() => t.className = '', 2800);
}


async function sendChat() {
  const inp  = document.getElementById('chatInput');
  const text = inp.value.trim();
  if (!text || document.getElementById('sendBtn').disabled) return;
  inp.value = '';
  appendMsg(text, 'user');
  chatHistory.push({ role:'user', content: text });
  if (chatHistory.length > 20) chatHistory.splice(0, 2);
  document.getElementById('sendBtn').disabled = true;
  const typingEl = addTyping();
  try {
    const res = await fetch(GROQ_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + GROQ_KEY },
      body: JSON.stringify({ model: GROQ_MODEL, max_tokens: 600, temperature: 0.55, stream: false,
        messages: [{ role:'system', content: SYSTEM_PROMPT }, ...chatHistory] })
    });
    typingEl.remove();
    if (!res.ok) {
      let errMsg = 'HTTP ' + res.status;
      try { const e = await res.json(); errMsg = e?.error?.message || errMsg; } catch(_) {}
      appendMsg('⚠ Groq error: ' + errMsg, 'err');
    } else {
      const data  = await res.json();
      const reply = data?.choices?.[0]?.message?.content?.trim();
      if (reply) { chatHistory.push({ role:'assistant', content: reply }); appendMsg(reply, 'ai'); }
      else appendMsg('No reply received. Try again.', 'err');
    }
  } catch(e) {
    typingEl.remove();
    let msg = e.message || String(e);
    if (msg.includes('Failed to fetch') || msg.includes('NetworkError'))
      msg = 'Cannot reach api.groq.com. Check your internet connection.';
    appendMsg('⚠ ' + msg, 'err');
  }
  document.getElementById('sendBtn').disabled = false;
  inp.focus();
}


function appendMsg(text, type) {
  const msgs = document.getElementById('chatMessages');
  const div  = document.createElement('div');
  div.className   = 'msg ' + type;
  div.textContent = text;
  if (type === 'ai') {
    const meta = document.createElement('div');
    meta.className   = 'msg-meta';
    meta.textContent = 'AI Assistant · ' + new Date().toLocaleTimeString('en-IN',{hour:'2-digit',minute:'2-digit'});
    div.appendChild(meta);
  }
  msgs.appendChild(div);
  msgs.scrollTop = msgs.scrollHeight;
  return div;
}


function addTyping() {
  const msgs = document.getElementById('chatMessages');
  const div  = document.createElement('div');
  div.className = 'msg ai';
  div.innerHTML = '<div class="typing-dots"><span></span><span></span><span></span></div>';
  msgs.appendChild(div); msgs.scrollTop = msgs.scrollHeight;
  return div;
}


document.getElementById('chatInput').addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendChat(); }
});


window.onload = () => {
  fetchSchedule(true); fetchHistory(); pollStatus();
  setInterval(pollStatus,            5000);
  setInterval(() => fetchSchedule(), 10000);
  setInterval(fetchHistory,          20000);
};
</script>
</body>
</html>
)rawliteral";




// ═══════════════════════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ═══════════════════════════════════════════════════════════════════════════════


void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}


void handleRing() {
  if (!requireAuth()) return;
  int d = server.arg("dur").toInt();
  triggerBell(d > 0 ? d : 3, "WEB_INSTANT");
  server.send(200, "text/plain", "OK");
}


void handleList() {
  if (!requireAuth()) return;
  server.send(200, "application/json", scheduleJson);
}


void handleHistory() {
  if (!requireAuth()) return;
  server.send(200, "application/json", historyJson);
}


void handleSave() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  String body = server.arg("plain"); body.trim();
  if (body.length() > MAX_BODY_BYTES) { server.send(413, "text/plain", "Too large"); return; }
  if (!body.startsWith("["))           { server.send(400, "text/plain", "Invalid JSON"); return; }
  DynamicJsonDocument tmp(2048);
  DeserializationError err = deserializeJson(tmp, body);
  if (err || tmp.as<JsonArray>().size() > MAX_BELLS) {
    server.send(400, "text/plain", "Bad JSON or too many bells"); return;
  }
  scheduleDoc.clear();
  deserializeJson(scheduleDoc, body);
  scheduleCache = scheduleDoc.as<JsonArray>();
  scheduleJson  = body;
  File f = LittleFS.open("/sched.json", "w");
  if (f) { f.print(body); f.close(); }
  logEvent("Schedule saved — " + String(scheduleCache.size()) + " bells");
  server.send(200, "text/plain", "OK");
}


void handleSetHoliday() {
  if (!requireAuth()) return;
  holidayMode = (server.arg("val") == "1");
  File f = LittleFS.open("/holiday.txt", "w");
  if (f) { f.print(server.arg("val")); f.close(); }
  server.send(200, "text/plain", "OK");
}


void handleGetHoliday() {
  if (!requireAuth()) return;
  server.send(200, "text/plain", holidayMode ? "1" : "0");
}


void handleStatus() {
  if (!requireAuth()) return;
  struct tm ti;
  bool valid = getLocalTime(&ti);
  char timeStr[6] = "--:--";
  if (valid) sprintf(timeStr, "%02d:%02d", ti.tm_hour, ti.tm_min);


  String nextTime = "--:--", nextLabel = "";
  int nextMins = -1;
  if (valid && !holidayMode) {
    NextBellInfo nb = getNextBell(ti);
    nextTime  = nb.timeStr;
    nextLabel = nb.label;
    nextMins  = (nb.minutesLeft < 9999) ? nb.minutesLeft : -1;
  }


  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  String json = "{";
  json += "\"time\":\""          + String(timeStr)            + "\",";
  json += "\"ntpSynced\":"       + String(ntpSynced ? 1 : 0)  + ",";
  json += "\"wifiOk\":"          + String(wifiOk ? 1 : 0)     + ",";
  json += "\"holidayMode\":"     + String(holidayMode ? 1 : 0) + ",";
  json += "\"bellActive\":"      + String(bellActive ? 1 : 0)  + ",";
  json += "\"ip\":\""            + WiFi.localIP().toString()   + "\",";
  json += "\"ssid\":\""          + WiFi.SSID()                 + "\",";
  json += "\"nextBell\":\""      + nextTime                    + "\",";
  json += "\"nextBellLabel\":\"" + nextLabel                   + "\",";
  json += "\"nextBellMins\":"    + String(nextMins)            + "}";
  server.send(200, "application/json", json);
}


void handleBackup() {
  if (!requireAuth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=dpsi_schedule.json");
  server.send(200, "application/json", scheduleJson);
}


void handleRestore() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  String body = server.arg("plain"); body.trim();
  if (body.length() > MAX_BODY_BYTES) { server.send(413, "text/plain", "Too large"); return; }
  if (!body.startsWith("["))           { server.send(400, "text/plain", "Invalid JSON"); return; }
  DynamicJsonDocument tmp(2048);
  if (deserializeJson(tmp, body)) { server.send(400, "text/plain", "Bad JSON"); return; }
  scheduleDoc.clear();
  deserializeJson(scheduleDoc, body);
  scheduleCache = scheduleDoc.as<JsonArray>();
  scheduleJson  = body;
  File f = LittleFS.open("/sched.json", "w");
  if (f) { f.print(body); f.close(); }
  logEvent("Schedule restored");
  server.send(200, "text/plain", "Restored");
}


void handleTestBell() {
  if (!requireAuth()) return;
  int d = server.arg("dur").toInt();
  triggerBell(d > 0 ? d : 3, "TEST_ROUTE");
  server.send(200, "text/plain", "Test bell fired");
}




// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════════


void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(300);
  logEvent("Booting DPSI Autonomous Bell V31");


  pinMode(RELAY_PIN,  OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);


  wdtInit();
  esp_task_wdt_reset();


  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("DPSI AUTONOMOUS ");
  lcd.setCursor(0, 1); lcd.print("BELL  V31       ");


  // ── Mount LittleFS & load files ──────────────────────────────────────────
  if (LittleFS.begin(true)) {
    if (LittleFS.exists("/sched.json")) {
      File f = LittleFS.open("/sched.json", "r");
      scheduleJson = f.readString(); f.close();
      deserializeJson(scheduleDoc, scheduleJson);
      scheduleCache = scheduleDoc.as<JsonArray>();
      logEvent("Schedule loaded — " + String(scheduleCache.size()) + " bells");
    }
    if (LittleFS.exists("/history.json")) {
      File f = LittleFS.open("/history.json", "r");
      String raw = f.readString(); f.close();
      deserializeJson(historyDoc, raw);
      serializeJson(historyDoc, historyJson);
    }
    if (LittleFS.exists("/holiday.txt")) {
      File f = LittleFS.open("/holiday.txt", "r");
      String val = f.readString(); f.close();
      val.trim(); holidayMode = (val == "1");
    }
    // Load saved time BEFORE WiFi so schedule is armed immediately on boot
    if (loadSavedTime()) {
      ntpSynced = true;
      logEvent("Pre-loaded saved time — schedule active before NTP");
    }
  } else {
    logEvent("LittleFS mount failed!");
  }


  // ── WiFi via WiFiManager ──────────────────────────────────────────────────
  WiFiManager wm;
  wm.setHostname(HOSTNAME);
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(120);
  esp_task_wdt_delete(NULL);
  esp_task_wdt_deinit();

  bool connected = wm.autoConnect(AP_SSID, AP_PASS);

  wdtReinit();
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  if (!wm.autoConnect(AP_SSID, AP_PASS)) {
    logEvent("WiFi connect failed — OFFLINE mode. Bell schedule running on saved time.");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("OFFLINE MODE    ");
    lcd.setCursor(0, 1); lcd.print("Bell sched: OK  ");
    delay(2000);
  } else {
    wifiConnectedMs = millis();
    logEvent("WiFi OK | IP: " + WiFi.localIP().toString() + " | SSID: " + WiFi.SSID());
    bool mdnsOk = false;
    for (int i = 0; i < 3 && !mdnsOk; i++) { if (i) delay(500); mdnsOk = MDNS.begin(HOSTNAME); }
    if (mdnsOk) { MDNS.addService("http", "tcp", 80); logEvent("mDNS OK → dpsi-bell.local"); }
    else          logEvent("mDNS failed (use IP address)");
  }


  // ── NTP sync ─────────────────────────────────────────────────────────────
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
    struct tm ti;
    if (getLocalTime(&ti, 8000)) {
      ntpSynced     = true;
      lastNtpSyncMs = millis();
      saveCurrentTime();
      logEvent("NTP synced at boot");
    } else {
      logEvent("NTP unavailable at boot — using pre-loaded saved time");
    }
  } else {
    logEvent("No WiFi at boot — fully on saved time");
  }


  if (!ntpSynced) logEvent("WARNING: No time source. Schedule disabled until time is known.");


  // ── OTA ──────────────────────────────────────────────────────────────────
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() {
    esp_task_wdt_delete(NULL); esp_task_wdt_deinit();
    digitalWrite(RELAY_PIN, LOW); bellActive = false;
    logEvent("OTA start");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("OTA UPDATING... ");
    lcd.setCursor(0, 1); lcd.print("DO NOT POWER OFF");
  });
  ArduinoOTA.onEnd([]() {
    logEvent("OTA complete");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("UPDATE DONE!    ");
    lcd.setCursor(0, 1); lcd.print("Rebooting...    ");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    static uint8_t last = 255;
    uint8_t pct = (uint8_t)((p * 100) / t);
    if (pct != last) { last = pct; char line[17]; snprintf(line, sizeof(line), "Progress: %3d%%  ", pct); lcd.setCursor(0,1); lcd.print(line); }
  });
  ArduinoOTA.onError([](ota_error_t err) {
    logEvent("OTA error: " + String(err));
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("OTA FAILED!     ");
    wdtReinit();
  });
  ArduinoOTA.begin();


  // ── Routes ────────────────────────────────────────────────────────────────
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/ring",       HTTP_GET,  handleRing);
  server.on("/list",       HTTP_GET,  handleList);
  server.on("/history",    HTTP_GET,  handleHistory);
  server.on("/save",       HTTP_POST, handleSave);
  server.on("/setHoliday", HTTP_GET,  handleSetHoliday);
  server.on("/getHoliday", HTTP_GET,  handleGetHoliday);
  server.on("/status",     HTTP_GET,  handleStatus);
  server.on("/backup",     HTTP_GET,  handleBackup);
  server.on("/restore",    HTTP_POST, handleRestore);
  server.on("/testbell",   HTTP_GET,  handleTestBell);


  server.begin();
  logEvent("HTTP server started");


  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(lcdPad("IP:" + WiFi.localIP().toString()));
    lcd.setCursor(0, 1); lcd.print("dpsi-bell.local ");
  } else {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("OFFLINE - NO IP ");
    lcd.setCursor(0, 1); lcd.print("Bell sched: ON  ");
  }


  logEvent("System ready — V31");
}




// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP  — everything runs non-blocking, parallel, never stalls
// ═══════════════════════════════════════════════════════════════════════════════


void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  ArduinoOTA.handle();


  // ── Button (state machine — zero delay, zero blocking) ────────────────────
  handleButton();


  // ── WiFi watchdog (non-blocking state machine) ────────────────────────────
  checkWifi();


  unsigned long cm = millis();


  // ── Bell STOP — checked every loop() iteration for accuracy ──────────────
  // Using subtraction-safe unsigned arithmetic
  if (bellActive && (cm - bellStartMs >= bellDurationMs)) {
    digitalWrite(RELAY_PIN, LOW);
    bellActive = false;
    logEvent("RING STOP");
  }


  // ── Periodic NTP re-sync every 6 h ───────────────────────────────────────
  if (cm - lastNtpSyncMs >= NTP_RESYNC_INTERVAL_MS) {
    checkNtp();
  }


  // ── Save time to flash every 60 s ────────────────────────────────────────
  if (ntpSynced && (cm - lastTimeSaveMs >= TIME_SAVE_INTERVAL_MS)) {
    lastTimeSaveMs = cm;
    saveCurrentTime();
  }


  // ── 1-second LCD & schedule tick ─────────────────────────────────────────
  static unsigned long lastSecMs = 0;
  if (cm - lastSecMs < 1000) return;
  lastSecMs = cm;


  struct tm ti;
  bool timeValid = getLocalTime(&ti);
  if (!ntpSynced && timeValid) { ntpSynced = true; logEvent("NTP late-sync OK"); }


  // ── LCD ───────────────────────────────────────────────────────────────────
  if (showIP && WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(0, 0); lcd.print(lcdPad("IP:" + WiFi.localIP().toString()));
    lcd.setCursor(0, 1); lcd.print("dpsi-bell.local ");
    if (cm - wifiConnectedMs > IP_DISPLAY_MS) { showIP = false; lcd.clear(); }
  } else if (timeValid) {
    char top[17];
    strftime(top, sizeof(top), "TIME: %H:%M:%S  ", &ti);
    lcd.setCursor(0, 0); lcd.print(top);
    lcd.setCursor(0, 1);
    if (holidayMode)     lcd.print("MODE: HOLIDAY   ");
    else if (bellActive) lcd.print("**** RINGING ****");
    else {
      NextBellInfo nb = getNextBell(ti);
      String line = "NEXT: " + (nb.timeStr.length() > 4 ? nb.timeStr : "--:--");
      if (nb.label == "tomorrow" || nb.label.startsWith("in ")) line += "-Tom.";
      lcd.print(lcdPad(line));
    }
  }


  // ── Schedule trigger ──────────────────────────────────────────────────────
  if (!ntpSynced || holidayMode || !timeValid) return;


  char curMin[6];
  sprintf(curMin, "%02d:%02d", ti.tm_hour, ti.tm_min);
  char curDate[11];
  sprintf(curDate, "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
  String guardKey = String(curDate) + "T" + String(curMin);
  if (guardKey == lastTriggeredMin) return;
  lastTriggeredMin = guardKey;


  int dayIdx = (ti.tm_wday == 0) ? 6 : ti.tm_wday - 1;
  for (JsonObject item : scheduleCache) {
    String stored = item["t"].as<String>();
    if (stored.length() == 4) stored = "0" + stored;
    if (String(curMin) == stored && item["d"][dayIdx].as<int>() == 1) {
      int dur = item["s"].as<int>();
      if (dur < 1 || dur > 60) dur = 4;
      triggerBell(dur, "SCHEDULE");
      break;
    }
  }
}
