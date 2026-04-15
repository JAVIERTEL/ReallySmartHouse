/*
 * Mailbox Node — Really Smart House (IoT 34346)
 * ESP32 (WROOM-32) + RN2483 LoRa P2P
 *
 * Logic:
 *   1. ESP32 wakes up via ext0 interrupt on GPIO33 (button simulates PIR motion sensor)
 *   2. Init RN2483 via UART
 *   3. Read battery level (ADC GPIO34) to decide msg_type
 *   4. Send 2-byte LoRa packet to gateway
 *   5. Wait brief RX window in case gateway sends ACK or downlink command
 *   6. Go back to deep sleep indefinitely
 *
 * Power supply: Solar panel -> TP4056 charger -> LiPo 3.7V -> ESP32
 *
 * Payload (2 bytes, hex):
 *   Byte 0: node_id  = 0x04
 *   Byte 1: msg_type = 0x01 (motion alert) | 0x02 (battery low)
 *
 * WIRING:
 *   RN2483 TX  -> ESP32 GPIO19
 *   RN2483 RX  -> ESP32 GPIO18
 *   RN2483 RST -> ESP32 GPIO23
 *   RN2483 3V3 -> 3V3
 *   RN2483 GND -> GND
 *
 *   Button     -> GPIO33 (other leg to GND, internal pullup enabled)
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
#define NODE_ID         0x04          // Mailbox node ID
#define MSG_MOTION      0x01          // Motion detected alert
#define MSG_BAT_LOW     0x02          // Battery low alert
#define BAT_LOW_THRESH  819           // ADC ~ 1.5V = LiPo at ~20% (3.3V ref, 12-bit: 1.5/3.3*4095)
#define RX_WINDOW_MS    2000          // Time to wait for gateway ACK (ms)

// ── Global objects ────────────────────────────────────────────────────────────
HardwareSerial loraSerial(1);

// ── Function declarations ─────────────────────────────────────────────────────
void     lora_autobaud();
bool     lora_init();
bool     lora_send(uint8_t node_id, uint8_t msg_type);
void     lora_wait_ack();
uint8_t  read_msg_type();
String   bytes_to_hex(uint8_t* buf, uint8_t len);

// ─────────────────────────────────────────────────────────────────────────────
// SETUP — runs once on every wake-up
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(57600);
  Serial.println("\n[Mailbox] Woke up from deep sleep");

  // Pull down GPIO33 so it stays LOW when button is not pressed
rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, LOW);

  // Log wake-up cause
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

  // Init LoRa module — if it fails, skip TX and go straight to sleep
  if (!lora_init()) {
    Serial.println("[Mailbox] ERROR: LoRa init failed, going back to sleep");
    goto deep_sleep;
  }

  {
    // Decide message type based on battery level
    uint8_t msg_type = read_msg_type();
    Serial.print("[Mailbox] Sending msg_type: 0x0");
    Serial.println(msg_type, HEX);

    // Send LoRa packet and optionally wait for ACK
    if (lora_send(NODE_ID, msg_type)) {
      Serial.println("[Mailbox] Packet sent successfully");
      lora_wait_ack();
    } else {
      Serial.println("[Mailbox] ERROR: Failed to send packet");
    }
  }

deep_sleep:
  Serial.println("[Mailbox] Going to deep sleep — waiting for next motion event");
  Serial.flush();

  // Configure GPIO33 as ext0 wake-up source (wake on HIGH = button press)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, HIGH);

  // Enter deep sleep indefinitely until next button press
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — never reached (deep sleep restarts setup() on each wake-up)
// ─────────────────────────────────────────────────────────────────────────────
void loop() {}

// ─────────────────────────────────────────────────────────────────────────────
// read_msg_type
// Reads battery ADC and returns MSG_BAT_LOW if below threshold, MSG_MOTION otherwise
// ─────────────────────────────────────────────────────────────────────────────
uint8_t read_msg_type() {
  int adc = analogRead(BAT_PIN);
  Serial.print("[Mailbox] Battery ADC raw: ");
  Serial.println(adc);
  if (adc < BAT_LOW_THRESH) {
    Serial.println("[Mailbox] Battery LOW — sending battery alert");
    return MSG_BAT_LOW;
  }
  return MSG_MOTION;
}

// ─────────────────────────────────────────────────────────────────────────────
// lora_autobaud
// Sends autobaud sequence to RN2483 until it responds
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
// Configures RN2483 for LoRa P2P transmission
// Returns true if configuration succeeds
// ─────────────────────────────────────────────────────────────────────────────
bool lora_init() {
  lora_autobaud();

  // Flush any leftover response after autobaud
  loraSerial.readStringUntil('\n');

  // Pause LoRaWAN MAC layer to use raw LoRa P2P mode
  loraSerial.println("mac pause");
  String r = loraSerial.readStringUntil('\n');
  Serial.println("[LoRa] mac pause: " + r);

  // Apply all radio settings
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
// Builds 2-byte payload and transmits it via RN2483 "radio tx" command
// Returns true if RN2483 confirms with "radio_tx_ok"
// ─────────────────────────────────────────────────────────────────────────────
bool lora_send(uint8_t node_id, uint8_t msg_type) {
  uint8_t payload[2] = { node_id, msg_type };
  String hex = bytes_to_hex(payload, 2);

  Serial.print("[LoRa] Sending payload (hex): ");
  Serial.println(hex);

  loraSerial.println("radio tx " + hex);

  // First response: "ok" (command accepted)
  String response = loraSerial.readStringUntil('\n');
  response.trim();
  Serial.print("[LoRa] TX accepted: ");
  Serial.println(response);

  if (response.indexOf("ok") != 0) {
    return false;
  }

  // Keep reading until we get radio_tx_ok or timeout
  loraSerial.setTimeout(8000);
  String txResult = "";
  unsigned long start = millis();
  while (millis() - start < 8000) {
    txResult = loraSerial.readStringUntil('\n');
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
// Opens a brief RX window to receive optional ACK or downlink from gateway
// ─────────────────────────────────────────────────────────────────────────────
void lora_wait_ack() {
  Serial.println("[LoRa] Opening RX window for ACK...");
  loraSerial.println("radio rx 0");
  String r = loraSerial.readStringUntil('\n');

  if (r.indexOf("ok") == 0) {
    loraSerial.setTimeout(RX_WINDOW_MS);
    String msg = loraSerial.readStringUntil('\n');
    loraSerial.setTimeout(2000);

    if (msg.indexOf("radio_rx") == 0) {
      Serial.print("[LoRa] ACK/downlink received: ");
      Serial.println(msg);
    } else {
      // No ACK within window — stop RX and continue to sleep
      Serial.println("[LoRa] No ACK received, sending rxstop");
      loraSerial.println("radio rxstop");
      loraSerial.readStringUntil('\n');
    }
  } else {
    Serial.println("[LoRa] Could not open RX window");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// bytes_to_hex
// Converts byte array to uppercase hex string (e.g. {0x04, 0x01} -> "0401")
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