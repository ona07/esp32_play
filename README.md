# esp32_play

## 目的/概要
ESP32 + INA226 で電圧・電流・電力を測定し、Supabase に保存、フロント（`power_logs.html`）で表示する。

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
bool sendSupabasePower(const DateTime &ts, float voltageV, float currentmA, float powerMw,
                       int &httpCode, String &response)
```

## ハード構成
- ESP32
- INA226（電流/電圧センサ）
- RTC（DS3231）
- 電池（単四など）
- 負荷（測定したい回路）

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
RTC_DS3231 rtc;
INA226_WE ina226(INA226_ADDR);
```

## 配線図（I2C + 電流測定ライン）
### I2C
- ESP32 SDA → GPIO21
- ESP32 SCL → GPIO22
- ESP32 3.3V → INA226 VCC / DS3231 VCC
- ESP32 GND → INA226 GND / DS3231 GND

### 電流測定ライン（直列）
- 電池＋ → INA226 VIN+
- INA226 VIN- → 負荷＋
- 負荷− → 電池−（ESP32/INA226 GND と共通）

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
Wire.begin(); // ESP32 default SDA=21, SCL=22
constexpr uint8_t INA226_ADDR = 0x40;
```

## ファーム設定
以下は `esp32_ina_rtc_supabase.ino` の差し替え対象。

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* SUPABASE_REST_URL = "https://PROJECT.supabase.co/rest/v1/power_logs";
const char* SUPABASE_API_KEY  = "SUPABASE_ANON_KEY";
const char* SUPABASE_DEVICE_ID = "esp32-ina-001";
```

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
const char* SUPABASE_REST_URL = "https://.../rest/v1/power_logs";
const char* SUPABASE_API_KEY  = "SUPABASE_ANON_KEY";
const char* SUPABASE_DEVICE_ID = "esp32-ina-001";
```

## 送信データ構造と送信頻度
### データ構造
```json
{
  "device_id": "esp32-ina-001",
  "voltage_v": 1.234,
  "current_ma": 12.34,
  "power_mw": 15.22,
  "recorded_at": "2025-01-01T12:34:56+09:00"
}
```

### 送信頻度
1分間隔（`UPLOAD_INTERVAL_MS = 60000`）。

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
constexpr unsigned long UPLOAD_INTERVAL_MS   = 60000UL;
body += "\"device_id\":\""; body += SUPABASE_DEVICE_ID; body += "\",";
body += "\"voltage_v\":"; body += String(voltageV, 3); body += ',';
body += "\"current_ma\":"; body += String(currentmA, 2); body += ',';
body += "\"power_mw\":"; body += String(powerMw, 2); body += ',';
body += "\"recorded_at\":\""; body += formatIso8601(ts, TZ_OFFSET_SECONDS); body += "\"";
```

## Supabase側設定
### テーブル
- `power_logs`
- 列: `device_id`, `voltage_v`, `current_ma`, `power_mw`, `recorded_at`

### RLSポリシー例
```sql
-- insert を許可（ESP32から書き込み）
create policy "allow insert power_logs"
on public.power_logs
for insert
to anon
with check (true);

-- select を許可（フロントから読み取り）
create policy "allow read power_logs"
on public.power_logs
for select
to anon
using (true);
```

### 採用コード
```cpp
// esp32_ina_rtc_supabase.ino
http.addHeader("apikey", SUPABASE_API_KEY);
http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
```

## フロント（power_logs.html）
### CONFIG の編集箇所
`power_logs.html` 内の `CONFIG` を変更する。

```js
const CONFIG = Object.freeze({
  restUrl: 'https://PROJECT.supabase.co/rest/v1/power_logs',
  apiKey: 'SUPABASE_ANON_KEY',
  deviceId: 'esp32-ina-001',
  limit: 120
});
```

### グラフ操作
- 左右キーでポイント移動
- 横軸は「開始 / 選択 / 終了」を簡易表示

### 採用コード
```js
// power_logs.html
window.addEventListener('keydown', (event) => {
  if (event.key === 'ArrowLeft') setSelection(selectedIndex - 1);
  if (event.key === 'ArrowRight') setSelection(selectedIndex + 1);
});
```

## GitHub Pages 公開手順（power_logs.html 直指定）
1. GitHub → Settings → Pages
2. Source: `Deploy from branch`
3. Branch: `main` / Folder: `/ (root)`
4. 公開URL: `https://<user>.github.io/<repo>/power_logs.html`
