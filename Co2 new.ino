/********************************************************************************************
 *  (PENJELASAN UNTUK ORANG AWAM)
 *  APA YANG DILAKUKAN PERANGKAT INI?
 *  - Ini adalah “termometer pintar + detektor gerak + kipas otomatis” yang bisa dipantau dari HP/PC.
 *  - Alat membaca SUHU & KELEMBAPAN (pakai sensor SHT31; kalau gagal, jatuh ke DHT11).
 *  - Alat juga mendeteksi GERAK (sensor PIR). Jika ada gerak dan suhu panas, kipas otomatis MENYALA.
 *  - Alat menghitung perkiraan ENERGI listrik yang dipakai kipas dan EMISI CO2-nya.
 *  - Semua status bisa dilihat dari halaman WEB lokal (dashboard) yang disajikan oleh ESP8266.
 *
 *  BAGAIMANA CARA KERJANYA (VERSI SEDERHANA):
 *  1) ESP8266 nyambung ke Wi-Fi rumah/kantor Anda (isi SSID & password di bawah).
 *  2) ESP8266 membaca sensor tiap detik.
 *  3) Jika suhu >= 28°C DAN ada gerakan, kipas ON. Kalau suhu turun <= 26°C atau tidak ada gerakan, kipas OFF.
 *  4) Kapan pun, Anda bisa buka dashboard dari HP/PC: tampil suhu, kelembapan, status gerakan, kipas, energi, CO2.
 *  5) Tombol “Simulate SCAN” di dashboard berfungsi sebagai “kunci”. Sistem hanya berjalan jika Anda menekan tombol itu
 *     (artinya “diizinkan”). Tombol “Lock” untuk mengunci lagi (kipas juga langsung dimatikan demi aman).
 *
 *  CARA MEMAKAI (LANGKAH PRAKTIS):
 *  a) Rangkai kabel:
 *     - PIR OUT -> D5 (GPIO14). VCC PIR ke 5V (atau 3V3 sesuai modul), GND ke GND.
 *     - Kipas dikuatkan transistor 2N222: BASIS ke D6 (GPIO12) lewat resistor 1k–4k7,
 *       KOLEKTOR ke - kipas (GND kipas), EMITER ke GND. + kipas ke supply kipas (misal 5V).
 *       Pasang DIODE FLYBACK (1N4007) paralel ke kipas (anoda ke GND, katoda ke +V kipas).
 *       GABUNGKAN GND kipas dengan GND ESP8266 (common ground).
 *     - DHT11 DATA -> D4 (GPIO2), VCC 3V3, GND ke GND.
 *     - SHT31 via I2C: SDA=D2, SCL=D1, VCC 3V3, GND ke GND (alamat 0x44/0x45).
 *  b) Di kode di bawah, isi WIFI_SSID & WIFI_PASS.
 *  c) Upload. Buka Serial Monitor (115200) dan catat alamat IP yang dicetak (misal 192.168.1.23).
 *  d) Buka di browser: http://ALAMAT_IP/  → muncul dashboard.
 *  e) Klik “Simulate SCAN” untuk mengaktifkan sistem. “Lock” untuk mengunci lagi.
 *
 *  CATATAN TEKNIS:
 *  - Histeresis suhu (ON di 28°C, OFF di 26°C) mencegah kipas “kedap-kedip”.
 *  - Perhitungan energi = Daya kipas (W) x Lama ON (jam). Ubah `FAN_POWER_W` sesuai kipas Anda.
 *  - Estimasi CO2 = kWh × 0.82 kg/kWh (angka acuan; bisa diubah).
 *  - Endpoint web:
 *      GET  /        -> Dashboard HTML
 *      GET  /status  -> JSON status (untuk integrasi/otomasi lebih lanjut)
 *      POST /auth/scan -> Izinkan sistem berjalan
 *      POST /auth/lock -> Kunci sistem (kipas dimatikan)
 *  - Untuk produksi, tambahkan autentikasi (password/token) pada endpoint /auth/*.
 ********************************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <DHT.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// ====== Wi-Fi (ISI SENDIRI) ======
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// ====== PIN ======
#define PIN_PIR    D5       // GPIO14 : input PIR (HIGH = ada gerak)
#define PIN_FAN    D6       // GPIO12 : output ke basis 2N222 (via resistor 1k–4k7)
#define PIN_DHT    D4       // GPIO2  : data DHT11

// ====== SENSOR ======
Adafruit_SHT31 sht31 = Adafruit_SHT31();
#define DHTTYPE    DHT11
DHT dht(PIN_DHT, DHTTYPE);

// ====== PARAMETER KONTROL ======
const float TEMP_ON_C   = 28.0;      // suhu menyalakan kipas
const float TEMP_OFF_C  = 26.0;      // suhu mematikan kipas (histeresis)
const unsigned long READ_INTERVAL_MS = 1000; // jeda pembacaan sensor (ms)
const float FAN_POWER_W = 2.5;       // daya kipas (W) → ubah sesuai spesifikasi
const float CO2_FACTOR  = 0.82;      // kg CO2 / kWh (asumsi grid)

bool authorized = false;             // “kunci” sistem
bool fanOn      = false;
unsigned long lastReadMs   = 0;
unsigned long fanOnStartMs = 0;
double energy_Wh_accum     = 0.0;

ESP8266WebServer server(80);

// ====== Kontrol kipas + akumulasi energi ======
void fanWrite(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN, on ? HIGH : LOW);
  if (on) {
    fanOnStartMs = millis();
  } else {
    unsigned long onMs = millis() - fanOnStartMs;
    energy_Wh_accum += (FAN_POWER_W * (onMs / 3600000.0)); // Wh = W × jam
  }
}

// ====== Halaman dashboard (HTML + CSS + JS) ======
String htmlPage() {
  String page = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>ESP8266 Environment & Fan Monitor</title>
<style>
  :root { font-family: system-ui, Arial, sans-serif; color-scheme: light dark; }
  body { margin: 0; padding: 16px; }
  .wrap { max-width: 760px; margin: 0 auto; }
  .card { border: 1px solid #9993; border-radius: 14px; padding: 16px; margin-bottom: 14px; box-shadow: 0 1px 8px #0001; }
  .row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .big { font-size: 2rem; font-weight: 700; }
  .ok { color: #0a7; }
  .warn { color: #d80; }
  .bad { color: #d33; }
  button { padding: 10px 14px; border-radius: 10px; border: 1px solid #9994; cursor: pointer; }
  code { padding: 2px 6px; border-radius: 6px; background: #00000010; }
  .grid { display:grid; grid-template-columns: 1fr 1fr; gap:10px; }
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
</style></head><body>
<div class="wrap">
  <h2>ESP8266 Environment & Fan Monitor</h2>

  <div class="card">
    <div class="grid">
      <div>
        <div>Authorization</div>
        <div id="auth" class="big">…</div>
        <div style="margin-top:10px">
          <button id="scanBtn">Simulate SCAN</button>
          <button id="lockBtn">Lock</button>
        </div>
      </div>
      <div>
        <div>Network</div>
        <div>IP: <code id="ip">...</code></div>
        <div>Uptime: <span id="uptime">...</span></div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <div><div>Temperature</div><div id="temp" class="big">– °C</div></div>
      <div><div>Humidity</div><div id="hum" class="big">– %</div></div>
    </div>
    <div class="row" style="margin-top:10px">
      <div><div>PIR (motion)</div><div id="pir" class="big">–</div></div>
      <div><div>Fan</div><div id="fan" class="big">–</div></div>
    </div>
  </div>

  <div class="card">
    <div>Energy</div>
    <div class="row">
      <div><div>Total</div><div id="kwh" class="big mono">0.000000 kWh</div></div>
      <div><div>CO₂ Est.</div><div id="co2" class="big mono">0.000000 kg</div></div>
    </div>
  </div>

  <div class="card">
    <div>Raw JSON (<code>/status</code>)</div>
    <pre id="raw" class="mono" style="white-space:pre-wrap; font-size:12px; background:#00000008; padding:10px; border-radius:10px; max-height:260px; overflow:auto;">loading…</pre>
  </div>
</div>

<script>
const $ = s => document.querySelector(s);
async function pull() {
  try {
    const res = await fetch('/status', {cache:'no-store'});
    const j = await res.json();
    $('#auth').textContent = j.authorized ? 'AUTHORIZED' : 'LOCKED';
    $('#auth').className = 'big ' + (j.authorized ? 'ok' : 'bad');
    $('#temp').textContent = isNaN(j.temp_c) ? 'NaN °C' : (j.temp_c.toFixed(1)+' °C');
    $('#hum').textContent  = isNaN(j.hum_pct) ? 'NaN %'  : (j.hum_pct.toFixed(0)+' %');
    $('#pir').textContent  = j.motion ? 'MOTION' : 'NO';
    $('#pir').className    = 'big ' + (j.motion ? 'warn' : '');
    $('#fan').textContent  = j.fan ? 'ON' : 'OFF';
    $('#fan').className    = 'big ' + (j.fan ? 'ok' : '');
    $('#kwh').textContent  = (j.energy_kWh||0).toFixed(6)+' kWh';
    $('#co2').textContent  = (j.co2_kg||0).toFixed(6)+' kg';
    $('#raw').textContent  = JSON.stringify(j,null,2);
    $('#uptime').textContent = j.uptime_s+' s';
    const ip = res.headers.get('X-ESP-IP'); if (ip) $('#ip').textContent = ip;
  } catch(e) { console.error(e); }
}
setInterval(pull, 1000); pull();
$('#scanBtn').addEventListener('click', async ()=>{ await fetch('/auth/scan',{method:'POST'}); pull(); });
$('#lockBtn').addEventListener('click', async ()=>{ await fetch('/auth/lock',{method:'POST'}); pull(); });
</script>
</body></html>
)HTML";
  return page;
}

// ====== HTTP handlers ======
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", htmlPage());
}

void handleStatus() {
  double energy_Wh_live = energy_Wh_accum;
  if (fanOn) {
    unsigned long onMs = millis() - fanOnStartMs;
    energy_Wh_live += (FAN_POWER_W * (onMs / 3600000.0));
  }
  const double energy_kWh = energy_Wh_live / 1000.0;
  const double co2_kg = energy_kWh * CO2_FACTOR;

  const bool motion = digitalRead(PIN_PIR) == HIGH;

  float tC = NAN, h = NAN;
  float tSHT = sht31.readTemperature();
  float hSHT = sht31.readHumidity();
  if (!isnan(tSHT) && !isnan(hSHT)) { tC = tSHT; h = hSHT; }
  else {
    float tDHT = dht.readTemperature();
    float hDHT = dht.readHumidity();
    if (!isnan(tDHT) && !isnan(hDHT)) { tC = tDHT; h = hDHT; }
  }

  server.sendHeader("X-ESP-IP", WiFi.localIP().toString());
  server.sendHeader("Cache-Control", "no-store");

  String json = "{";
  json += "\"authorized\":" + String(authorized ? "true":"false") + ",";
  json += "\"fan\":"        + String(fanOn ? "true":"false") + ",";
  json += "\"motion\":"     + String(motion ? "true":"false") + ",";
  json += "\"energy_kWh\":" + String(energy_kWh, 6) + ",";
  json += "\"co2_kg\":"     + String(co2_kg, 6) + ",";
  json += "\"uptime_s\":"   + String(millis()/1000);
  json += ",\"temp_c\":"    + (isnan(tC)?String("null"):String(tC,1));
  json += ",\"hum_pct\":"   + (isnan(h) ?String("null"):String(h,0));
  json += "}";
  server.send(200, "application/json", json);
}

void handleScan() { authorized = true;  server.send(200, "text/plain", "OK"); }
void handleLock() { authorized = false; if (fanOn) fanWrite(false); server.send(200, "text/plain", "OK"); }

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_FAN, OUTPUT);
  fanWrite(false);

  Wire.begin(); // I2C (D2=D1 default: SDA=SCL)

  if (!sht31.begin(0x44)) {
    if (!sht31.begin(0x45)) {
      Serial.println(F("[SHT31] tidak terdeteksi (0x44/0x45). Cek wiring 3V3/SDA/SCL."));
    }
  } else {
    Serial.println(F("[SHT31] OK"));
  }
  dht.begin();

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(); Serial.print("Wi-Fi connected. IP: "); Serial.println(WiFi.localIP());

  if (MDNS.begin("esp8266-fan")) Serial.println("mDNS: http://esp8266-fan.local/");

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/auth/scan", HTTP_POST, handleScan);
  server.on("/auth/lock", HTTP_POST, handleLock);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("HTTP server started");
}

// ====== Loop ======
void loop() {
  server.handleClient();
  MDNS.update();

  // Opsi “scan” via Serial Monitor (ketik: SCAN)
  if (!authorized && Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim(); s.toUpperCase();
    if (s == "SCAN") { authorized = true; Serial.println(F("[Auth] Badge OK. Sistem aktif.")); }
  }

  if (!authorized) { delay(10); return; }

  if (millis() - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = millis();

    if (sht31.isHeaterEnabled()) sht31.heater(false);

    float tC = NAN, h = NAN;
    float tSHT = sht31.readTemperature();
    float hSHT = sht31.readHumidity();
    if (!isnan(tSHT) && !isnan(hSHT)) { tC = tSHT; h = hSHT; }
    else {
      float tDHT = dht.readTemperature();
      float hDHT = dht.readHumidity();
      if (!isnan(tDHT) && !isnan(hDHT)) { tC = tDHT; h = hDHT; }
    }

    const bool motion = digitalRead(PIN_PIR) == HIGH;

    // Logika kipas (histeresis + syarat gerak)
    if (!isnan(tC)) {
      if (fanOn) {
        if (tC <= TEMP_OFF_C || !motion) fanWrite(false);
      } else {
        if (tC >= TEMP_ON_C && motion)   fanWrite(true);
      }
    }

    // (Debug ringkas ke Serial)
    double energy_Wh_live = energy_Wh_accum;
    if (fanOn) {
      unsigned long onMs = millis() - fanOnStartMs;
      energy_Wh_live += (FAN_POWER_W * (onMs / 3600000.0));
    }
    const double energy_kWh = energy_Wh_live / 1000.0;
    const double co2_kg = energy_kWh * CO2_FACTOR;

    Serial.print("[T]="); if (isnan(tC)) Serial.print("NaN"); else Serial.print(tC, 1);
    Serial.print("C  [H]="); if (isnan(h)) Serial.print("NaN"); else Serial.print(h, 0);
    Serial.print("%  PIR="); Serial.print(motion ? "MOTION" : "NO");
    Serial.print("  FAN=");  Serial.print(fanOn ? "ON" : "OFF");
    Serial.print("  E=");    Serial.print(energy_kWh, 6); Serial.print(" kWh");
    Serial.print("  CO2=");  Serial.print(co2_kg, 6); Serial.println(" kg");
  }

  delay(5);
}
