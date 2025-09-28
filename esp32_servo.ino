#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

/// ====== Wi-Fi ======
const char* WIFI_SSID = "aterm-ea809c-g";
const char* WIFI_PASS = "636bed4138a9f";

/// ====== Supabase 認証情報 ======
const char* ANON_KEY     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Imtwamt4enNka255bW9wd3BwcGZwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTY1MjY0ODgsImV4cCI6MjA3MjEwMjQ4OH0.n1o1c50_iTL2rRYTAnITONXP4sma0gSl5xyihapArMI";   // API -> Project API keys -> anon
const char* FANBOT_TOKEN = "switch";                   // Functions Secretsに設定した値
const char* DEVICE_ID    = "d08a3b7c-bff1-4952-97f0-9293fcc07e5d"; // devices.id
const char* DEVICE_API_KEY = "a0b539741901d1688bc2a536343e7809";   // devices.api_key

/// ====== Supabase Edge Function ======
const char* EDGE_HOST = "kpjkxzsdknymopwpppfp.supabase.co";
const int   EDGE_PORT = 443;
const char* EDGE_PATH = "/functions/v1/fanbot_event";

/// ====== mDNS ======
const char* MDNS_NAME = "fanbot"; // http://fanbot.local/

/// ====== サーボ設定 ======
static const int SERVO_PIN = 18;
static const int FREQ_HZ   = 50;
static const int PULSE_MIN = 500;
static const int PULSE_MAX = 2400;
int angle_idle     = 90;
int angle_press_on = 120;
int angle_press_off = 60;
int press_hold_ms = 600;
int cooldown_ms   = 800;

Servo servo;
unsigned long lastAction = 0;

/// ====== HTTP サーバ ======
WebServer server(80);

/// ====== ヘッダ収集 ======
const char* headerKeys[] = { "x-fanbot-token", "X-Fanbot-Token" };

/// ====== JST タイムスタンプ生成 ======
String getTimestampISO8601() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
  String ts(buf);
  // %z は +0900 形式なので +09:00 に整形
  if (ts.length() >= 5) {
    ts = ts.substring(0, ts.length()-2) + ":" + ts.substring(ts.length()-2);
  }
  return ts;
}

/// ====== action_id を作る ======
String makeActionId(const String& cmd, const String& ts) {
  return ts + "-" + DEVICE_ID + "-" + cmd + "-" + String((uint32_t)random(1, 99999));
}

/// ====== Supabase POST ======
bool postResultToSupabase(const String& action_id, const String& cmd, bool executed, const String& errorMsg, const String& timestamp) {
  WiFiClientSecure https;
  https.setInsecure(); // プロトタイプ用。本番は証明書ピンニング推奨

  if (!https.connect(EDGE_HOST, EDGE_PORT)) {
    Serial.println("[POST] connect failed");
    return false;
  }

  // JSON組み立て
  StaticJsonDocument<400> doc;
  doc["device_id"] = DEVICE_ID;
  doc["command"]   = cmd;
  doc["executed"]  = executed;
  doc["timestamp"] = timestamp;
  if (errorMsg.length()) doc["error"] = errorMsg;
  if (action_id.length()) doc["action_id"] = action_id;

  String payload;
  serializeJson(doc, payload);

  // HTTPリクエスト送信
  String req;
  req.reserve(500 + payload.length());
  req  = "POST " + String(EDGE_PATH) + " HTTP/1.1\r\n";
  req += "Host: " + String(EDGE_HOST) + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Authorization: Bearer " + String(ANON_KEY) + "\r\n";
  req += "x-fanbot-token: " + String(FANBOT_TOKEN) + "\r\n";
  req += "x-device-id: " + String(DEVICE_ID) + "\r\n";
  req += "x-device-api-key: " + String(DEVICE_API_KEY) + "\r\n";
  req += "Content-Length: " + String(payload.length()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += payload;

  https.print(req);

  // ステータス行
String status = https.readStringUntil('\n');
status.trim();
Serial.println("[POST] status=" + status);

// ← 追加：レスポンス本文も全部読む
String body;
while (https.connected() || https.available()) {
  String line = https.readStringUntil('\n');
  body += line + "\n";
}
Serial.println("[POST] body:\n" + body);

return status.startsWith("HTTP/1.1 200");
}

/// ====== サーボ動作 ======
bool runServoOnce(const String& cmd, String& err) {
  int targetAngle = angle_press_on;
  if (cmd == "off") targetAngle = angle_press_off;

  servo.write(targetAngle);
  delay(press_hold_ms);
  servo.write(angle_idle);
  return true; // 今は常に成功とみなす
}

/// ====== コマンド適用 + Supabase送信 ======
bool applyCommandAndReport(const String& cmd, String& err) {
  if (cmd != "on" && cmd != "off") { err = "invalid command"; return false; }

  unsigned long now = millis();
  if (now - lastAction < (unsigned long)cooldown_ms) {
    err = "cooldown";
    String ts = getTimestampISO8601();
    String action_id = makeActionId(cmd, ts);
    for (int i=0;i<3;i++) {
      if (postResultToSupabase(action_id, cmd, false, err, ts)) break;
      delay(300 * (1<<i));
    }
    return false;
  }

  Serial.printf("[CMD] %s\n", cmd.c_str());
  String ts = getTimestampISO8601();
  String action_id = makeActionId(cmd, ts);

  bool ok = runServoOnce(cmd, err);

  for (int i=0;i<3;i++) {
    if (postResultToSupabase(action_id, cmd, ok, ok ? "" : err, ts)) break;
    delay(300 * (1<<i));
  }

  if (ok) lastAction = millis();
  return ok;
}

/// ====== 認証 ======
bool authorized() {
  for (auto key : headerKeys) {
    if (server.hasHeader(key)) {
      if (server.header(key) == FANBOT_TOKEN) return true;
    }
  }
  if (server.hasArg("token") && server.arg("token") == FANBOT_TOKEN) return true;
  return false;
}

/// ====== ハンドラ ======
void handleHealth() {
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCmd() {
  if (!authorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (!server.hasArg("c")) {
    server.send(400, "application/json", "{\"error\":\"missing c\"}");
    return;
  }
  String c = server.arg("c");
  c.toLowerCase();

  String err;
  bool ok = applyCommandAndReport(c, err);
  if (!ok) {
    server.send(400, "application/json", String("{\"ok\":false,\"error\":\"") + err + "\"}");
    return;
  }
  server.send(200, "application/json", String("{\"ok\":true,\"command\":\"") + c + "\"}");
}

void handleRoot() {
  String html =
    "<!doctype html><meta charset='utf-8'>"
    "<h1>FanBot HTTP</h1>"
    "<button onclick=\"send('on')\">ON</button> "
    "<button onclick=\"send('off')\">OFF</button>"
    "<pre id=log></pre>"
    "<script>"
    "async function send(c){"
      "const r=await fetch('/cmd?c='+c,{headers:{'x-fanbot-token':'" + String(FANBOT_TOKEN) + "'}});"
      "document.getElementById('log').textContent = await r.text();"
    "}"
    "</script>";
  server.send(200, "text/html", html);
}

/// ====== setup ======
void setup() {
  Serial.begin(115200);
  delay(200);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Wi-Fi connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // NTP (JST)
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");

  // サーボ初期化
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  servo.setPeriodHertz(FREQ_HZ);
  servo.attach(SERVO_PIN, PULSE_MIN, PULSE_MAX);
  servo.write(angle_idle);

  // mDNS
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
  }

  server.collectHeaders(headerKeys, sizeof(headerKeys)/sizeof(headerKeys[0]));
  server.on("/", handleRoot);
  server.on("/healthz", handleHealth);
  server.on("/cmd", handleCmd);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
