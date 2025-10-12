// ESP8266 + 4CH Relay + 4 Buttons + Fan (mini blower)
// Flowchart: Scan -> Fan ON -> Choose energy button -> Relays ON -> Measure energy/CO2 -> Finish
// Serial Monitor: 115200. Ketik SCAN untuk memulai (simulasi scan badge).

#include <Arduino.h>

// ===================== Konfigurasi Hardware =====================
// Set ke LOW jika modul relay aktif LOW (umum), HIGH jika aktif HIGH
const uint8_t RELAY_ACTIVE_LEVEL = LOW;
const uint8_t RELAY_INACTIVE_LEVEL = (LOW == LOW) ? HIGH : LOW; // disesuaikan runtime

// Relay pins
const uint8_t RELAY_FAN = D1; // CH1 - mini blower
const uint8_t RELAY_A   = D2; // CH2
const uint8_t RELAY_B   = D6; // CH3
const uint8_t RELAY_C   = D7; // CH4

// Button pins (gunakan tombol ke GND, INPUT_PULLUP)
const uint8_t BTN_A   = D5; // pilih Energi A  -> CH2
const uint8_t BTN_B   = D4; // pilih Energi B  -> CH3
const uint8_t BTN_C   = D0; // pilih Energi C  -> CH4
const uint8_t BTN_STOP= D3; // Finish/STOP semua relay

// ===================== Parameter Energi (simulasi power sensor) =====================
// Perkiraan daya beban (Watt). Ubah sesuai perangkatmu.
const float P_FAN_W = 2.5f;   // kipas mini 5V ~0.5A -> 2.5 W (contoh)
const float P_A_W   = 5.0f;   // beban A
const float P_B_W   = 10.0f;  // beban B
const float P_C_W   = 15.0f;  // beban C

const float CO2_FACTOR = 0.82f; // kg CO2 / kWh

// Jika ingin mode TIMER (durasi tetap saat tombol ditekan), isi durasi berikut (ms) dan aktifkan blok TIMER di bawah.
// const unsigned long D_A_MS = 10000; // 10 s
// const unsigned long D_B_MS = 20000; // 20 s
// const unsigned long D_C_MS = 30000; // 30 s

// ===================== Variabel Status =====================
bool authorized = false;

struct Channel {
  uint8_t pin;
  bool on;
  float powerW;
  unsigned long lastOnMs;
  double energyWh; // akumulasi
  // unsigned long offAtMs; // untuk mode TIMER
};

Channel chFan = { RELAY_FAN, false, P_FAN_W, 0UL, 0.0 };
Channel chA   = { RELAY_A,   false, P_A_W,   0UL, 0.0 };
Channel chB   = { RELAY_B,   false, P_B_W,   0UL, 0.0 };
Channel chC   = { RELAY_C,   false, P_C_W,   0UL, 0.0 };

const uint8_t buttons[4] = { BTN_A, BTN_B, BTN_C, BTN_STOP };
bool lastBtn[4] = {1,1,1,1};
unsigned long lastDebounceMs[4] = {0,0,0,0};
const unsigned long DEBOUNCE_MS = 35;

void relayWrite(Channel &ch, bool turnOn) {
  if (ch.on == turnOn) return;
  ch.on = turnOn;

  digitalWrite(ch.pin, turnOn ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);

  if (turnOn) {
    ch.lastOnMs = millis();
  } else {
    unsigned long onMs = millis() - ch.lastOnMs;
    ch.energyWh += (ch.powerW * (onMs / 3600000.0)); // W * h = Wh
  }
}

void allOff() {
  relayWrite(chFan, false);
  relayWrite(chA, false);
  relayWrite(chB, false);
  relayWrite(chC, false);
}

void printStatus() {
  double eWhFan = chFan.energyWh + (chFan.on ? (chFan.powerW * ((millis()-chFan.lastOnMs)/3600000.0)) : 0.0);
  double eWhA   = chA.energyWh   + (chA.on   ? (chA.powerW   * ((millis()-chA.lastOnMs)/3600000.0))   : 0.0);
  double eWhB   = chB.energyWh   + (chB.on   ? (chB.powerW   * ((millis()-chB.lastOnMs)/3600000.0))   : 0.0);
  double eWhC   = chC.energyWh   + (chC.on   ? (chC.powerW   * ((millis()-chC.lastOnMs)/3600000.0))   : 0.0);

  double eKWhTotal = (eWhFan + eWhA + eWhB + eWhC) / 1000.0;
  double co2kg = eKWhTotal * CO2_FACTOR;

  Serial.print("FAN="); Serial.print(chFan.on ? "ON" : "OFF");
  Serial.print("  A="); Serial.print(chA.on ? "ON" : "OFF");
  Serial.print("  B="); Serial.print(chB.on ? "ON" : "OFF");
  Serial.print("  C="); Serial.print(chC.on ? "ON" : "OFF");
  Serial.print("  E_total="); Serial.print(eKWhTotal, 6); Serial.print(" kWh");
  Serial.print("  CO2="); Serial.print(co2kg, 6); Serial.println(" kg");
}

bool readButton(uint8_t idx) {
  bool raw = digitalRead(buttons[idx]); // PULLUP -> LOW saat ditekan
  if (raw != lastBtn[idx]) {
    lastDebounceMs[idx] = millis();
    lastBtn[idx] = raw;
  }
  if (millis() - lastDebounceMs[idx] > DEBOUNCE_MS) {
    return (raw == LOW); // ditekan
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Relay output
  pinMode(chFan.pin, OUTPUT);
  pinMode(chA.pin, OUTPUT);
  pinMode(chB.pin, OUTPUT);
  pinMode(chC.pin, OUTPUT);
  digitalWrite(chFan.pin, RELAY_INACTIVE_LEVEL);
  digitalWrite(chA.pin, RELAY_INACTIVE_LEVEL);
  digitalWrite(chB.pin, RELAY_INACTIVE_LEVEL);
  digitalWrite(chC.pin, RELAY_INACTIVE_LEVEL);

  // Buttons
  for (uint8_t i=0;i<4;i++) {
    pinMode(buttons[i], INPUT_PULLUP);
  }

  Serial.println(F("Ketik 'SCAN' di Serial untuk mensimulasikan scan badge."));
}

unsigned long lastPrint = 0;

void loop() {
  // -------- Simulasi Scan Badge ----------
  if (!authorized && Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim(); s.toUpperCase();
    if (s == "SCAN") {
      authorized = true;
      Serial.println(F("[Auth] Badge OK. Sistem aktif."));
      // Flowchart: microcontroller send a signal to mini blower
      relayWrite(chFan, true); // mulai meniup balon
      Serial.println(F("[Fan] Mini blower ON (mulai meniup balon)"));
    }
  }
  if (!authorized) {
    delay(10);
    return;
  }

  // -------- Baca tombol (Choose energy button) ----------
  if (readButton(0)) { // BTN_A
    relayWrite(chA, !chA.on);
    Serial.println(chA.on ? F("[Relay A] ON") : F("[Relay A] OFF"));
    delay(200);
  }
  if (readButton(1)) { // BTN_B
    relayWrite(chB, !chB.on);
    Serial.println(chB.on ? F("[Relay B] ON") : F("[Relay B] OFF"));
    delay(200);
  }
  if (readButton(2)) { // BTN_C
    relayWrite(chC, !chC.on);
    Serial.println(chC.on ? F("[Relay C] ON") : F("[Relay C] OFF"));
    delay(200);
  }
  if (readButton(3)) { // BTN_STOP -> Finish
    allOff();
    Serial.println(F("[Finish] Semua relay OFF"));
    delay(300);
  }

  // -------- Jika ingin MODE TIMER, aktifkan blok ini --------
  // if (chA.on && millis() - chA.lastOnMs >= D_A_MS) relayWrite(chA, false);
  // if (chB.on && millis() - chB.lastOnMs >= D_B_MS) relayWrite(chB, false);
  // if (chC.on && millis() - chC.lastOnMs >= D_C_MS) relayWrite(chC, false);

  // -------- Print status berkala (simulasi touchscreen) --------
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    printStatus();
  }

  delay(5);
}
