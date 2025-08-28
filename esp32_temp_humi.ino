#include "DHT.h"

#define DHTPIN 12        // DHT11 の DATA ピンを接続した GPIO
#define DHTTYPE DHT11   // センサーの種類を指定 (DHT11)

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println("DHT11 test on ESP32");
}

void loop() {
  // 湿度
  float h = dht.readHumidity();
  // 温度 (摂氏)
  float t = dht.readTemperature();
  // 温度 (華氏)
  float f = dht.readTemperature(true);

  // 失敗時のチェック
  if (isnan(h) || isnan(t) || isnan(f)) {
    Serial.println("❌ Failed to read from DHT11 sensor!");
    delay(2000);
    return;
  }

  Serial.print("Humidity: ");
  Serial.print(h);
  Serial.print(" %\t");

  Serial.print("Temperature: ");
  Serial.print(t);
  Serial.print(" *C  ");
  Serial.print(f);
  Serial.println(" *F");

  delay(2000); // DHT11 は1秒以上間隔をあける必要あり（安全に2秒）
}
