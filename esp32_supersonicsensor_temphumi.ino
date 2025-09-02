#include <Arduino.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ---- Pins ----
const uint8_t TRIG_PIN = 5;
const uint8_t ECHO_PIN = 16;
const uint8_t LED_PIN  = 23;
const uint8_t DHT_PIN  = 4;     // DHT11 DATA
#define DHTTYPE DHT11

// ---- WiFi / API Config ----
// NOTE: 必ず自分の Wi-Fi / API キーに書き換えてください
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* API_BASE  = "https://esp32-play.onrender.com"; // 末尾スラッシュ無し
const char* API_KEY   = "REPLACE_WITH_DEVICE_API_KEY";     // サーバの devices.api_key

// ---- Timing / Logic Config ----
static const unsigned long PULSE_TIMEOUT_US   = 30000UL;
static const unsigned long DHT_MIN_INTERVALMS = 2000UL;  // DHT11は>=1s推奨（読み取り間隔の下限）
static const unsigned long LOOP_INTERVAL_MS   = 500UL;   // 距離更新周期
static const unsigned long PERIODIC_SEND_MS   = 30000UL; // 予備: 未同期時の簡易間隔（30s）
static const float DIST_WORK_THRESHOLD_CM     = 20.0f;    // 作業中の閾値
static const float DEFAULT_TEMP_C = 20.0f;

static const unsigned long HTTP_TIMEOUT_MS    = 7000UL;

DHT dht(DHT_PIN, DHTTYPE);
float lastTempC = NAN, lastHum = NAN;
unsigned long lastDhtReadMs = 0;
unsigned long lastPeriodicSendMs = 0;
bool lastWorkState = false;       // false: 離席中, true: 作業中
bool workStateInited = false;

WiFiClientSecure https;
// NTP / 時刻同期
const long GMT_OFFSET_SEC = 9 * 3600;   // JST (+09:00) に合わせる（環境に応じて変更可）
const int  DAYLIGHT_OFFSET_SEC = 0;
bool timeIsSynced = false;
long lastPeriodicSlot = -1; // 30分スロット単位の最終送信スロット

// ---- Helpers ----
String jsonEscape(const String &s) {
  String out; out.reserve(s.length()+8);
  for (size_t i=0;i<s.length();++i) {
    char c = s[i];
    if (c=='"' || c=='\\') { out += '\\'; out += c; }
    else if (c=='\n') out += "\\n";
    else out += c;
  }
  return out;
}

bool httpPostIngest(const String &json) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String(API_BASE) + "/ingest";
  https.setInsecure(); // 証明書検証を省略（必要なら正しいルート証明書を設定）
  https.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(https, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int code = http.POST(json);
  http.end();
  return (code >= 200 && code < 300);
}

bool sendDistance(float cm, const char* cause, const char* zone=nullptr) {
  if (isnan(cm)) return false;
  String body;
  body.reserve(160);
  body += "[";
  body += "{\"metric\":\"distance_ultrasonic\",\"value\":";
  body += String(cm, 2);
  body += ",\"meta\":{\"src\":\"esp32\",\"cause\":\"";
  body += jsonEscape(String(cause));
  body += "\"";
  if (zone && zone[0]) {
    body += ",\"zone\":\"";
    body += jsonEscape(String(zone));
    body += "\"";
  }
  body += "}}]";
  return httpPostIngest(body);
}

bool sendPeriodicAll(float dist_cm, float temp_c, float hum, bool working) {
  // current values as a single POST array
  String body; body.reserve(256);
  body += '[';
  bool first = true;
  auto pushComma = [&](){ if(!first){ body += ','; } else { first=false; } };
  // humidity
  if (!isnan(hum)) {
    pushComma();
    body += "{\"metric\":\"humidity\",\"value\":"; body += String(hum,1);
    body += ",\"meta\":{\"src\":\"esp32\",\"cause\":\"periodic\"}}";
  }
  // temperature
  if (!isnan(temp_c)) {
    pushComma();
    body += "{\"metric\":\"temperature\",\"value\":"; body += String(temp_c,2);
    body += ",\"meta\":{\"src\":\"esp32\",\"cause\":\"periodic\"}}";
  }
  // distance
  if (!isnan(dist_cm)) {
    pushComma();
    body += "{\"metric\":\"distance_ultrasonic\",\"value\":"; body += String(dist_cm,2);
    body += ",\"meta\":{\"src\":\"esp32\",\"cause\":\"periodic\",\"zone\":\"";
    body += working ? "UNDER_OR_EQ_20cm" : "OVER_GT_20cm";
    body += "\"}}";
  }
  body += ']';
  if (first) return true; // nothing to send
  return httpPostIngest(body);
}

// 音速(cm/μs)
static inline float soundSpeed_cm_per_us(float tempC) {
  const float c_ms = 331.3f + 0.606f * tempC;
  return (c_ms * 100.0f) / 1e6f;
}

// 超音波1回測距（cm）
float measureOnceCm(float tempC) {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long dur = pulseIn(ECHO_PIN, HIGH, PULSE_TIMEOUT_US);
  if (dur == 0) return NAN;
  return dur * soundSpeed_cm_per_us(tempC) * 0.5f;
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  dht.begin();

  // WiFi connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000UL) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: "); Serial.println(WiFi.localIP());
    // NTP 時刻同期
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm tmInfo;
    unsigned long t1 = millis();
    while (!getLocalTime(&tmInfo) && millis() - t1 < 10000UL) {
      delay(200);
    }
    timeIsSynced = getLocalTime(&tmInfo);
    if (timeIsSynced) {
      char buf[40];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
      Serial.print("Time synced: "); Serial.println(buf);
      // 初期スロットを記録（直後の重複送信を避ける）
      time_t nowEpoch = time(nullptr);
      lastPeriodicSlot = (nowEpoch >= 0) ? (nowEpoch / 1800) : -1;
    } else {
      Serial.println("Time sync failed; fallback to interval timer");
    }
  } else {
    Serial.println("WiFi connect timeout");
  }
}

void loop() {
  const unsigned long now = millis();

  // DHT読み取り（2秒間隔）
  if (now - lastDhtReadMs >= DHT_MIN_INTERVALMS) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h)) lastHum = h;
    if (!isnan(t)) lastTempC = t;
    lastDhtReadMs = now;
  }

  // 温度が未取得ならデフォルト
  float tempForCalc = isnan(lastTempC) ? DEFAULT_TEMP_C : lastTempC;

  // 距離計測
  float d_cm = measureOnceCm(tempForCalc);

  // 出力＆LED
  if (isnan(d_cm)) {
    Serial.println("Distance: NaN, DHT: "
                   + String(isnan(lastHum) ? -1 : lastHum, 1) + "% "
                   + String(isnan(lastTempC) ? DEFAULT_TEMP_C : lastTempC, 1) + "C");
    digitalWrite(LED_PIN, LOW);
  } else {
    float tC = isnan(lastTempC) ? DEFAULT_TEMP_C : lastTempC;
    float h  = isnan(lastHum) ? -1 : lastHum;
    Serial.printf("Humidity: %.1f %%\tTemperature: %.2f *C (%.2f *F)\tDistance: %.2f cm\n",
                  h, tC, tC * 1.8f + 32.0f, d_cm);
    // LED threshold aligned to work threshold (20cm)
    digitalWrite(LED_PIN, (d_cm <= DIST_WORK_THRESHOLD_CM) ? HIGH : LOW);

    // --- 作業状態（20cm閾値） ---
    bool working = (d_cm <= DIST_WORK_THRESHOLD_CM);
    if (!workStateInited) {
      const char* zone = working ? "UNDER_OR_EQ_20cm" : "OVER_GT_20cm";
      bool ok = sendDistance(d_cm, "boot", zone);
      Serial.printf("[SEND][BOOT] %.2f cm (%s) %s\n", d_cm, zone, ok?"OK":"NG");
      lastWorkState = working;
      workStateInited = true;
    }

    // 1) 境界イベント送信（状態変化時に1回）
    if (working != lastWorkState) {
      const char* zone = working ? "UNDER_OR_EQ_20cm" : "OVER_GT_20cm";
      bool ok = sendDistance(d_cm, "edge", zone);
      Serial.printf("[SEND][EDGE] %.2f cm -> %s %s\n", d_cm, zone, ok?"OK":"NG");
      lastWorkState = working;
    }

    // 2) 定期送信（00分 / 30分）
    bool shouldSendPeriodic = false;
    if (timeIsSynced) {
      time_t nowEpoch = time(nullptr);
      if (nowEpoch > 0) {
        long slot = nowEpoch / 1800; // 30分=1800秒単位
        if (slot != lastPeriodicSlot) {
          // 新しいスロットに切り替わった瞬間に1回だけ送信
          shouldSendPeriodic = true;
          lastPeriodicSlot = slot;
        }
      }
    } else {
      // 時刻未同期時は予備の一定間隔で送信
      static unsigned long lastPeriodicSendMs = 0;
      if (millis() - lastPeriodicSendMs >= PERIODIC_SEND_MS) {
        shouldSendPeriodic = true;
        lastPeriodicSendMs = millis();
      }
    }

    if (shouldSendPeriodic) {
      float tCsend = isnan(lastTempC) ? DEFAULT_TEMP_C : lastTempC;
      float hsend  = isnan(lastHum) ? NAN : lastHum;
      bool ok = sendPeriodicAll(d_cm, tCsend, hsend, working);
      Serial.printf("[SEND][PERIODIC] T=%.2fC H=%.1f%% D=%.2fcm %s\n", tCsend, hsend, d_cm, ok?"OK":"NG");
    }
  }

  delay(LOOP_INTERVAL_MS);
}
