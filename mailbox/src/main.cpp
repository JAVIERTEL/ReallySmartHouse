/*
 * Mailbox Node — Really Smart House (IoT 34346)
 * ESP32 (WROOM-32) + RN2483 LoRa P2P
 *
 * Logic:
 *   1. ESP32 wakes up via ext0 (button press)
 *   2. Init RN2483 via UART
 *   3. Read battery level (ADC GPIO34) as percentage (0-100)
 *   4. Send 4-byte LoRa packet to gateway
 *   5. Wait for ACK — if no ACK, retry up to 3 times with 90s delay
 *   6. Go back to deep sleep until next button press
 *
 * Power supply: Solar panel -> TP4056 charger -> LiPo 3.7V -> ESP32
 *
 * Payload sent (hex):
 *   Byte 0:    0x03              — node ID (mailbox)
 *   Bytes 1-4: "DATA" (ASCII)   — 0x44415441
 *   Byte 5:    0x00              — reserved (TBD with gateway)
 *   Bytes 6+:  "mails=1;battery=XX;" (ASCII) — XX = battery percentage (0-100)
 *
 * ACK received (4 bytes, hex):
 *   Byte 0: 0x00        — gateway ID
 *   Byte 1: 0xAC        — ACK type
 *   Byte 2: 0x03        — addressed to mailbox node
 *   Byte 3: 0x01        — ok
 *
 * WIRING:
 *   RN2483 TX  -> ESP32 GPIO19
 *   RN2483 RX  -> ESP32 GPIO18
 *   RN2483 RST -> ESP32 GPIO23
 *   RN2483 3V3 -> 3V3
 *   RN2483 GND -> GND
 *
 *   Button     -> GPIO33 (other leg to 3V3, internal pullup enabled)
 *   Bat sense  -> GPIO34 (resistor divider R1=100k, R2=100k from LiPo+)
 *
 * BATTERY RESISTOR DIVIDER:
 *   LiPo+ -> R1(100k) -> GPIO34 -> R2(100k) -> GND
 *   LiPo range: 3.0V (empty) to 4.2V (full)
 *   After divider: 1.5V - 2.1V (within ESP32 ADC range 0-3.3V)
 *
 * Author: Ignacio Diez (s253632)
 * Date: April 2026
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include "driver/rtc_io.h"

// ── Pin definitions ───────────────────────────────────────────────────────────
#define RXD2            19      // ESP32 RX <- RN2483 TX
#define TXD2            18      // ESP32 TX -> RN2483 RX
#define RST_LORA        23      // RN2483 hardware reset
#define BUTTON_PIN      33      // Wake-up pin (button simulating motion sensor)
#define BAT_PIN         34      // Battery voltage ADC input

// ── LoRa config ───────────────────────────────────────────────────────────────
#define LORA_FREQ       "869100000"   // 868 MHz band, must match gateway
#define LORA_SF         "sf12"        // Spreading factor 12 (max range)
#define LORA_BW         "125"         // Bandwidth 125 kHz
#define LORA_CR         "4/5"         // Coding rate
#define LORA_PWR        "14"          // TX power (dBm)
#define LORA_SYNC       "12"          // Sync word (must match gateway)

// ── Node config ───────────────────────────────────────────────────────────────
#define NODE_ID         0x03          // Mailbox node ID
#define MSG_RESERVED    0x00          // Reserved byte (TBD with gateway)
#define MAX_RETRIES     2             // 1 initial attempt + 1 retry
#define RETRY_DELAY_MS  10000         // 10 seconds between attempts
#define ACK_WINDOW_MS   5000          // Time to wait for gateway ACK (ms)

// ── ADC battery config ────────────────────────────────────────────────────────
// LiPo: 3.0V (0%) to 4.2V (100%) — after /2 divider: 1.5V to 2.1V
// 12-bit ADC, 3.3V ref: 1.5V -> ADC 1861, 2.1V -> ADC 2606
#define BAT_ADC_MIN     1861          // ADC value at 0% battery
#define BAT_ADC_MAX     2606          // ADC value at 100% battery

// ── Global objects ────────────────────────────────────────────────────────────
HardwareSerial loraSerial(1);

// ── Function declarations ─────────────────────────────────────────────────────
void    lora_autobaud();
bool    lora_init();
bool    lora_send(uint8_t battery_pct);
bool    lora_wait_ack();
uint8_t read_battery_pct();
String  bytes_to_hex(uint8_t* buf, uint8_t len);

// ─────────────────────────────────────────────────────────────────────────────
// SETUP — runs once on every wake-up
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(57600);
  Serial.println("\n[Mailbox] Woke up from deep sleep");

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[Mailbox] Wake cause: button press (motion event)");
  } else {
    Serial.println("[Mailbox] Wake cause: power on / reset");
  }

  // Init UART for RN2483
  loraSerial.begin(57600, SERIAL_8N1, RXD2, TXD2);
  loraSerial.setTimeout(2000);

  // Hardware reset RN2483
  pinMode(RST_LORA, OUTPUT);
  digitalWrite(RST_LORA, LOW);
  delay(200);
  digitalWrite(RST_LORA, HIGH);
  delay(1000);

  // Init LoRa module
  if (!lora_init()) {
    Serial.println("[Mailbox] ERROR: LoRa init failed, going back to sleep");
    goto deep_sleep;
  }

  {
    // Read battery percentage — included in every packet
    uint8_t bat_pct = read_battery_pct();
    Serial.print("[Mailbox] Battery: ");
    Serial.print(bat_pct);
    Serial.println("%");

    // Try to send up to MAX_RETRIES times
    bool ack_received = false;
    for (int attempt = 1; attempt <= MAX_RETRIES && !ack_received; attempt++) {
      Serial.print("[Mailbox] TX attempt ");
      Serial.print(attempt);
      Serial.print(" of ");
      Serial.println(MAX_RETRIES);

      if (lora_send(bat_pct)) {
        Serial.println("[Mailbox] Packet sent — waiting for ACK...");
        ack_received = lora_wait_ack();
      } else {
        Serial.println("[Mailbox] ERROR: TX failed");
      }

      if (!ack_received && attempt < MAX_RETRIES) {
        Serial.println("[Mailbox] No ACK — retrying in 10 seconds...");
        delay(RETRY_DELAY_MS);
      }
    }

    if (ack_received) {
      Serial.println("[Mailbox] ACK confirmed — going to sleep");
    } else {
      Serial.println("[Mailbox] No ACK after 3 attempts — giving up, going to sleep");
    }
  }

deep_sleep:
  Serial.println("[Mailbox] Entering deep sleep — waiting for next motion event");
  Serial.flush();

  // Arm ext0 wake-up on GPIO33 (LOW = button press, pullup keeps it HIGH at rest)
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, LOW);

  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — never reached (deep sleep restarts setup() on each wake-up)
// ─────────────────────────────────────────────────────────────────────────────
void loop() {}

// ─────────────────────────────────────────────────────────────────────────────
// read_battery_pct
// Reads ADC on GPIO34 and returns battery level as percentage (0-100)
// ─────────────────────────────────────────────────────────────────────────────
uint8_t read_battery_pct() {
  int adc = analogRead(BAT_PIN);
  Serial.print("[Mailbox] Battery ADC raw: ");
  Serial.println(adc);
  adc = constrain(adc, BAT_ADC_MIN, BAT_ADC_MAX);
  return (uint8_t) map(adc, BAT_ADC_MIN, BAT_ADC_MAX, 0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// lora_autobaud
// Sends autobaud sequence until RN2483 responds
// ─────────────────────────────────────────────────────────────────────────────
void lora_autobaud() {
  String response = "";
  while (response == "") {
    delay(1000);
    loraSerial.write((byte)0x00);
    loraSerial.write(0x55);
    loraSerial.println();
    loraSerial.println("sys get ver");
    response = loraSerial.readStringUntil('\n');
  }
  Serial.print("[LoRa] Module version: ");
  Serial.println(response);
}

// ─────────────────────────────────────────────────────────────────────────────
// lora_init
// Configures RN2483 for LoRa P2P mode
// ─────────────────────────────────────────────────────────────────────────────
bool lora_init() {
  lora_autobaud();

  loraSerial.readStringUntil('\n');  // flush leftover

  loraSerial.println("mac pause");
  String r = loraSerial.readStringUntil('\n');
  Serial.println("[LoRa] mac pause: " + r);

  loraSerial.println("radio set mod lora");        loraSerial.readStringUntil('\n');
  loraSerial.println("radio set freq " LORA_FREQ); loraSerial.readStringUntil('\n');
  loraSerial.println("radio set pwr "  LORA_PWR);  loraSerial.readStringUntil('\n');
  loraSerial.println("radio set sf "   LORA_SF);   loraSerial.readStringUntil('\n');
  loraSerial.println("radio set bw "   LORA_BW);   loraSerial.readStringUntil('\n');
  loraSerial.println("radio set cr "   LORA_CR);   loraSerial.readStringUntil('\n');
  loraSerial.println("radio set sync " LORA_SYNC); loraSerial.readStringUntil('\n');
  loraSerial.println("radio set crc on");           loraSerial.readStringUntil('\n');
  loraSerial.println("radio set iqi off");          loraSerial.readStringUntil('\n');
  loraSerial.println("radio set prlen 8");          loraSerial.readStringUntil('\n');
  loraSerial.println("radio set afcbw 41.7");       loraSerial.readStringUntil('\n');
  loraSerial.println("radio set rxbw 20.8");        loraSerial.readStringUntil('\n');
  loraSerial.println("radio set wdt 60000");        loraSerial.readStringUntil('\n');

  Serial.println("[LoRa] Init complete");
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// lora_send
// Builds payload and transmits via RN2483
// Payload: 03 | "DATA" | 00 | "mails=1;battery=XX;"
// Returns true if radio_tx_ok received
// ─────────────────────────────────────────────────────────────────────────────
bool lora_send(uint8_t battery_pct) {
  // Build data string: "mails=1;battery=XX;"
  String data = "mails=1;battery=" + String(battery_pct) + ";";

  // Build full payload as hex string
  // Fixed header: 03 | DATA(44415441) | 00
  String hex = "03";
  hex += "44415441";  // "DATA" in ASCII hex
  hex += "00";        // reserved byte

  // Append data string as ASCII hex
  for (int i = 0; i < data.length(); i++) {
    if ((uint8_t)data[i] < 0x10) hex += "0";
    hex += String((uint8_t)data[i], HEX);
  }
  hex.toUpperCase();

  Serial.print("[LoRa] Sending payload (hex): ");
  Serial.println(hex);
  Serial.print("[LoRa] Data string: ");
  Serial.println(data);

  loraSerial.println("radio tx " + hex);

  // First response: "ok" = command accepted
  String response = loraSerial.readStringUntil('\n');
  response.trim();
  Serial.print("[LoRa] TX accepted: ");
  Serial.println(response);

  if (response.indexOf("ok") != 0) {
    return false;
  }

  // Loop until radio_tx_ok or 8s timeout (SF12 airtime ~2.5s)
  loraSerial.setTimeout(8000);
  unsigned long start = millis();
  while (millis() - start < 8000) {
    String txResult = loraSerial.readStringUntil('\n');
    txResult.trim();
    if (txResult.length() > 0) {
      Serial.print("[LoRa] TX result: ");
      Serial.println(txResult);
      if (txResult.indexOf("radio_tx_ok") == 0) {
        loraSerial.setTimeout(2000);
        return true;
      }
    }
  }
  loraSerial.setTimeout(2000);
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// lora_wait_ack
// Opens RX window and checks for valid ACK from gateway
// Expected ACK (hex): 00AC0301
// Returns true if valid ACK received
// ─────────────────────────────────────────────────────────────────────────────
bool lora_wait_ack() {
  loraSerial.println("radio rx 0");
  String r = loraSerial.readStringUntil('\n');
  r.trim();

  if (r.indexOf("ok") != 0) {
    Serial.println("[LoRa] Could not open RX window");
    return false;
  }

  // Wait for incoming packet
  loraSerial.setTimeout(ACK_WINDOW_MS);
  String msg = loraSerial.readStringUntil('\n');
  loraSerial.setTimeout(2000);
  msg.trim();

  if (msg.indexOf("radio_rx") == 0) {
    // Extract hex payload from "radio_rx  <hex>"
    String received = msg.substring(msg.lastIndexOf(" ") + 1);
    received.toUpperCase();
    Serial.print("[LoRa] Received: ");
    Serial.println(received);

    // Valid ACK: 00 | AC | 03 | 01
    if (received == "00AC0301") {
      Serial.println("[LoRa] Valid ACK received");
      return true;
    } else {
      Serial.println("[LoRa] Unexpected packet — not a valid ACK");
      return false;
    }
  }

  // No packet in window — stop RX
  Serial.println("[LoRa] No packet in ACK window");
  loraSerial.println("radio rxstop");
  loraSerial.readStringUntil('\n');
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// bytes_to_hex
// Converts byte array to uppercase hex string (e.g. {0x03, 0x01} -> "0301")
// ─────────────────────────────────────────────────────────────────────────────
String bytes_to_hex(uint8_t* buf, uint8_t len) {
  String result = "";
  for (uint8_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) result += "0";
    result += String(buf[i], HEX);
  }
  result.toUpperCase();
  return result;
}