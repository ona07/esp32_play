#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <INA226_WE.h>

// ---- Config (replace placeholders) ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
/// ====== Supabase (REST) ======
// 例: "https://PROJECT.supabase.co/rest/v1/protein_logs"
const char* SUPABASE_REST_URL = "https://PROJECT.supabase.co/rest/v1/protein_logs";
// Project API keys -> anon（RLSで insert/select を許可する場合）
const char* SUPABASE_API_KEY  = "SUPABASE_ANON_KEY";
// devices.id 等、識別したいID（任意）
const char* SUPABASE_DEVICE_ID = "esp32-ina-001";

constexpr unsigned long SAMPLE_INTERVAL_MS   = 5000UL;    // Sensor print interval
constexpr unsigned long UPLOAD_INTERVAL_MS   = 60000UL;  // 1 minute
constexpr unsigned long INA_RETRY_INTERVAL_MS = 5000UL;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 10000UL;
constexpr long TZ_OFFSET_SECONDS = 9 * 3600; // JST by default
constexpr float INA226_SHUNT_OHMS = 0.1f;     // 実際のシャント抵抗値に合わせて変更
constexpr uint8_t INA226_ADDR = 0x40;

// ---- NTP servers ----
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.google.com";
const char* NTP3 = "time.cloudflare.com";

RTC_DS3231 rtc;
INA226_WE ina226(INA226_ADDR);
WiFiClientSecure httpsClient;

bool rtcReady = false;
bool rtcAligned = false;
bool inaReady = false;

unsigned long lastSampleMs = 0;
unsigned long lastUploadMs = 0;
unsigned long lastInaRetryMs = 0;

DateTime fallbackStart;
unsigned long fallbackStartMs = 0;

String formatIso8601(const DateTime &dt, long offsetSeconds) {
  char buf[32];
  long totalMinutes = offsetSeconds / 60;
  char sign = '+';
  if (totalMinutes < 0) {
    sign = '-';
    totalMinutes = -totalMinutes;
  }
  int hh = totalMinutes / 60;
  int mm = totalMinutes % 60;
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second(),
           sign, hh, mm);
  return String(buf);
}

bool initRtc() {
  if (!rtc.begin()) {
    Serial.println(F("RTC not detected; using fallback clock."));
    rtcReady = false;
    return false;
  }
  rtcReady = true;
  if (rtc.lostPower()) {
    Serial.println(F("RTC lost power; setting compile time."));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  return true;
}

bool initIna() {
  if (!ina226.init()) {
    inaReady = false;
    Serial.println(F("INA226 not found. Check wiring/addr (default 0x40)."));
    return false;
  }
  ina226.setAverage(INA226_AVERAGE_16);
  ina226.setConversionTime(INA226_CONV_TIME_1100);
  ina226.setMeasureMode(INA226_CONTINUOUS);
  inaReady = true;
  Serial.println(F("INA226 detected and configured (using library defaults for shunt/current range)."));
  return true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("WiFi connecting"));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi connect timeout."));
  }
}

bool syncTimeFromNtp() {
  configTime(TZ_OFFSET_SECONDS, 0, NTP1, NTP2, NTP3);
  struct tm tmInfo;
  unsigned long t0 = millis();
  while (!getLocalTime(&tmInfo, 500) && millis() - t0 < NTP_SYNC_TIMEOUT_MS) {
    delay(200);
  }
  if (!getLocalTime(&tmInfo, 500)) {
    Serial.println(F("NTP sync failed."));
    return false;
  }
  if (rtcReady) {
    rtc.adjust(DateTime(
      tmInfo.tm_year + 1900,
      tmInfo.tm_mon + 1,
      tmInfo.tm_mday,
      tmInfo.tm_hour,
      tmInfo.tm_min,
      tmInfo.tm_sec));
    rtcAligned = true;
    Serial.println(F("RTC synced from NTP."));
  }
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  Serial.print(F("Time synced: "));
  Serial.println(buf);
  return true;
}

DateTime currentTime(unsigned long nowMs) {
  struct tm tmInfo;
  if (getLocalTime(&tmInfo, 200)) {
    return DateTime(
      tmInfo.tm_year + 1900,
      tmInfo.tm_mon + 1,
      tmInfo.tm_mday,
      tmInfo.tm_hour,
      tmInfo.tm_min,
      tmInfo.tm_sec);
  }
  if (rtcReady) {
    return rtc.now();
  }
  if (fallbackStartMs == 0) {
    fallbackStart = DateTime(F(__DATE__), F(__TIME__));
    fallbackStartMs = nowMs;
  }
  uint32_t elapsed = (nowMs - fallbackStartMs) / 1000U;
  return fallbackStart + TimeSpan(elapsed);
}

bool sendSupabasePower(const DateTime &ts, float voltageV, float currentmA, float powerMw,
                       int &httpCode, String &response) {
  httpCode = 0;
  response = "";
  if (WiFi.status() != WL_CONNECTED) {
    httpCode = -1;
    response = "wifi not connected";
    return false;
  }
  HTTPClient http;
  httpsClient.setInsecure(); // omit certificate pinning for simplicity
  httpsClient.setTimeout(10000);
  if (!http.begin(httpsClient, SUPABASE_REST_URL)) {
    httpCode = -2;
    response = "http.begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
  http.addHeader("Prefer", "return=minimal");
  String body; body.reserve(200);
  body += '{';
  body += "\"device_id\":\""; body += SUPABASE_DEVICE_ID; body += "\",";
  body += "\"voltage_v\":"; body += String(voltageV, 3); body += ',';
  body += "\"current_ma\":"; body += String(currentmA, 2); body += ',';
  body += "\"power_mw\":"; body += String(powerMw, 2); body += ',';
  body += "\"recorded_at\":\""; body += formatIso8601(ts, TZ_OFFSET_SECONDS); body += "\"";
  body += '}';
  httpCode = http.POST(body);
  if (httpCode > 0) {
    response = http.getString();
  }
  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(); // ESP32 default SDA=21, SCL=22

  initRtc();
  initIna();
  connectWiFi();
  syncTimeFromNtp();

  lastSampleMs = millis();
  lastUploadMs = millis();
  lastInaRetryMs = millis();

  Serial.println(F("ESP32 INA226 + RTC + Supabase monitor ready."));
}

void loop() {
  unsigned long nowMs = millis();

  if (!inaReady && (nowMs - lastInaRetryMs >= INA_RETRY_INTERVAL_MS)) {
    initIna();
    lastInaRetryMs = nowMs;
  }

  if (nowMs - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = nowMs;
    if (inaReady) {
      float v = ina226.getBusVoltage_V();
      float c = ina226.getCurrent_mA();
      float p = v * c; // mW (V * mA)
      if (!isnan(v) && !isnan(c) && !isnan(p)) {
        DateTime ts = currentTime(nowMs);
        Serial.printf("%04d-%02d-%02d %02d:%02d:%02d | V=%.3f V I=%.2f mA P=%.2f mW\n",
                      ts.year(), ts.month(), ts.day(),
                      ts.hour(), ts.minute(), ts.second(),
                      v, c, p);
      } else {
        Serial.println(F("INA226 read failed."));
      }
    } else {
      Serial.println(F("INA226 unavailable; retrying..."));
    }
  }

  if (nowMs - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs = nowMs;
    connectWiFi();
    if (inaReady) {
      float v = ina226.getBusVoltage_V();
      float c = ina226.getCurrent_mA();
      float p = v * c; // mW
      if (!isnan(v) && !isnan(c) && !isnan(p)) {
        DateTime ts = currentTime(nowMs);
        int httpCode = 0;
        String response;
        bool ok = sendSupabasePower(ts, v, c, p, httpCode, response);
        Serial.printf("[SUPABASE] V=%.3fV I=%.2fmA P=%.2fmW %s (http=%d)\n",
                      v, c, p, ok ? "OK" : "NG", httpCode);
        if (!ok && response.length() > 0) {
          Serial.printf("[SUPABASE] response: %s\n", response.c_str());
        }
      } else {
        Serial.println(F("[SUPABASE] skipped (INA read failed)"));
      }
    } else {
      Serial.println(F("[SUPABASE] skipped (INA not ready)"));
    }
  }

  delay(10);
}
