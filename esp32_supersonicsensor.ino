#include <Arduino.h>

// ==== 配線 ====
// trigPin: ESP32 -> HC-SR04 Trig
// echoPin: HC-SR04 Echo -> ESP32 (※5V系なら必ずレベル変換)
// ledPin : ESP32 -> LED（330Ω抵抗を挟むのが推奨）
const uint8_t trigPin = 4;
const uint8_t echoPin = 16;
const uint8_t ledPin  = 23;   // 例: ESP32 の GPIO2（オンボードLEDでもOK）

// ==== 設定 ====
static const unsigned long PULSE_TIMEOUT_US = 30000UL; // 30ms ≒ 最大約5m
static const float        DEFAULT_TEMP_C    = 20.0f;   // 温度補正の基準
static const unsigned int MEASURE_INTERVAL_MS = 500;   // 出力間隔(ms)

// 音速(cm/μs)を温度から計算
static inline float soundSpeed_cm_per_us(float tempC) {
  const float c_ms = 331.3f + 0.606f * tempC; // m/s
  return (c_ms * 100.0f) / 1e6f;              // cm/μs
}

// 1回計測（cm）。失敗時は NaN
float measureOnceCm(float tempC = DEFAULT_TEMP_C) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, PULSE_TIMEOUT_US);
  if (duration == 0) return NAN;

  const float v = soundSpeed_cm_per_us(tempC);
  return (duration * v * 0.5f);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(ledPin, OUTPUT);

  digitalWrite(ledPin, LOW); // 初期状態: OFF
}

void loop() {
  float d_cm = measureOnceCm(DEFAULT_TEMP_C);

  if (isnan(d_cm)) {
    Serial.println("Distance: NaN (timeout or invalid reading)");
    digitalWrite(ledPin, LOW); // 計測失敗時はLED消灯
  } else {
    Serial.print("Distance: ");
    Serial.print(d_cm, 2);
    Serial.println(" cm");

    // === 判定処理 ===
    if (d_cm <= 10.0f) {
      digitalWrite(ledPin, HIGH);  // 10cm以下 → LED点灯
    } else {
      digitalWrite(ledPin, LOW);   // それ以外 → LED消灯
    }
  }

  delay(MEASURE_INTERVAL_MS);
}
