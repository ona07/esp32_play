#include <Arduino.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <cstring>

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
// static const unsigned long PERIODIC_SEND_MS   = 30000UL; // 未使用: 未同期フォールバックは行わない
static const float DIST_WORK_THRESHOLD_CM     = 20.0f;    // 作業中の閾値
static const float DEFAULT_TEMP_C = 20.0f;

static const unsigned long HTTP_TIMEOUT_MS    = 7000UL;

// ---- LCD Monitor Config ----
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;
constexpr int LCD_SDA_PIN = 21;
constexpr int LCD_SCL_PIN = 22;
constexpr unsigned long LCD_UPDATE_INTERVAL_MS = 1000UL;
constexpr uint8_t LCD_CUSTOM_CHAR_BASE = 1;

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
unsigned long lastTimeSyncAttemptMs = 0; // ループ内での再同期トライ用

RTC_DS3231 rtc;
bool rtcAvailable = false;
bool rtcAlignedFromNtp = false;
bool fallbackClockActive = false;
DateTime fallbackStartTime;
unsigned long fallbackStartMillis = 0;
constexpr uint32_t kLocalTimePollTimeoutMs = 5;
constexpr unsigned long RTC_RESYNC_INTERVAL_MS = 600000UL;  // 10 minutes
unsigned long lastRtcResyncMs = 0;

LiquidCrystal_I2C *lcd = nullptr;
unsigned long lastLcdUpdateMs = 0;
bool lcdAnnounced = false;

constexpr uint8_t LCD_ICON_HUMIDITY = LCD_CUSTOM_CHAR_BASE + 0;
constexpr uint8_t LCD_ICON_TEMP = LCD_CUSTOM_CHAR_BASE + 1;
constexpr uint8_t LCD_ICON_STATE = LCD_CUSTOM_CHAR_BASE + 2;

const uint8_t LCD_CUSTOM_CHARS[][8] = {
    {0b00000, 0b00100, 0b01010, 0b01010, 0b10001, 0b10001, 0b10001, 0b01110},  // droplet
    {0b00100, 0b00100, 0b00100, 0b00100, 0b01110, 0b11111, 0b11111, 0b01110},  // thermometer
    {0b00100, 0b01110, 0b00100, 0b01110, 0b10101, 0b00100, 0b01010, 0b10001},  // person
};
constexpr size_t LCD_CUSTOM_CHAR_COUNT = sizeof(LCD_CUSTOM_CHARS) / sizeof(LCD_CUSTOM_CHARS[0]);

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

uint8_t detectLcdAddress() {
  const uint8_t commonAddresses[] = {0x27, 0x3F, 0x20, 0x3E, 0x38};
  for (uint8_t addr : commonAddresses) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      return addr;
    }
  }
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      return addr;
    }
  }
  return 0;
}

void lcdPrintLine(uint8_t row, const char *text) {
  if (!lcd) return;
  char buffer[LCD_COLS + 1];
  snprintf(buffer, sizeof(buffer), "%-16s", text);
  lcd->setCursor(0, row);
  lcd->print(buffer);
}

void initLcd() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  uint8_t address = detectLcdAddress();
  if (address == 0) {
    Serial.println("LCD1602 not found on I2C bus.");
    return;
  }
  lcd = new LiquidCrystal_I2C(address, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  for (uint8_t i = 0; i < LCD_CUSTOM_CHAR_COUNT; ++i) {
    lcd->createChar(LCD_CUSTOM_CHAR_BASE + i, const_cast<uint8_t *>(LCD_CUSTOM_CHARS[i]));
  }
  lcdPrintLine(0, "ESP32 Monitor");
  lcdPrintLine(1, "Booting sensors");
  lcdAnnounced = false;
  lastLcdUpdateMs = millis() - LCD_UPDATE_INTERVAL_MS;
}

void initRtc() {
  if (!rtc.begin()) {
    Serial.println("RTC not detected; using fallback clock.");
    rtcAvailable = false;
    fallbackClockActive = true;
    fallbackStartTime = DateTime(F(__DATE__), F(__TIME__));
    fallbackStartMillis = millis();
    return;
  }

  rtcAvailable = true;
  rtcAlignedFromNtp = false;
  fallbackClockActive = false;

  if (rtc.lostPower()) {
    Serial.println("RTC lost power; loading compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

bool tryGetLocalDateTime(DateTime &out) {
  struct tm tmInfo;
  if (getLocalTime(&tmInfo, kLocalTimePollTimeoutMs)) {
    out = DateTime(
        tmInfo.tm_year + 1900,
        tmInfo.tm_mon + 1,
        tmInfo.tm_mday,
        tmInfo.tm_hour,
        tmInfo.tm_min,
        tmInfo.tm_sec);
    return true;
  }
  return false;
}

DateTime currentClockTime(unsigned long nowMillis) {
  DateTime localNow;
  if (tryGetLocalDateTime(localNow)) {
    return localNow;
  }

  if (rtcAvailable) {
    return rtc.now();
  }

  if (!fallbackClockActive) {
    fallbackClockActive = true;
    fallbackStartTime = DateTime(F(__DATE__), F(__TIME__));
    fallbackStartMillis = nowMillis;
  }

  uint32_t elapsedSeconds = (nowMillis - fallbackStartMillis) / 1000U;
  return fallbackStartTime + TimeSpan(elapsedSeconds);
}

void syncClocksFromEpoch(time_t epoch, unsigned long nowMillis) {
  if (epoch <= 0) {
    return;
  }

  if (rtcAvailable) {
    DateTime target(static_cast<uint32_t>(epoch));
    DateTime currentRtc = rtc.now();
    long diff = static_cast<long>(currentRtc.unixtime() - epoch);
    if (!rtcAlignedFromNtp || diff > 2 || diff < -2) {
      rtc.adjust(target);
      Serial.println("RTC synced from NTP.");
    }
    rtcAlignedFromNtp = true;
    fallbackClockActive = false;
  } else {
    fallbackClockActive = true;
    fallbackStartTime = DateTime(static_cast<uint32_t>(epoch));
    fallbackStartMillis = nowMillis;
  }
}

void maybeRefreshRtc(unsigned long nowMillis) {
  if (!rtcAvailable) {
    return;
  }
  if (!timeIsSynced) {
    return;
  }
  if (nowMillis - lastRtcResyncMs < RTC_RESYNC_INTERVAL_MS) {
    return;
  }
  time_t nowEpoch = time(nullptr);
  if (nowEpoch > 0) {
    syncClocksFromEpoch(nowEpoch, nowMillis);
    lastRtcResyncMs = nowMillis;
  }
}

void formatSensorValue(char *out, size_t len, float value, float minAllowed, float maxAllowed) {
  if (len < 5) {
    if (len > 0) {
      out[0] = '\0';
    }
    return;
  }

  if (isnan(value)) {
    strncpy(out, "--.-", len);
    out[len - 1] = '\0';
    return;
  }

  if (value < minAllowed) value = minAllowed;
  if (value > maxAllowed) value = maxAllowed;
  snprintf(out, len, "%4.1f", value);
}

void updateLcd(float distanceCm, float tempC, float humidity, bool working) {
  if (!lcd) return;
  unsigned long now = millis();
  if (now - lastLcdUpdateMs < LCD_UPDATE_INTERVAL_MS) {
    return;
  }
  lastLcdUpdateMs = now;

  DateTime clockTime = currentClockTime(now);
  char timeBuf[9];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
           clockTime.hour(), clockTime.minute(), clockTime.second());

  char distBuf[5];
  formatSensorValue(distBuf, sizeof(distBuf), distanceCm, 0.0f, 99.9f);
  char tempBuf[5];
  formatSensorValue(tempBuf, sizeof(tempBuf), tempC, -9.9f, 99.9f);
  char humBuf[5];
  formatSensorValue(humBuf, sizeof(humBuf), humidity, 0.0f, 99.9f);

  uint8_t line0[LCD_COLS];
  uint8_t line1[LCD_COLS];
  memset(line0, ' ', sizeof(line0));
  memset(line1, ' ', sizeof(line1));

  line0[0] = LCD_ICON_STATE;
  line0[1] = static_cast<uint8_t>(working ? 'W' : 'A');
  memcpy(&line0[2], timeBuf, 8);
  line0[10] = static_cast<uint8_t>(' ');
  line0[11] = static_cast<uint8_t>('D');
  memcpy(&line0[12], distBuf, 4);

  line1[0] = LCD_ICON_TEMP;
  memcpy(&line1[1], tempBuf, 4);
  line1[5] = static_cast<uint8_t>('C');
  line1[6] = static_cast<uint8_t>(' ');
  line1[7] = LCD_ICON_HUMIDITY;
  memcpy(&line1[8], humBuf, 4);
  line1[12] = static_cast<uint8_t>('%');

  lcd->setCursor(0, 0);
  for (uint8_t i = 0; i < LCD_COLS; ++i) {
    lcd->write(line0[i]);
  }
  lcd->setCursor(0, 1);
  for (uint8_t i = 0; i < LCD_COLS; ++i) {
    lcd->write(line1[i]);
  }

  if (!lcdAnnounced) {
    Serial.println("LCD1602 monitor initialized.");
    lcdAnnounced = true;
  }
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
  initLcd();
  initRtc();
  lcdPrintLine(1, "WiFi connecting");

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
    lcdPrintLine(1, "WiFi connected");
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
      syncClocksFromEpoch(nowEpoch, millis());
    } else {
      Serial.println("Time sync failed; will retry periodically and only send on 00/30 once synced");
    }
  } else {
    Serial.println("WiFi connect timeout");
    lcdPrintLine(1, "WiFi timeout");
  }
  updateLcd(NAN, NAN, NAN, false);
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
  float displayTempC = isnan(lastTempC) ? NAN : lastTempC;
  float displayHum = isnan(lastHum) ? NAN : lastHum;
  bool working = (!isnan(d_cm) && (d_cm <= DIST_WORK_THRESHOLD_CM));

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
    digitalWrite(LED_PIN, working ? HIGH : LOW);

    // --- 作業状態（20cm閾値） ---
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

    // 2) 定期送信（00分 / 30分のみ）
    //    未同期時は一切送信しない。ループ内で定期的に再同期を試みる。
    //    同期が成功したら、その時点のスロットを記録して重複送信を防ぐ。
    // 再同期トライ
    if (!timeIsSynced && (millis() - lastTimeSyncAttemptMs >= 5000UL)) {
      struct tm tmInfo;
      if (getLocalTime(&tmInfo, 1000)) {
        timeIsSynced = true;
        time_t nowEpoch = time(nullptr);
        lastPeriodicSlot = (nowEpoch > 0) ? (nowEpoch / 1800) : -1;
        Serial.println("Time synced (retry): periodic sends will align to 00/30.");
        syncClocksFromEpoch(nowEpoch, millis());
      }
      lastTimeSyncAttemptMs = millis();
    }

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
    }

    if (shouldSendPeriodic) {
      float tCsend = isnan(lastTempC) ? DEFAULT_TEMP_C : lastTempC;
      float hsend  = isnan(lastHum) ? NAN : lastHum;
      bool ok = sendPeriodicAll(d_cm, tCsend, hsend, working);
      Serial.printf("[SEND][PERIODIC] T=%.2fC H=%.1f%% D=%.2fcm %s\n", tCsend, hsend, d_cm, ok?"OK":"NG");
    }
  }

  updateLcd(d_cm, displayTempC, displayHum, working);
  maybeRefreshRtc(now);

  delay(LOOP_INTERVAL_MS);
}
