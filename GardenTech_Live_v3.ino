// ============================================================
// Garden Tech — Live Firmware v3.1
// Board:  DFRobot FireBeetle ESP32-E (DFR0654)
// Sensor: DFRobot SEN0308 Waterproof Capacitive Soil Moisture
//
// BUTTON BEHAVIOUR (BOOT button = GPIO 0):
//   Hold for 3 seconds at any time → wipes saved WiFi and
//   restarts into setup mode (SoftAP "GardenTech-Setup").
//   Connect your phone to that network, open 192.168.4.1
//   and enter your home WiFi credentials.
//
// DATA FLOW:
//   Probe → Supabase (HTTPS) ← App (Vercel HTTPS)
//   Also serves /data locally for on-network access.
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

// --- BUTTON ---
const int BUTTON_PIN        = 0;
const unsigned long HOLD_MS = 3000;

// --- SOFTAP ---
const char* AP_SSID     = "GardenTech-Setup";
const char* AP_PASSWORD = "gardentech";

// --- SENSOR ---
const int SENSOR_PIN = A0;

// --- CALIBRATION ---
const int DRY_VALUE = 2800;
const int WET_VALUE = 1200;

// --- SUPABASE ---
const char* SUPABASE_URL = "https://umxwirxhdqnoquumuwtq.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_TnPbe7dEyTv-Wmxf9evrJA_RKb3IwGe";

// How often to push a reading to Supabase.
// 30 000 ms (30 s) for testing. Change to 900 000 (15 min) for production.
const unsigned long SUPABASE_INTERVAL_MS = 30000;

// --- TELEGRAM ---
const char* TG_BOT_TOKEN = "8899511521:AAEvpfXqJJl8w8Sy4ZYQpPz5-RaBWABac6I";
const char* TG_CHAT_ID   = "8800599722";

// --- ALERT TIMING ---
const unsigned long ALERT_INTERVAL_MS     = 20000;
const unsigned long SPRINKLER_SUPPRESS_MS = 4UL * 60 * 60 * 1000;
const int           SPRINKLER_JUMP        = 20;

// --- GLOBALS ---
Preferences prefs;
WebServer   server(80);

bool   wifiConnected    = false;
bool   apMode           = false;
String probeId          = "";   // MAC address — used as Supabase probe_id

int           lastMoisture      = -1;
String        lastStatus        = "";
unsigned long lastAlertMs       = 0;
unsigned long lastSupabaseMs    = 0;
unsigned long suppressUntilMs   = 0;

unsigned long btnPressedAt = 0;
bool          btnHeld      = false;

// ============================================================
// SERIAL HELPERS
// ============================================================
void printSep()                { Serial.println("------------------------------------------------------------"); }
void printHeader(const char* t){ printSep(); Serial.print("  "); Serial.println(t); printSep(); }
void logPass(const char* m)    { Serial.print("  [PASS] "); Serial.println(m); }
void logFail(const char* m)    { Serial.print("  [FAIL] "); Serial.println(m); }
void logInfo(const char* m)    { Serial.print("  [INFO] "); Serial.println(m); }

// ============================================================
// SENSOR
// ============================================================
int readRaw() {
  long sum = 0;
  for (int i = 0; i < 5; i++) { sum += analogRead(SENSOR_PIN); delay(10); }
  return sum / 5;
}

int toMoisture(int raw) {
  if (raw >= DRY_VALUE) return 0;
  if (raw <= WET_VALUE) return 100;
  return map(raw, DRY_VALUE, WET_VALUE, 0, 100);
}

String toStatus(int raw, int moisture) {
  if (raw == 0)       return "fault_floating";
  if (raw >= 2400)    return "dry";
  if (moisture < 40)  return "low";
  if (moisture <= 60) return "ideal";
  return "wet";
}

// ============================================================
// SUPABASE — push reading to cloud
// ============================================================
void postToSupabase(int moisture, int raw, String status) {
  if (!wifiConnected) return;

  WiFiClientSecure client;
  client.setInsecure(); // skip cert verify — fine for local IoT device

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/readings";
  http.begin(client, url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer",        "return=minimal");

  String body = "{\"probe_id\":\"" + probeId + "\","
                 "\"moisture\":"   + String(moisture) + ","
                 "\"raw\":"        + String(raw)      + ","
                 "\"status\":\""   + status + "\"}";

  int code = http.POST(body);
  if (code == 201) {
    Serial.println("  [Supabase] Reading saved.");
  } else {
    Serial.print("  [Supabase] Failed — HTTP "); Serial.println(code);
  }
  http.end();
}

// ============================================================
// TELEGRAM
// ============================================================
void sendTelegram(String message) {
  if (!wifiConnected) { Serial.println("  [TG] Skipped — no WiFi."); return; }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TG_BOT_TOKEN;
  url += "/sendMessage";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  message.replace("\"", "\\\"");
  String body = "{\"chat_id\":\"" + String(TG_CHAT_ID) +
                "\",\"text\":\"" + message +
                "\",\"parse_mode\":\"HTML\"}";
  int code = http.POST(body);
  Serial.print("  [TG] HTTP "); Serial.println(code);
  http.end();
}

// ============================================================
// ALERT LOGIC
// ============================================================
void handleAlerts(int moisture, String status) {
  unsigned long now = millis();
  bool suppressed = (now < suppressUntilMs);

  if (lastMoisture >= 0 && !suppressed) {
    int jump = moisture - lastMoisture;
    if (jump >= SPRINKLER_JUMP) {
      suppressUntilMs = now + SPRINKLER_SUPPRESS_MS;
      String msg = "💧 <b>Sprinkler detected!</b>\n";
      msg += "Moisture jumped " + String(lastMoisture) + "% → " + String(moisture) + "%\n";
      msg += "Alerts suppressed for 4 hours.";
      sendTelegram(msg);
      lastMoisture = moisture; lastStatus = status; lastAlertMs = now;
      return;
    }
  }

  if (suppressed) { lastMoisture = moisture; lastStatus = status; return; }

  if (status == "dry" && lastStatus != "dry" && lastStatus != "") {
    String msg = "⚠️ <b>Lawn is dry!</b>\nMoisture: " + String(moisture) + "%";
    sendTelegram(msg);
    lastAlertMs = now; lastMoisture = moisture; lastStatus = status;
    return;
  }

  if (now - lastAlertMs >= ALERT_INTERVAL_MS) {
    String emoji = "🌱";
    if (status == "dry")              emoji = "🌵";
    else if (status == "low")         emoji = "🌾";
    else if (status == "ideal")       emoji = "✅";
    else if (status == "wet")         emoji = "💧";
    else if (status == "fault_floating") emoji = "❌";
    String msg = emoji + " <b>Garden Tech</b>\nMoisture: " + String(moisture) + "%\nStatus: " + status;
    sendTelegram(msg);
    lastAlertMs = now;
  }

  lastMoisture = moisture; lastStatus = status;
}

// ============================================================
// CREDENTIALS
// ============================================================
String loadSSID()     { prefs.begin("wifi", true);  String v = prefs.getString("ssid", "");     prefs.end(); return v; }
String loadPassword() { prefs.begin("wifi", true);  String v = prefs.getString("password", ""); prefs.end(); return v; }

void saveCredentials(String ssid, String password) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.end();
  Serial.println("  [PREFS] Credentials saved.");
}

void clearCredentials() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  Serial.println("  [PREFS] Credentials cleared.");
}

// ============================================================
// SETUP PAGE HTML
// ============================================================
String buildSetupPage(String msg = "") {
  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Garden Tech Setup</title>"
    "<style>"
    "body{font-family:sans-serif;background:#0d1f0e;color:#e8f5e8;"
    "display:flex;flex-direction:column;align-items:center;padding:2rem;}"
    "h1{color:#3ecf4a;margin-bottom:0.25rem;}p{color:#6b9e6e;margin-bottom:2rem;}"
    "form{width:100%;max-width:360px;}"
    "label{font-size:0.85rem;color:#6b9e6e;display:block;margin-bottom:4px;}"
    "input{width:100%;padding:12px;margin-bottom:16px;border-radius:8px;"
    "border:1px solid #2d5a30;background:#142316;color:#e8f5e8;"
    "font-size:1rem;box-sizing:border-box;}"
    "button{width:100%;padding:14px;background:#2d7a35;color:#a8e6a3;"
    "border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;}"
    ".msg{padding:10px 14px;border-radius:8px;margin-bottom:16px;font-size:0.9rem;"
    "background:rgba(62,207,74,0.1);color:#3ecf4a;border:1px solid rgba(62,207,74,0.25);}"
    "</style></head><body>"
    "<h1>🌿 Garden Tech</h1>"
    "<p>Enter your home WiFi details below</p>";
  if (msg.length() > 0) html += "<div class='msg'>" + msg + "</div>";
  html += "<form method='POST' action='/save'>"
    "<label>WiFi Network Name (SSID)</label>"
    "<input type='text' name='ssid' placeholder='Your WiFi name' required>"
    "<label>WiFi Password</label>"
    "<input type='password' name='password' placeholder='Your WiFi password'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></body></html>";
  return html;
}

// ============================================================
// HTTP ROUTES
// ============================================================
void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET");
  server.sendHeader("Cache-Control", "no-cache");
  int raw      = readRaw();
  int moisture = toMoisture(raw);
  String status = toStatus(raw, moisture);
  // Include probe_id (MAC) so the app can associate this probe with Supabase data
  String json = "{\"raw\":"      + String(raw)      +
                ",\"moisture\":" + String(moisture)  +
                ",\"status\":\""  + status           +
                "\",\"probe_id\":\"" + probeId + "\"}";
  server.send(200, "application/json", json);
  Serial.print("  /data — raw:"); Serial.print(raw);
  Serial.print("  moisture:"); Serial.print(moisture); Serial.println("%");
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

void handleRoot() {
  if (apMode) {
    server.send(200, "text/html", buildSetupPage());
  } else {
    int raw = readRaw();
    int moisture = toMoisture(raw);
    String html = "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Garden Tech</title>"
      "<style>body{font-family:sans-serif;background:#1a2e1a;color:#e0f0e0;"
      "display:flex;flex-direction:column;align-items:center;padding:2rem;}"
      "h1{color:#4caf50;}p{font-size:1.2rem;}"
      "a{color:#81c784;}</style></head><body>"
      "<h1>🌿 Garden Tech</h1>"
      "<p>Moisture: <strong>" + String(moisture) + "%</strong></p>"
      "<p>Raw ADC: " + String(raw) + "</p>"
      "<p>Probe ID: " + probeId + "</p>"
      "<p><a href='/data'>/data (JSON for app)</a></p>"
      "<p><a href='/setup'>Change WiFi settings</a></p>"
      "<meta http-equiv='refresh' content='20'></body></html>";
    server.send(200, "text/html", html);
  }
}

void handleSetup() {
  server.send(200, "text/html", buildSetupPage());
}

void handleSave() {
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(200, "text/html", buildSetupPage("WiFi name cannot be empty."));
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  saveCredentials(ssid, pass);

  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>body{font-family:sans-serif;background:#0d1f0e;color:#e8f5e8;"
    "display:flex;flex-direction:column;align-items:center;justify-content:center;"
    "min-height:100vh;text-align:center;padding:2rem;}"
    "h1{color:#3ecf4a;}p{color:#6b9e6e;line-height:1.8;}</style></head><body>"
    "<h1>✅ Saved!</h1>"
    "<p>Reconnect your phone to your home WiFi.<br>"
    "The probe is restarting and connecting to <strong>" + ssid + "</strong>.<br><br>"
    "Open the Serial Monitor to find the probe's IP address,<br>"
    "then enter it in the Garden Tech app.</p>"
    "</body></html>";
  server.send(200, "text/html", html);
  delay(1500);
  ESP.restart();
}

// ============================================================
// NETWORK
// ============================================================
bool tryConnect(String ssid, String password) {
  Serial.print("  Connecting to: "); Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void startSoftAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4);
  printHeader("Setup Mode — SoftAP");
  Serial.print("  >> SSID: "); Serial.println(AP_SSID);
  Serial.print("  >> Password: "); Serial.println(AP_PASSWORD);
  Serial.print("  >> Open on your phone: http://"); Serial.println(WiFi.softAPIP());
}

void startNetwork() {
  printHeader("WiFi");
  String ssid = loadSSID();
  String pwd  = loadPassword();

  if (ssid.length() == 0) {
    logInfo("No saved credentials — starting setup mode.");
    startSoftAP();
    return;
  }

  if (tryConnect(ssid, pwd)) {
    wifiConnected = true;
    probeId = WiFi.macAddress(); // use MAC as stable probe identifier
    logPass("Connected to home WiFi.");
    Serial.print("  >> Probe IP:  "); Serial.println(WiFi.localIP());
    Serial.print("  >> Probe ID:  "); Serial.println(probeId);
    Serial.println("  >> Copy the IP into the Garden Tech app.");
    Serial.print("  >> RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else {
    logFail("Could not connect. Starting setup mode.");
    startSoftAP();
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("============================================================");
  Serial.println("  Garden Tech — Live Firmware v3.1");
  Serial.println("  Hold BOOT button 3 s to wipe WiFi and re-enter setup");
  Serial.println("============================================================");

  printHeader("ADC Check");
  int raw = readRaw();
  int moisture = toMoisture(raw);
  Serial.print("  Raw: "); Serial.print(raw);
  Serial.print("  Moisture: "); Serial.print(moisture); Serial.println("%");
  if (raw == 0)         logFail("Reading 0 — check yellow wire on A0.");
  else if (raw >= 2400) logPass("Dry-air range. Sensor wired OK.");
  else                  logInfo("Mid-range reading.");

  startNetwork();

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/setup",  HTTP_GET,  handleSetup);
  server.on("/save",   HTTP_POST, handleSave);
  server.on("/data",   HTTP_GET,  handleData);
  server.on("/data",   HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  printSep();
  if (wifiConnected) {
    Serial.print("  [INFO] Local endpoint: http://"); Serial.print(WiFi.localIP()); Serial.println("/data");
    Serial.println("  [INFO] Supabase push: every 30 seconds");
    String msg = "🟢 <b>Garden Tech online</b>\nIP: " + WiFi.localIP().toString() +
                 "\nProbe ID: " + probeId +
                 "\nMoisture: " + String(moisture) + "%";
    sendTelegram(msg);
    lastMoisture  = moisture;
    lastStatus    = toStatus(raw, moisture);
    lastAlertMs   = millis();
    lastSupabaseMs = millis();
    // Push first reading immediately
    postToSupabase(moisture, raw, toStatus(raw, moisture));
  } else {
    Serial.println("  [INFO] Setup page: http://192.168.4.1/");
  }
  printSep();
  Serial.println("  Ready. Hold BOOT 3 s to re-enter WiFi setup.");
  printSep();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();

  // --- Button hold (3 s → clear WiFi + restart) ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!btnHeld) {
      btnHeld = true;
      btnPressedAt = millis();
      Serial.println("  [BTN] Hold 3 s to enter WiFi setup...");
    } else if (millis() - btnPressedAt >= HOLD_MS) {
      Serial.println("  [BTN] Clearing WiFi credentials and restarting.");
      clearCredentials();
      delay(500);
      ESP.restart();
    }
  } else {
    btnHeld = false;
  }

  // --- Sensor cycle (every 5 s) ---
  static unsigned long lastCycle = 0;
  if (millis() - lastCycle >= 5000) {
    lastCycle = millis();
    int raw      = readRaw();
    int moisture = toMoisture(raw);
    float mv     = (raw / 4095.0f) * 3300.0f;
    String status = toStatus(raw, moisture);

    Serial.print("LIVE | raw:"); Serial.print(raw);
    Serial.print(" | "); Serial.print(mv / 1000.0f, 2); Serial.print("V");
    Serial.print(" | moisture:"); Serial.print(moisture);
    Serial.print("% | "); Serial.println(status);

    if (wifiConnected) {
      // Push to Supabase on its own interval
      if (millis() - lastSupabaseMs >= SUPABASE_INTERVAL_MS) {
        lastSupabaseMs = millis();
        postToSupabase(moisture, raw, status);
      }
      handleAlerts(moisture, status);
    }
  }
}
