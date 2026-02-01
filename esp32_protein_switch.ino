/**
 * ESP32: ボタンが押されたら Wi-Fi 接続を開始し、接続成功したら 1 秒だけ LED を点灯するサンプル
 *
 * 配線（おすすめ: 内部プルアップ方式）
 * - SWITCH_PIN(例: GPIO13) ---[スイッチ]--- GND
 * - LED は LED_BUILTIN（多くのボードで GPIO2）を使用。外付けLEDの場合は LED_PIN を変更。
 *
 * 動作
 * - INPUT_PULLUP を使うので、未押下=HIGH / 押下=LOW
 * - ボタンが押されたら Wi-Fi 接続を開始（未接続時のみ）
 * - 接続に成功したら LED を 1 秒だけ点灯
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

/// ====== Wi-Fi ======
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

/// ====== Supabase (REST) ======
// 例: "https://PROJECT.supabase.co/rest/v1/protein_logs"
const char* SUPABASE_REST_URL = "https://PROJECT.supabase.co/rest/v1/protein_logs";
// Project API keys -> anon（RLSで insert/select を許可する場合）
const char* SUPABASE_API_KEY  = "SUPABASE_ANON_KEY";
// devices.id 等、識別したいID（任意）
const char* SUPABASE_DEVICE_ID = "esp32-protein-switch-001";

WiFiClientSecure httpsClient;

constexpr uint8_t SWITCH_PIN = 13;     // ここを実配線の GPIO に合わせて変更
constexpr uint8_t LED_PIN    = LED_BUILTIN;

// オンボードLEDが Active-Low のボードなら、ここを LOW/HIGH で入れ替えてください
constexpr uint8_t LED_ON_LEVEL  = HIGH;
constexpr uint8_t LED_OFF_LEVEL = LOW;

constexpr uint32_t DEBOUNCE_MS = 20; // チャタリングが気になる場合は増やす
constexpr uint32_t ERROR_BLINK_DURATION_MS = 3000;
constexpr uint32_t ERROR_BLINK_INTERVAL_MS = 200;

bool stablePressed = false;
bool lastRawPressed = false;
uint32_t lastChangeMs = 0;

bool blinkActive = false;
uint32_t blinkEndMs = 0;

bool errorBlinkActive = false;
uint32_t errorBlinkEndMs = 0;
uint32_t errorBlinkNextToggleMs = 0;
bool errorBlinkLedOn = false;

static bool readPressedRaw() {
  return digitalRead(SWITCH_PIN) == LOW; // INPUT_PULLUP: 押下=LOW
}

static bool connectWiFiBlocking() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("WiFi connect timeout.");
  return false;
}

static bool sendSupabasePressLog(int &httpCode, String &response) {
  httpCode = 0;
  response = "";

  if (WiFi.status() != WL_CONNECTED) {
    httpCode = -1;
    response = "wifi not connected";
    return false;
  }

  HTTPClient http;
  httpsClient.setInsecure(); // 簡易化。本番は証明書ピンニング等推奨
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

  String body;
  body.reserve(160);
  body += '{';
  body += "\"device_id\":\""; body += SUPABASE_DEVICE_ID; body += "\",";
  body += "\"pressed_ms\":"; body += String((uint32_t)millis());
  body += '}';

  httpCode = http.POST(body);
  if (httpCode > 0) response = http.getString();
  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

static void startBlink1s() {
  errorBlinkActive = false;
  errorBlinkLedOn = false;
  blinkActive = true;
  blinkEndMs = millis() + 1000;
  digitalWrite(LED_PIN, LED_ON_LEVEL);
}

static void startErrorBlink3s() {
  blinkActive = false;
  errorBlinkActive = true;
  const uint32_t now = millis();
  errorBlinkEndMs = now + ERROR_BLINK_DURATION_MS;
  errorBlinkNextToggleMs = now;
  errorBlinkLedOn = false;
  digitalWrite(LED_PIN, LED_OFF_LEVEL);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF_LEVEL);

  lastRawPressed = readPressedRaw();
  stablePressed = lastRawPressed;
  lastChangeMs = millis();

  Serial.printf("SWITCH_PIN=%u (pressed=LOW), LED_PIN=%u\n", SWITCH_PIN, LED_PIN);
  Serial.print("MAC(STA)=");
  Serial.println(WiFi.macAddress());
}

void loop() {
  const uint32_t now = millis();
  const bool rawPressed = readPressedRaw();

  if (rawPressed != lastRawPressed) {
    lastRawPressed = rawPressed;
    lastChangeMs = now;
  }

  // 押下イベント（debounce後、false -> true）
  if ((now - lastChangeMs) >= DEBOUNCE_MS && rawPressed != stablePressed) {
    stablePressed = rawPressed;
    if (stablePressed) {
      Serial.println("pressed");
      if (connectWiFiBlocking()) {
        int httpCode = 0;
        String response;
        const bool ok = sendSupabasePressLog(httpCode, response);
        Serial.printf("Supabase log: %s (http=%d)\n", ok ? "ok" : "ng", httpCode);
        if (!ok && response.length()) {
          Serial.print("Supabase response: ");
          Serial.println(response);
        }
        if (ok) {
          startBlink1s();
        } else {
          startErrorBlink3s();
        }
      } else {
        startErrorBlink3s();
      }
    } else {
      Serial.println("released");
    }
  }

  // LED: 失敗時は 3 秒点滅 / 成功時は 1 秒点灯
  if (errorBlinkActive) {
    if ((int32_t)(now - errorBlinkEndMs) >= 0) {
      errorBlinkActive = false;
      errorBlinkLedOn = false;
      digitalWrite(LED_PIN, LED_OFF_LEVEL);
    } else if ((int32_t)(now - errorBlinkNextToggleMs) >= 0) {
      errorBlinkLedOn = !errorBlinkLedOn;
      digitalWrite(LED_PIN, errorBlinkLedOn ? LED_ON_LEVEL : LED_OFF_LEVEL);
      errorBlinkNextToggleMs = now + ERROR_BLINK_INTERVAL_MS;
    }
  } else if (blinkActive && (int32_t)(now - blinkEndMs) >= 0) {
    blinkActive = false;
    digitalWrite(LED_PIN, LED_OFF_LEVEL);
  }

  // FreeRTOS へ少し譲る（Wi-Fi 処理が詰まる環境の対策）
  delay(10);
}
