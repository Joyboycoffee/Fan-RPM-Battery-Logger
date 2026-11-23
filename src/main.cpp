#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "esp_wifi.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ================= USER CONFIGURATION =================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// Static IP Configuration
IPAddress local_IP = DEFAULT_LOCAL_IP;
IPAddress gateway = DEFAULT_GATEWAY;
IPAddress subnet = DEFAULT_SUBNET;
IPAddress primaryDNS = DEFAULT_DNS1;
IPAddress secondaryDNS = DEFAULT_DNS2;

// ================= PIN ASSIGNMENTS =================
// Battery ADC Voltage Divider Pin (Two 10kΩ Resistors)
const int PIN_BATTERY_ADC = 0; // GPIO 0 (ADC1_CH0)

// HW-477 / A3144 Hall Effect Sensor Pin
const int PIN_HALL_SENSOR = 4; // GPIO 4

// Onboard LED on ESP32-C3 Super Mini (Active-LOW)
const int PIN_LED = 8;

// ================= TIMING & WEAR-LEVELING CONSTANTS =================
const unsigned long BATTERY_NVS_SAVE_INTERVAL_MS = 10000; // Auto-save live uptime every 10s
const unsigned long PULSE_TIMEOUT_MS = 2200;              // 2.2s pulse timeout for fan stop
const unsigned long FAN_NVS_SAVE_INTERVAL_MS = 30000;     // Commit fan stats every 30s while spinning

// ================= PERSISTENT STORAGE & SERVER OBJECTS =================
Preferences prefs;
WebServer server(80);

// ================= BATTERY BENCHMARK STATE =================
unsigned long lastBatteryRunDurationSec = 0; // Duration of previous battery run
unsigned long totalBatteryRunsCount = 0;     // Total completed battery runs
unsigned long lastBatNvsSaveMillis = 0;      // Last periodic NVS save timestamp
float liveBatteryVoltage = 0.0f;
int liveBatteryPercent = 0;

// ================= FAN SENSOR STATE =================
volatile unsigned long totalLivePulses = 0;
volatile unsigned long lastPulseMicros = 0;
volatile unsigned long lastPulseMillis = 0;
volatile unsigned long pulseIntervalMicros = 0;

// Persistent Fan Totals
unsigned long totalSavedRunningSeconds = 0;
unsigned long totalSavedStoppedSeconds = 0;
unsigned long totalSavedLifetimePulses = 0;
unsigned long totalFanSessionsCount = 0;

// Live Fan Session Variables
bool isFanRunning = false;
unsigned long sessionStartMillis = 0;
unsigned long lastStateChangeMillis = 0;
unsigned long lastFanNvsSaveMillis = 0;
unsigned long lastSecTickMillis = 0;

float currentRPM = 0.0f;
float currentHz = 0.0f;
float sessionSpeedSum = 0.0f;
unsigned long sessionSpeedSamples = 0;

// ================= HARDWARE ISR =================
void IRAM_ATTR hallSensorISR() {
  unsigned long nowMicros = micros();
  unsigned long nowMillis = millis();
  
  if (nowMicros - lastPulseMicros > 500) { // 500us debounce
    pulseIntervalMicros = nowMicros - lastPulseMicros;
    lastPulseMicros = nowMicros;
    lastPulseMillis = nowMillis;
    totalLivePulses++;
  }
}

// ================= BATTERY MEASUREMENT =================
float readBatteryVoltage() {
  // Over-sample 16 times to smooth out Wi-Fi RF noise spikes
  uint32_t sumMilliVolts = 0;
  for (int i = 0; i < 16; i++) {
    sumMilliVolts += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  float avgPinMilliVolts = (float)sumMilliVolts / 16.0f;
  
  // 1:1 voltage divider (Two 10kΩ resistors) divides voltage by 2.0
  float vBat = (avgPinMilliVolts * 2.0f) / 1000.0f;
  return vBat;
}

int calculateBatteryPercent(float v) {
  if (v >= 4.20f) return 100;
  if (v <= 3.20f) return 0;
  
  // Multi-segment Li-ion discharge approximation
  if (v >= 4.05f) return 90 + (int)((v - 4.05f) / 0.15f * 10);
  if (v >= 3.85f) return 60 + (int)((v - 3.85f) / 0.20f * 30);
  if (v >= 3.70f) return 30 + (int)((v - 3.70f) / 0.15f * 30);
  if (v >= 3.50f) return 10 + (int)((v - 3.50f) / 0.20f * 20);
  return (int)((v - 3.20f) / 0.30f * 10);
}

// ================= HELPER FUNCTIONS =================
String formatDuration(unsigned long totalSecs) {
  unsigned long days = totalSecs / 86400;
  unsigned long hours = (totalSecs % 86400) / 3600;
  unsigned long minutes = (totalSecs % 3600) / 60;
  unsigned long seconds = totalSecs % 60;
  
  char buf[32];
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", days, hours, minutes, seconds);
  } else {
    snprintf(buf, sizeof(buf), "%02luh %02lum %02lus", hours, minutes, seconds);
  }
  return String(buf);
}

// Append Completed Battery Discharge Run to LittleFS
void logBatteryRunToFile(unsigned long runId, unsigned long durationSec) {
  File f = LittleFS.open("/battery_runs.csv", FILE_APPEND);
  if (f) {
    f.printf("%lu,%lu,%s\n", runId, durationSec, formatDuration(durationSec).c_str());
    f.close();
    Serial.printf("[BATTERY LOG] Recorded Run #%lu: %s (%lu sec)\n", runId, formatDuration(durationSec).c_str(), durationSec);
  }
}

// Append Fan Session to LittleFS
void logFanSessionToFile(unsigned long durationSec, float avgRPM, unsigned long sessionPulses) {
  if (durationSec < 2) return;
  File f = LittleFS.open("/sessions.csv", FILE_APPEND);
  if (f) {
    f.printf("%lu,%lu,%.1f,%lu\n", totalFanSessionsCount, durationSec, avgRPM, sessionPulses);
    f.close();
    Serial.printf("[FAN LOG] Session #%lu logged: Duration %lus, Avg RPM: %.1f\n", totalFanSessionsCount, durationSec, avgRPM);
  }
}

// Save Fan Totals to NVS
void saveFanTotalsToNVS() {
  prefs.putULong("run_sec", totalSavedRunningSeconds);
  prefs.putULong("stop_sec", totalSavedStoppedSeconds);
  prefs.putULong("pulses", totalSavedLifetimePulses + totalLivePulses);
  prefs.putULong("sessions", totalFanSessionsCount);
  lastFanNvsSaveMillis = millis();
}

// Wi-Fi Event Handler
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("\n=========================================");
    Serial.println(">>> WI-FI CONNECTED & PORTAL ONLINE! <<<");
    Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Web Portal URL: http://%s or http://esp32-hall.local\n", WiFi.localIP().toString().c_str());
    Serial.printf("Signal RSSI: %d dBm\n", WiFi.RSSI());
    Serial.println("=========================================\n");
  }
}

// ================= EMBEDDED REAL-TIME WEB PORTAL =================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Battery &amp; Fan Dashboard</title>
<style>
:root{--bg:#0B0F19;--card:#151D2F;--border:#23304B;--text:#F1F5F9;--sub:#94A3B8;--accent:#38BDF8;--green:#10B981;--amber:#F59E0B;--red:#EF4444;}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;}
body{background:var(--bg);color:var(--text);padding:14px;min-height:100vh;display:flex;flex-direction:column;align-items:center;}
.wrap{width:100%;max-width:640px;}
header{text-align:center;margin-bottom:14px;}
.badge{display:inline-block;background:rgba(56,189,248,0.15);color:var(--accent);font-size:11px;font-weight:700;padding:3px 12px;border-radius:99px;border:1px solid rgba(56,189,248,0.3);margin-bottom:4px;}
h1{font-size:20px;font-weight:800;letter-spacing:-0.5px;}
.sub{font-size:12px;color:var(--sub);}

/* Battery Hero Card */
.bat-card{background:linear-gradient(135deg,#132338 0%,#0F172A 100%);border:2px solid #2563EB;border-radius:14px;padding:16px;margin-bottom:12px;box-shadow:0 8px 24px rgba(37,99,235,0.25);}
.bat-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;}
.bat-title{font-size:14px;font-weight:800;color:var(--accent);display:flex;align-items:center;gap:6px;}

/* Battery Level Gauge Bar */
.meter-box{background:#0B132B;border:1px solid rgba(56,189,248,0.25);border-radius:10px;padding:12px;margin-bottom:12px;}
.meter-top{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:6px;}
.pct-huge{font-size:28px;font-weight:900;letter-spacing:-1px;}
.volt-sub{font-size:14px;font-weight:700;color:var(--sub);}
.bar-track{width:100%;height:14px;background:#1E293B;border-radius:99px;overflow:hidden;border:1px solid #334155;position:relative;}
.bar-fill{height:100%;width:0%;background:var(--green);border-radius:99px;transition:width 0.4s ease,background-color 0.4s ease;}

.bat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
.bat-stat{background:rgba(15,23,42,0.7);border:1px solid rgba(56,189,248,0.2);border-radius:10px;padding:10px;}
.bat-lbl{font-size:10px;font-weight:700;text-transform:uppercase;color:var(--sub);letter-spacing:0.5px;}
.bat-val{font-size:18px;font-weight:800;margin-top:2px;font-variant-numeric:tabular-nums;}
.bat-sub{font-size:11px;color:var(--sub);margin-top:2px;}

/* Generic Grid */
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px;}
.stat{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:12px;}
.lbl{font-size:10px;font-weight:600;text-transform:uppercase;color:var(--sub);}
.val{font-size:18px;font-weight:800;margin-top:2px;}
.unit{font-size:12px;font-weight:400;color:var(--sub);margin-left:2px;}

/* Status Banner */
.status{background:var(--card);border:2px solid var(--border);border-radius:12px;padding:12px 16px;text-align:center;margin-bottom:12px;display:flex;align-items:center;gap:12px;transition:0.2s;}
.status.active{border-color:var(--green);box-shadow:0 0 20px rgba(16,185,129,0.3);}
.icon{width:42px;height:42px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:20px;background:#1E293B;color:#64748B;flex-shrink:0;}
.status.active .icon{background:rgba(16,185,129,0.2);color:var(--green);animation:spin 1s linear infinite;}
@keyframes spin{100%{transform:rotate(360deg);}}
.stitle{font-size:15px;font-weight:800;text-align:left;}
.ssub{font-size:11px;color:var(--sub);text-align:left;margin-top:1px;}

.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:12px;margin-bottom:12px;}
.card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;}
.ctitle{font-size:13px;font-weight:700;}

.btn{background:#1E293B;border:1px solid var(--border);color:var(--text);font-size:11px;font-weight:600;padding:5px 10px;border-radius:6px;cursor:pointer;text-decoration:none;display:inline-block;}
.btn:hover{background:#334155;border-color:var(--accent);color:var(--accent);}
.btn-primary{background:#2563EB;border-color:#3B82F6;color:#FFF;}
.btn-primary:hover{background:#1D4ED8;}
.btn-danger{color:#F87171;}
.btn-danger:hover{border-color:#EF4444;color:#EF4444;}

.row{display:flex;justify-content:space-between;align-items:center;padding:4px 0;}
.table-wrap{max-height:150px;overflow-y:auto;border:1px solid var(--border);border-radius:6px;margin-top:6px;}
table{width:100%;border-collapse:collapse;font-size:11px;text-align:left;}
th{background:#0F172A;color:var(--sub);padding:6px 8px;font-weight:600;position:sticky;top:0;}
td{padding:5px 8px;border-top:1px solid var(--border);}
footer{font-size:10px;color:#475569;text-align:center;margin-top:8px;}
</style>
</head>
<body>
<div class="wrap">
<header>
<div class="badge">ESP32-C3 • REAL-TIME BATTERY &amp; SENSOR HUB</div>
<h1>Battery Benchmark &amp; Telemetry</h1>
<div class="sub">10kΩ Voltage Divider on GPIO 0 • Auto-Saves Across Power Cuts</div>
</header>

<!-- Battery Hero Section -->
<div class="bat-card">
<div class="bat-header">
<span class="bat-title">🔋 Battery Fuel Gauge &amp; Benchmark</span>
<div>
<a class="btn btn-primary" href="/api/export_battery.csv" download="battery_benchmark_log.csv">Export CSV</a>
<button class="btn btn-danger" onclick="clearBatteryStats()">Reset Log</button>
</div>
</div>

<!-- Percentage & Voltage Meter -->
<div class="meter-box">
<div class="meter-top">
<div class="pct-huge" id="batPct">--%</div>
<div class="volt-sub" id="batVolt">0.00 V</div>
</div>
<div class="bar-track">
<div class="bar-fill" id="batBar"></div>
</div>
</div>

<div class="bat-grid">
<div class="bat-stat">
<div class="bat-lbl">⏱️ Current Battery Uptime</div>
<div class="bat-val" id="curUptime" style="color:#38BDF8;">00h 00m 00s</div>
<div class="bat-sub">Live uptime since boot</div>
</div>
<div class="bat-stat">
<div class="bat-lbl">⏮️ Last Battery Run</div>
<div class="bat-val" id="lastBatRun" style="color:#10B981;">--</div>
<div class="bat-sub" id="lastBatSub">Before previous power cut</div>
</div>
</div>
<div style="display:flex;justify-content:space-between;align-items:center;margin-top:10px;font-size:11px;color:var(--sub);">
<span>Completed Battery Tests: <strong id="batRunsCount" style="color:#FFF;">0</strong></span>
<span>Flash Save: <strong style="color:#38BDF8;">Every 10s</strong></span>
</div>
</div>

<!-- Battery History Table -->
<div class="card">
<div class="card-header">
<span class="ctitle">📋 Completed Battery Discharge Runs</span>
<button class="btn" onclick="loadBatteryHistory()">Refresh</button>
</div>
<div class="table-wrap">
<table>
<thead><tr><th>Run #</th><th>Discharge Duration</th><th>Total Seconds</th></tr></thead>
<tbody id="batteryTable"><tr><td colspan="3" style="text-align:center;color:var(--sub);">No completed battery runs recorded yet</td></tr></tbody>
</table>
</div>
</div>

<!-- Fan Live Status -->
<div id="card" class="status idle">
<div id="ico" class="icon">🌀</div>
<div>
<div id="stxt" class="stitle" style="color:#64748B;">FAN STOPPED / IDLE</div>
<div id="ssub" class="ssub">No rotation detected • Hall Sensor on GPIO 4</div>
</div>
</div>

<!-- Fan Telemetry Grid -->
<div class="grid">
<div class="stat"><div class="lbl">Live Rotation Speed</div><div class="val" id="rpm">0.0<span class="unit">RPM</span></div></div>
<div class="stat"><div class="lbl">Current Fan Session</div><div class="val" id="curSession">00h 00m 00s</div></div>
<div class="stat"><div class="lbl">Total Lifetime 'ON' Time</div><div class="val" id="totalOn" style="color:#10B981;">00h 00m 00s</div></div>
<div class="stat"><div class="lbl">Total Lifetime 'OFF' Time</div><div class="val" id="totalOff">00h 00m 00s</div></div>
</div>

<!-- System & Hardware Telemetry -->
<div class="card">
<div class="card-header">
<span class="ctitle">⚙️ Hardware &amp; Network Info</span>
<div>
<a class="btn" href="/api/export.csv" download="fan_runtime_log.csv">Export Fan CSV</a>
<button class="btn btn-danger" onclick="clearFanStats()">Reset Fan Flash</button>
</div>
</div>
<div class="row"><span style="font-size:12px;">Wi-Fi Signal Strength</span><span id="rssi" style="font-size:12px;font-weight:700;color:var(--accent);">- dBm</span></div>
<div class="row"><span style="font-size:12px;">Total Fan Sessions</span><span id="fanSessions" style="font-size:12px;font-weight:700;">0</span></div>
<div class="row"><span style="font-size:12px;">Lifetime Magnet Pulses</span><span id="lifetimePulses" style="font-size:12px;font-weight:700;">0</span></div>
</div>

<footer>ESP32-C3 Mini • NVS Wear-Leveled &amp; LittleFS CSV Logger</footer>
</div>

<script>
async function update(){
try{
let r=await fetch('/api/data');
if(!r.ok)return;
let d=await r.json();

// Battery Percentage & Voltage
let pct = d.bat_percent;
let volt = d.bat_voltage.toFixed(2);
document.getElementById('batPct').innerText = pct + '%';
document.getElementById('batVolt').innerText = volt + ' V';

let bar = document.getElementById('batBar');
bar.style.width = Math.min(100, Math.max(0, pct)) + '%';
if(pct > 50){
  bar.style.backgroundColor = '#10B981';
  document.getElementById('batPct').style.color = '#10B981';
}else if(pct > 20){
  bar.style.backgroundColor = '#F59E0B';
  document.getElementById('batPct').style.color = '#F59E0B';
}else{
  bar.style.backgroundColor = '#EF4444';
  document.getElementById('batPct').style.color = '#EF4444';
}

// Battery Uptime Stats
document.getElementById('curUptime').innerText = d.uptime_str;
document.getElementById('lastBatRun').innerText = d.last_battery_run_str;
document.getElementById('batRunsCount').innerText = d.battery_runs;
if(d.last_battery_run_sec > 0){
  document.getElementById('lastBatSub').innerText = 'Lasted ' + (d.last_battery_run_sec / 3600).toFixed(2) + ' hours on battery';
}

// Fan State
let c=document.getElementById('card'),t=document.getElementById('stxt'),s=document.getElementById('ssub');
if(d.fan_running){
  c.className='status active';
  t.innerText='FAN IS RUNNING ('+d.fan_rpm.toFixed(0)+' RPM)';
  t.style.color='#10B981';
  s.innerText='Rotation pulse detected • Active session';
}else{
  c.className='status idle';
  t.innerText='FAN STOPPED / IDLE';
  t.style.color='#64748B';
  s.innerText='0 RPM • Sensor awaiting magnetic pulse';
}

document.getElementById('rpm').innerHTML=d.fan_rpm.toFixed(1)+'<span class="unit">RPM</span>';
document.getElementById('curSession').innerText=d.fan_cur_session_str;
document.getElementById('totalOn').innerText=d.fan_total_on_str;
document.getElementById('totalOff').innerText=d.fan_total_off_str;
document.getElementById('fanSessions').innerText=d.fan_sessions;
document.getElementById('lifetimePulses').innerText=d.lifetime_pulses;
document.getElementById('rssi').innerText=d.rssi+' dBm';
}catch(e){}
}

async function loadBatteryHistory(){
try{
let r=await fetch('/api/battery_history');
if(!r.ok)return;
let list=await r.json();
let tb=document.getElementById('batteryTable');
if(!list || list.length===0){
  tb.innerHTML='<tr><td colspan="3" style="text-align:center;color:var(--sub);">No completed battery runs recorded yet</td></tr>';
  return;
}
let html='';
for(let i=list.length-1;i>=0;i--){
  let item=list[i];
  html+='<tr><td><strong>Run #'+item.id+'</strong></td><td style="color:#10B981;font-weight:700;">'+item.dur_str+'</td><td>'+item.dur_sec+'s</td></tr>';
}
tb.innerHTML=html;
}catch(e){}
}

async function clearBatteryStats(){
if(confirm('Are you sure you want to erase all completed battery benchmark logs?')){
  await fetch('/api/clear_battery_history',{method:'POST'});
  update();
  loadBatteryHistory();
}
}

async function clearFanStats(){
if(confirm('Are you sure you want to erase all fan rotation records?')){
  await fetch('/api/clear_history',{method:'POST'});
  update();
}
}

setInterval(update,500);
update();
loadBatteryHistory();
</script>
</body>
</html>)rawliteral";

// Handler for Web Portal Root "/"
void handleRoot() {
  size_t totalLen = sizeof(INDEX_HTML) - 1;
  server.setContentLength(totalLen);
  server.send(200, "text/html", "");
  
  WiFiClient client = server.client();
  size_t offset = 0;
  const size_t chunkSize = 512;
  while (offset < totalLen && client.connected()) {
    size_t toSend = (totalLen - offset > chunkSize) ? chunkSize : (totalLen - offset);
    client.write_P(INDEX_HTML + offset, toSend);
    offset += toSend;
    yield();
  }
}

// Handler for JSON Telemetry API "/api/data"
void handleData() {
  unsigned long now = millis();
  unsigned long currentUptimeSec = now / 1000;
  
  unsigned long liveCurSession = isFanRunning ? ((now - sessionStartMillis) / 1000) : 0;
  unsigned long currentTotalOn = totalSavedRunningSeconds + (isFanRunning ? liveCurSession : 0);
  
  String json = "{";
  // Battery stats
  json += "\"uptime_sec\":" + String(currentUptimeSec) + ",";
  json += "\"uptime_str\":\"" + formatDuration(currentUptimeSec) + "\",";
  json += "\"last_battery_run_sec\":" + String(lastBatteryRunDurationSec) + ",";
  json += "\"last_battery_run_str\":\"" + (lastBatteryRunDurationSec > 0 ? formatDuration(lastBatteryRunDurationSec) : String("--")) + "\",";
  json += "\"battery_runs\":" + String(totalBatteryRunsCount) + ",";
  json += "\"bat_voltage\":" + String(liveBatteryVoltage, 2) + ",";
  json += "\"bat_percent\":" + String(liveBatteryPercent) + ",";
  
  // Fan stats
  json += "\"fan_running\":" + String(isFanRunning ? "true" : "false") + ",";
  json += "\"fan_rpm\":" + String(currentRPM, 1) + ",";
  json += "\"fan_hz\":" + String(currentHz, 1) + ",";
  json += "\"fan_cur_session_str\":\"" + formatDuration(liveCurSession) + "\",";
  json += "\"fan_total_on_str\":\"" + formatDuration(currentTotalOn) + "\",";
  json += "\"fan_total_off_str\":\"" + formatDuration(totalSavedStoppedSeconds) + "\",";
  json += "\"fan_sessions\":" + String(totalFanSessionsCount) + ",";
  json += "\"lifetime_pulses\":" + String(totalSavedLifetimePulses + totalLivePulses) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI());
  json += "}";
  
  server.send(200, "application/json", json);
}

// Handler for Battery Benchmark History JSON "/api/battery_history"
void handleBatteryHistory() {
  File f = LittleFS.open("/battery_runs.csv", FILE_READ);
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }
  
  String json = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    
    int comma1 = line.indexOf(',');
    int comma2 = line.indexOf(',', comma1 + 1);
    if (comma1 > 0 && comma2 > 0) {
      String idStr = line.substring(0, comma1);
      String durSecStr = line.substring(comma1 + 1, comma2);
      String durStr = line.substring(comma2 + 1);
      
      if (!first) json += ",";
      json += "{\"id\":" + idStr + ",\"dur_sec\":" + durSecStr + ",\"dur_str\":\"" + durStr + "\"}";
      first = false;
    }
  }
  f.close();
  json += "]";
  server.send(200, "application/json", json);
}

// Handler for Battery CSV Export "/api/export_battery.csv"
void handleExportBattery() {
  File f = LittleFS.open("/battery_runs.csv", FILE_READ);
  if (!f) {
    server.send(200, "text/csv", "RunID,DurationSec,DurationStr\n");
    return;
  }
  
  server.setContentLength(f.size() + 32);
  server.sendHeader("Content-Disposition", "attachment; filename=\"battery_benchmark_log.csv\"");
  server.send(200, "text/csv", "");
  
  WiFiClient client = server.client();
  client.print("RunID,DurationSec,DurationStr\n");
  
  uint8_t buf[256];
  while (f.available() && client.connected()) {
    size_t len = f.read(buf, sizeof(buf));
    client.write(buf, len);
    yield();
  }
  f.close();
}

// Handler for Fan CSV Export "/api/export.csv"
void handleExportFan() {
  File f = LittleFS.open("/sessions.csv", FILE_READ);
  if (!f) {
    server.send(200, "text/csv", "SessionID,DurationSec,AvgRPM,Pulses\n");
    return;
  }
  
  server.setContentLength(f.size() + 40);
  server.sendHeader("Content-Disposition", "attachment; filename=\"fan_runtime_log.csv\"");
  server.send(200, "text/csv", "");
  
  WiFiClient client = server.client();
  client.print("SessionID,DurationSec,AvgRPM,Pulses\n");
  
  uint8_t buf[256];
  while (f.available() && client.connected()) {
    size_t len = f.read(buf, sizeof(buf));
    client.write(buf, len);
    yield();
  }
  f.close();
}

// Handler for Clearing Battery History "/api/clear_battery_history"
void handleClearBatteryHistory() {
  LittleFS.remove("/battery_runs.csv");
  totalBatteryRunsCount = 0;
  lastBatteryRunDurationSec = 0;
  prefs.putULong("bat_runs", 0);
  prefs.putULong("bat_last_sec", 0);
  prefs.putULong("bat_cur_sec", 0);
  server.send(200, "text/plain", "OK");
  Serial.println("[BATTERY] Benchmark history cleared.");
}

// Handler for Clearing Fan History "/api/clear_history"
void handleClearFanHistory() {
  LittleFS.remove("/sessions.csv");
  totalSavedRunningSeconds = 0;
  totalSavedStoppedSeconds = 0;
  totalSavedLifetimePulses = 0;
  totalLivePulses = 0;
  totalFanSessionsCount = 0;
  prefs.putULong("run_sec", 0);
  prefs.putULong("stop_sec", 0);
  prefs.putULong("pulses", 0);
  prefs.putULong("sessions", 0);
  server.send(200, "text/plain", "OK");
  Serial.println("[FAN] Persistent records cleared.");
}

// ================= SETUP =================
void setup() {
  // 1. Disable RTC Brownout Detector for ESP32-C3 RF stability
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // 2. Hardware Pin Setup
  pinMode(PIN_BATTERY_ADC, INPUT);
  pinMode(PIN_HALL_SENSOR, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH); // LED off initially

  // Set ADC Attenuation to 11dB (allows reading up to ~2.6V - 3.1V on pin)
  analogSetAttenuation(ADC_11db);

  // 3. Attach Interrupt for Hall Sensor
  attachInterrupt(digitalPinToInterrupt(PIN_HALL_SENSOR), hallSensorISR, FALLING);

  // 4. Initialize Serial Communication
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==========================================");
  Serial.println("  ESP32-C3 BATTERY BENCHMARK & FAN LOGGER ");
  Serial.println("==========================================");

  // 5. Initialize LittleFS Flash Storage
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] Failed to mount SPIFFS/LittleFS!");
  } else {
    Serial.println("[LittleFS] Storage mounted successfully.");
  }

  // 6. Initialize Preferences (NVS Flash Storage)
  prefs.begin("bat_logger", false);
  
  // Read previous run's saved uptime
  unsigned long previousUptimeSec = prefs.getULong("bat_cur_sec", 0);
  totalBatteryRunsCount = prefs.getULong("bat_runs", 0);
  lastBatteryRunDurationSec = prefs.getULong("bat_last_sec", 0);

  // If previous uptime was >= 15 seconds, record it as a completed battery discharge run!
  if (previousUptimeSec >= 15) {
    totalBatteryRunsCount++;
    lastBatteryRunDurationSec = previousUptimeSec;
    prefs.putULong("bat_runs", totalBatteryRunsCount);
    prefs.putULong("bat_last_sec", lastBatteryRunDurationSec);
    
    // Log to CSV
    logBatteryRunToFile(totalBatteryRunsCount, lastBatteryRunDurationSec);
    Serial.printf("\n>>> [POWER CUT DETECTED] Previous Battery Run lasted %s (%lu sec) <<<\n\n",
                  formatDuration(lastBatteryRunDurationSec).c_str(), lastBatteryRunDurationSec);
  }
  
  // Reset current run counter in NVS for this boot
  prefs.putULong("bat_cur_sec", 0);

  // Load persistent Fan stats
  totalSavedRunningSeconds = prefs.getULong("run_sec", 0);
  totalSavedStoppedSeconds = prefs.getULong("stop_sec", 0);
  totalSavedLifetimePulses = prefs.getULong("pulses", 0);
  totalFanSessionsCount    = prefs.getULong("sessions", 0);

  Serial.printf("[NVS] Total Completed Battery Runs: %lu\n", totalBatteryRunsCount);
  Serial.printf("[NVS] Last Battery Run Duration: %s\n", formatDuration(lastBatteryRunDurationSec).c_str());
  Serial.printf("[NVS] Total Fan Lifetime ON: %s\n", formatDuration(totalSavedRunningSeconds).c_str());

  // 7. Initialize Wi-Fi
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_country_code("01", true);
  WiFi.setSleep(false);
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);

  Serial.printf("[WIFI] Connecting to '%s'...\n", ssid);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 35) {
    delay(300);
    Serial.print(".");
    attempts++;
  }

  // 8. Initialize mDNS
  if (MDNS.begin("esp32-hall")) {
    Serial.println("[mDNS] Responder started at: http://esp32-hall.local");
  }

  // 9. Register Web Endpoints & Start Server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/battery_history", HTTP_GET, handleBatteryHistory);
  server.on("/api/export_battery.csv", HTTP_GET, handleExportBattery);
  server.on("/api/clear_battery_history", HTTP_POST, handleClearBatteryHistory);
  server.on("/api/export.csv", HTTP_GET, handleExportFan);
  server.on("/api/clear_history", HTTP_POST, handleClearFanHistory);
  server.begin();
  Serial.println("[HTTP] Web Server started on port 80");
}

unsigned long lastSerialPrint = 0;
unsigned long lastAdcReadMillis = 0;

// ================= MAIN LOOP =================
void loop() {
  server.handleClient();

  unsigned long now = millis();
  unsigned long currentUptimeSec = now / 1000;

  // 1. Periodic Battery Voltage & Percentage Read (Every 500ms)
  if (now - lastAdcReadMillis >= 500) {
    lastAdcReadMillis = now;
    liveBatteryVoltage = readBatteryVoltage();
    liveBatteryPercent = calculateBatteryPercent(liveBatteryVoltage);
  }

  // 2. Periodic Battery Uptime Snapshot to NVS (Every 10 seconds)
  // Ensures zero data loss even if battery cuts out abruptly
  if (now - lastBatNvsSaveMillis >= BATTERY_NVS_SAVE_INTERVAL_MS) {
    lastBatNvsSaveMillis = now;
    prefs.putULong("bat_cur_sec", currentUptimeSec);
  }

  // 3. Fan Rotation Detection
  bool fanSpinningNow = (lastPulseMillis > 0) && (now - lastPulseMillis < PULSE_TIMEOUT_MS);

  if (fanSpinningNow) {
    if (pulseIntervalMicros > 0) {
      currentHz = 1000000.0f / (float)pulseIntervalMicros;
      currentRPM = currentHz * 60.0f;
    }
    sessionSpeedSum += currentRPM;
    sessionSpeedSamples++;
  } else {
    currentRPM = 0.0f;
    currentHz = 0.0f;
  }

  // 4. State Transition: STOPPED -> RUNNING
  if (fanSpinningNow && !isFanRunning) {
    isFanRunning = true;
    sessionStartMillis = now;
    lastStateChangeMillis = now;
    sessionSpeedSum = currentRPM;
    sessionSpeedSamples = 1;
    totalFanSessionsCount++;
    Serial.printf("\n>>> [EVENT] Fan Started Spinning! (Session #%lu) <<<\n", totalFanSessionsCount);
  }
  // 5. State Transition: RUNNING -> STOPPED
  else if (!fanSpinningNow && isFanRunning) {
    isFanRunning = false;
    unsigned long sessionDuration = (now - sessionStartMillis) / 1000;
    totalSavedRunningSeconds += sessionDuration;

    float avgSessionRpm = (sessionSpeedSamples > 0) ? (sessionSpeedSum / sessionSpeedSamples) : 0.0f;
    logFanSessionToFile(sessionDuration, avgSessionRpm, totalLivePulses);
    saveFanTotalsToNVS();

    lastStateChangeMillis = now;
    sessionSpeedSum = 0.0f;
    sessionSpeedSamples = 0;
    Serial.printf("\n>>> [EVENT] Fan Stopped! Duration: %s | Lifetime ON: %s <<<\n",
                  formatDuration(sessionDuration).c_str(), formatDuration(totalSavedRunningSeconds).c_str());
  }

  // 6. One-Second Tick for Fan Idle Time
  if (now - lastSecTickMillis >= 1000) {
    lastSecTickMillis = now;
    if (!isFanRunning) {
      totalSavedStoppedSeconds++;
    }
  }

  // 7. Periodic Fan NVS Backup while spinning
  if (isFanRunning && (now - lastFanNvsSaveMillis >= FAN_NVS_SAVE_INTERVAL_MS)) {
    unsigned long incrementalRunningSecs = (now - sessionStartMillis) / 1000;
    prefs.putULong("run_sec", totalSavedRunningSeconds + incrementalRunningSecs);
    prefs.putULong("pulses", totalSavedLifetimePulses + totalLivePulses);
    lastFanNvsSaveMillis = now;
  }

  // 8. Onboard LED Sync (Glows while fan is spinning)
  digitalWrite(PIN_LED, isFanRunning ? LOW : HIGH);

  // 9. Serial Monitor Telemetry
  if (now - lastSerialPrint >= 2000) {
    lastSerialPrint = now;
    Serial.printf("[BATTERY] %d%% (%.2fV) | Uptime: %s | Prev Run: %s | Fan: %4.0f RPM\n",
                  liveBatteryPercent,
                  liveBatteryVoltage,
                  formatDuration(currentUptimeSec).c_str(),
                  (lastBatteryRunDurationSec > 0 ? formatDuration(lastBatteryRunDurationSec).c_str() : "None"),
                  currentRPM);
  }
}
