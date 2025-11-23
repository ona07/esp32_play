#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <Adafruit_SHT31.h>
#include <string.h>

// ---- I2C Addresses ----
constexpr uint8_t SHT31_ADDR_PRIMARY = 0x44;
constexpr uint8_t SHT31_ADDR_SECONDARY = 0x45;
constexpr uint8_t LCD_ADDR_CANDIDATES[] = {0x27, 0x3F, 0x20, 0x3E};

// ---- Update cadence ----
constexpr unsigned long SENSOR_POLL_INTERVAL_MS = 30000UL; // 30 seconds
constexpr uint8_t SHT31_FAIL_REINIT_THRESHOLD = 3;
constexpr unsigned long SHT31_RETRY_INTERVAL_MS = 2000UL;

RTC_DS3231 rtc;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
LiquidCrystal_I2C *lcd = nullptr;

float lastTempC = NAN;
float lastHum = NAN;
unsigned long lastPollMs = 0;
bool rtcReady = false;
bool shtReady = false;
uint8_t shtAddressInUse = 0;
uint8_t shtConsecutiveFails = 0;
unsigned long lastShtRetryMs = 0;

uint8_t detectLcdAddress() {
  for (uint8_t addr : LCD_ADDR_CANDIDATES) {
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

void initLcd() {
  uint8_t address = detectLcdAddress();
  if (address == 0) {
    Serial.println(F("LCD1602 not found; continuing without display."));
    return;
  }
  lcd = new LiquidCrystal_I2C(address, 16, 2);
  lcd->init();
  lcd->backlight();
  lcd->setCursor(0, 0);
  lcd->print(F("RTC Temp/Humi"));
  lcd->setCursor(0, 1);
  lcd->print(F("Initializing..."));
}

bool initRtc() {
  if (!rtc.begin()) {
    Serial.println(F("RTC not detected!"));
    return false;
  }
  if (rtc.lostPower()) {
    Serial.println(F("RTC lost power; set time via rtc.adjust()."));
  }
  return true;
}

bool initSht31() {
  if (sht31.begin(SHT31_ADDR_PRIMARY)) {
    shtAddressInUse = SHT31_ADDR_PRIMARY;
    Serial.print(F("SHT31 detected at 0x"));
    Serial.println(shtAddressInUse, HEX);
    return true;
  }
  if (sht31.begin(SHT31_ADDR_SECONDARY)) {
    shtAddressInUse = SHT31_ADDR_SECONDARY;
    Serial.print(F("Using secondary SHT31 address (0x"));
    Serial.print(shtAddressInUse, HEX);
    Serial.println(F(")."));
    return true;
  }
  shtAddressInUse = 0;
  Serial.println(F("SHT31 sensor not detected."));
  return false;
}

void maybeReinitSht31(unsigned long nowMs) {
  if (nowMs - lastShtRetryMs < SHT31_RETRY_INTERVAL_MS) {
    return;
  }
  Serial.println(F("Retrying SHT31 detection..."));
  shtReady = initSht31();
  lastShtRetryMs = nowMs;
  if (!shtReady) {
    Serial.println(F("Still no SHT31; check VCC/GND/SDA/SCL/ADDR wiring."));
  } else {
    shtConsecutiveFails = 0;
  }
}

void formatValue(char *out, size_t len, float value, uint8_t decimals, const char *fallback = "--.-") {
  if (isnan(value)) {
    strncpy(out, fallback, len);
    out[len - 1] = '\0';
    return;
  }
  dtostrf(value, 0, decimals, out);
}

void updateLcd(DateTime now, float tempC, float hum) {
  if (!lcd) return;
  char line0[17];
  snprintf(line0, sizeof(line0), "%02d:%02d:%02d %02d/%02d",
           now.hour(), now.minute(), now.second(), now.month(), now.day());

  char tempBuf[8];
  char humBuf[8];
  formatValue(tempBuf, sizeof(tempBuf), tempC, 1);
  formatValue(humBuf, sizeof(humBuf), hum, 1);

  lcd->setCursor(0, 0);
  lcd->print(line0);
  lcd->setCursor(0, 1);
  lcd->print("T:");
  lcd->print(tempBuf);
  lcd->print((char)223); // degree symbol
  lcd->print("C H:");
  lcd->print(humBuf);
  lcd->print('%');
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(400000);

  initLcd();
  rtcReady = initRtc();
  shtReady = initSht31();
  lastShtRetryMs = millis();

  if (!rtcReady) {
    Serial.println(F("RTC unavailable; defaulting to compile time."));
  }
  if (!shtReady) {
    Serial.println(F("SHT31 unavailable; temperature/humidity will show --.- until detected."));
  }
}

void loop() {
  unsigned long nowMs = millis();
  if (nowMs - lastPollMs >= SENSOR_POLL_INTERVAL_MS) {
    lastPollMs = nowMs;

    if (!shtReady) {
      maybeReinitSht31(nowMs);
    }

    if (shtReady) {
      float temp = sht31.readTemperature();
      float hum = sht31.readHumidity();
      if (isnan(temp) || isnan(hum)) {
        ++shtConsecutiveFails;
        Serial.println(F("SHT31 read failed."));
        if (shtConsecutiveFails >= SHT31_FAIL_REINIT_THRESHOLD) {
          Serial.println(F("Reinitializing SHT31 after repeated read errors..."));
          shtReady = false;
          maybeReinitSht31(nowMs);
        }
      } else {
        lastTempC = temp;
        lastHum = hum;
        shtConsecutiveFails = 0;
        Serial.print(F("Temp: "));
        Serial.print(temp, 1);
        Serial.print(F(" C  Hum: "));
        Serial.print(hum, 1);
        Serial.println(F(" %"));
      }
    }
  }

  DateTime now = rtcReady ? rtc.now() : DateTime(F(__DATE__), F(__TIME__));
  updateLcd(now, lastTempC, lastHum);
  delay(50);
}
