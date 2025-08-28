#include <Arduino.h>
#include "DHT.h"

// ---- Pins ----
const uint8_t TRIG_PIN = 5;
const uint8_t ECHO_PIN = 16;
const uint8_t LED_PIN  = 23;
const uint8_t DHT_PIN  = 4;     // DHT11 DATA
#define DHTTYPE DHT11

// ---- Config ----
static const unsigned long PULSE_TIMEOUT_US   = 30000UL;
static const unsigned long DHT_MIN_INTERVALMS = 2000UL;  // DHT11は>=1s推奨
static const unsigned long LOOP_INTERVAL_MS   = 500UL;   // 距離更新周期
static const float DEFAULT_TEMP_C = 20.0f;

DHT dht(DHT_PIN, DHTTYPE);
float lastTempC = NAN, lastHum = NAN;
unsigned long lastDhtReadMs = 0;

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
    digitalWrite(LED_PIN, (d_cm <= 10.0f) ? HIGH : LOW);
  }

  delay(LOOP_INTERVAL_MS);
}
