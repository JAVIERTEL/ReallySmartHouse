#include <Arduino.h>
#include <DHT.h>
#include <ESP32Servo.h>

// ── DHT11 ─────────────────────────────────────────────────────
#define DHTPIN      4    // GPIO4
#define DHTTYPE     DHT11

// ── Servo ─────────────────────────────────────────────────────
#define SERVO_PIN   5    // GPIO5

// ── RN2483 — Hardware Serial 2 ────────────────────────────────
// Change RN_RX and RN_TX to whichever free GPIOs you have
// Avoid: 6-11 (flash), 34/35/36/39 (input only), 0/2/12/15 (boot pins)
#define RN_RX       13   // GPIO13 — ESP32 RX2 <- RN2483 TX
#define RN_TX       14   // GPIO14 — ESP32 TX2 -> RN2483 RX
#define RN_RST      27   // GPIO27
#define RN_BAUD     57600

// ── Battery ───────────────────────────────────────────────────
#define BATTERY_PIN 34   // GPIO34 — input only, safe for ADC

// ── Protocol constants ────────────────────────────────────────
#define NODE_ADDR     "02"
#define GATEWAY_ADDR  "00"
#define TIMEOUT_ACK   1500   // ms to wait for ACK from gateway
#define TIMEOUT_SYNC  15000   // ms to wait for SYNC after wake-up
#define TIMEOUT_CMD   3000   // ms to wait for CMD after ACK
#define SLEEP_MINUTES 2

// ── Deep sleep ────────────────────────────────────────────────
// ESP32 uses RTC timer — no extra wire needed
#define SLEEP_US  (SLEEP_MINUTES * 60 * 1000000ULL)

DHT dht(DHTPIN, DHTTYPE);
Servo fanServo;
HardwareSerial loraSerial(2);  // UART2 — pins set in initLoRa()
String str;

// ─────────────────────────────────────────────────────────────
// LoRa P2P init — settings agreed with gateway team
// ─────────────────────────────────────────────────────────────

void initLoRa() {
  // Hardware reset
  pinMode(RN_RST, OUTPUT);
  digitalWrite(RN_RST, LOW);
  delay(400);
  digitalWrite(RN_RST, HIGH);
  delay(1000);

  loraSerial.begin(RN_BAUD, SERIAL_8N1, RN_RX, RN_TX);
  loraSerial.setTimeout(1000);
  delay(1000);

  Serial.println("[LoRa] Initialising RN2483...");

  // Read boot message
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("sys get ver");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  // Pause LoRaWAN stack — required before P2P radio commands
  loraSerial.println("mac pause");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set mod lora");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set freq 869100000");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set pwr 14");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set sf sf7");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set afcbw 41.7");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  // Lower RX bandwidth = better SNR but more sensitive to frequency drift
  loraSerial.println("radio set rxbw 20.8");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set prlen 8");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set crc on");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set iqi off");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set cr 4/5");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  // Watchdog: 60s max RX window before timeout
  loraSerial.println("radio set wdt 60000");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set sync 12");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  loraSerial.println("radio set bw 125");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  Serial.println("[LoRa] Ready");
}

// ─────────────────────────────────────────────────────────────
// LoRa P2P transmit and receive
// ─────────────────────────────────────────────────────────────

void loraTransmit(String payload) {
  // Encode payload as hex — RN2483 radio tx expects hex string
  String hex = "";
  for (int i = 0; i < payload.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (byte)payload[i]);
    hex += buf;
  }
  Serial.print("[TX] "); Serial.println(payload);
  loraSerial.println("radio tx " + hex);

  // Wait for "radio_tx_ok"
  str = loraSerial.readStringUntil('\n');
  str.trim();
  Serial.print("[RN2483] "); Serial.println(str);
}

// ─────────────────────────────────────────────────────────────
// TX to RX mode
// ─────────────────────────────────────────────────────────────

void loraCancelReceive() {
  loraSerial.println("radio rxstop");
  str = loraSerial.readStringUntil('\n');
  str.trim();
  Serial.print("[RN2483] "); Serial.println(str);
  delay(100);
}

String loraReceive(int timeout) {
  loraCancelReceive();  // TX to RX
  // Put radio into continuous receive mode
  loraSerial.println("radio rx 0");
  str = loraSerial.readStringUntil('\n');
  str.trim();
  Serial.print("[RN2483] "); Serial.println(str);  // should print "ok"

  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (loraSerial.available()) {
      String line = loraSerial.readStringUntil('\n');
      line.trim();
      if (line.startsWith("radio_rx")) {
        // Format: radio_rx  <hexdata>
        int spaceIdx = line.lastIndexOf(' ');
        String hexData = line.substring(spaceIdx + 1);

        // Decode hex to ASCII string
        String decoded = "";
        for (int i = 0; i < hexData.length(); i += 2) {
          decoded += (char)strtol(hexData.substring(i, i + 2).c_str(), nullptr, 16);
        }
        Serial.print("[RX] "); Serial.println(decoded);
        return decoded;
      }
      if (line == "radio_err") {
        Serial.println("[RX] radio_err - timeout or bad packet");
        return "";
      }
    }
  }
  return "";  // timeout
}

// ─────────────────────────────────────────────────────────────
// Protocol message builders
// ─────────────────────────────────────────────────────────────

String buildDataMessage(float temp, float hum, float battery) {
  // Format: 02|DATA|00|temp:XX.X;hum:XX.X;bat:XX
  return NODE_ADDR "|DATA|" GATEWAY_ADDR "|temp=" +
         String(temp, 1) + ";hum=" + String(hum, 1);
}

String buildAckMessage() {
  // Format: 02|ACK|00|OK
  return NODE_ADDR "|ACK|" GATEWAY_ADDR "|OK";
}

// ─────────────────────────────────────────────────────────────
// Protocol message parsers
// ─────────────────────────────────────────────────────────────

bool isSyncMessage(String msg) {
  // Expected: 00|SYNC|FF|cycle_start
  return msg.startsWith(GATEWAY_ADDR "|SYNC|");
}

bool isAckMessage(String msg) {
  // Expected: 00|ACK|02|OK
  return msg.startsWith(GATEWAY_ADDR "|ACK|");
}

bool isCmdMessage(String msg) {
  // Expected: 00|CMD|02|VENT:ON  or  00|CMD|02|VENT:OFF
  return msg.startsWith(GATEWAY_ADDR "|CMD|" NODE_ADDR "|");
}

String extractCmdValue(String msg) {
  // Returns "VENT:ON" or "VENT:OFF"
  int lastPipe = msg.lastIndexOf('|');
  return msg.substring(lastPipe + 1);
}

// ─────────────────────────────────────────────────────────────
// Battery
// ─────────────────────────────────────────────────────────────

float readBatteryPercent() {
  // ESP32 ADC: 0-4095 for 0-3.3V
  // With 1:2 voltage divider: full (~4.2V) -> ~2.1V -> ~2607
  //                           empty (~3.0V) -> ~1.5V -> ~1862
  long sum = 0;
  for (int i = 0; i < 5; i++) { sum += analogRead(BATTERY_PIN); delay(10); }
  float raw = sum / 5.0;
  return constrain(map(raw, 1862, 2607, 0, 100), 0.0, 100.0);
}

// ─────────────────────────────────────────────────────────────
// Deep sleep
// ─────────────────────────────────────────────────────────────

void goToSleep() {
  Serial.println("[SLEEP] Entering deep sleep for 10 minutes");
  Serial.flush();
  fanServo.detach();
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────
// Setup — runs once per wake cycle
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  fanServo.attach(SERVO_PIN);
  dht.begin();
  initLoRa();

  Serial.println("\n[WAKE] Air Node awake - waiting for SYNC");

  // ── Step 1: wait for SYNC from gateway ──────────────────────
  String sync = loraReceive(TIMEOUT_SYNC);
  if (!isSyncMessage(sync)) {
    Serial.println("[ERROR] No SYNC received - going back to sleep");
    goToSleep();
    return;
  }
  Serial.println("[SYNC] Received - waiting 4 seconds before sending data");
  delay(3800);

  // ── Step 2: read sensors ─────────────────────────────────────
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();
  float battery     = readBatteryPercent();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("[ERROR] DHT11 read failed - going back to sleep");
    goToSleep();
    return;
  }

  Serial.print("Temp= "); Serial.print(temperature); Serial.println(" C");
  Serial.print("Hum=  "); Serial.print(humidity);    Serial.println(" %");
  //Serial.print("Bat:  "); Serial.print(battery);     Serial.println(" %");
  // ── Automatic climate control ─────────────────────────────────
  bool autoFanOn = (temperature > 22.0 || humidity > 20.0);
  fanServo.write(autoFanOn ? 180 : 0);
  Serial.print("[AUTO] Fan: "); Serial.println(autoFanOn ? "ON" : "OFF");
  
  // ── Step 3: send DATA, wait for ACK, resend once if needed ──
  String dataMsg = buildDataMessage(temperature, humidity, battery);
  loraTransmit(dataMsg);

  String ack = loraReceive(TIMEOUT_ACK);
  if (!isAckMessage(ack)) {
    Serial.println("[WARN] No ACK - resending DATA once");
    loraTransmit(dataMsg); // resend only once as per protocol
    delay(200); 
  } else {
    Serial.println("[ACK] Data acknowledged by gateway");
  }

  // ── Step 4: wait for CMD ─────────────────────────────────────
  loraCancelReceive();  // TX to RX
  delay(200);
  String cmd = loraReceive(TIMEOUT_CMD);
  if (isCmdMessage(cmd)) {
    String action = extractCmdValue(cmd);
    Serial.print("[CMD] Received: "); Serial.println(action);

    if (action == "fan=on") {
      fanServo.write(180);
      Serial.println("[SERVO] Fan ON");
    } else if (action == "fan=off") {
      fanServo.write(0);
      Serial.println("[SERVO] Fan OFF");
    }
    // ── Step 5: ACK the CMD back to gateway ───────────────────
    loraTransmit(buildAckMessage());
    Serial.println("[ACK] Sent to gateway");
  } else {
    Serial.println("[WARN] No CMD received within timeout");
  }

  // ── Step 6: back to sleep ────────────────────────────────────
  goToSleep();
}

void loop() {
  // Intentionally empty — deep sleep resets into setup()
}