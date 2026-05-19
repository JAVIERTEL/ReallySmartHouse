#include <Arduino.h>
#include <DHT.h>

// ── DHT11 ─────────────────────────────────────────────────────
#define DHTPIN      4    // GPIO4
#define DHTTYPE     DHT11

// ── HW-482 Latching Relay ─────────────────────────────────────
// Single coil latching relay — one pin, toggles on each pulse
// State is held mechanically — survives deep sleep
#define RELAY_PIN   5    // GPIO5 — relay toggle pin

// ── RN2483 — Hardware Serial 2 ────────────────────────────────
#define RN_RX       13   // GPIO13 — ESP32 RX2 <- RN2483 TX
#define RN_TX       14   // GPIO14 — ESP32 TX2 -> RN2483 RX
#define RN_RST      27   // GPIO27
#define RN_BAUD     57600

// ── Battery ───────────────────────────────────────────────────
#define BATTERY_PIN 34   // GPIO34 — input only, safe for ADC

// ── Protocol constants ────────────────────────────────────────
#define NODE_ADDR     "02"
#define GATEWAY_ADDR  "00"
#define TIMEOUT_ACK   1500    // ms to wait for ACK from gateway
#define TIMEOUT_SYNC  120000  // ms to wait for SYNC after wake-up (2 minutes)
#define TIMEOUT_CMD   3000    // ms to wait for CMD after ACK
#define SLEEP_MINUTES 10      // minutes to sleep between cycles

// ── Deep sleep ────────────────────────────────────────────────
// ESP32 uses RTC timer — no extra wire needed
#define SLEEP_US  (SLEEP_MINUTES * 60 * 1000000ULL)

// ── Fan state — survives deep sleep via RTC memory ────────────
// RTC_DATA_ATTR persists across deep sleep cycles
// This prevents toggling the relay unnecessarily if state hasn't changed
RTC_DATA_ATTR bool fanState = false;

DHT dht(DHTPIN, DHTTYPE);
HardwareSerial loraSerial(2);  // UART2
String str;

// ─────────────────────────────────────────────────────────────
// Motor / relay control
// ─────────────────────────────────────────────────────────────

void setFan(bool on) {
  if (on == fanState) {
    // Already in correct state — skip pulse to avoid unnecessary toggle
    Serial.print("[RELAY] Motor already ");
    Serial.println(on ? "ON" : "OFF");
    return;
  }
  // Send a 50ms pulse to toggle the latching relay
  if (on) {
    Serial.println("[RELAY] Turning ON motor");
    digitalWrite(RELAY_PIN, HIGH);
  } else {
    Serial.println("[RELAY] Turning OFF motor");
    digitalWrite(RELAY_PIN, LOW);
  }
  delay(50);

  fanState = on;  // update RTC memory
  Serial.print("[RELAY] Motor toggled ");
  Serial.println(on ? "ON" : "OFF");
}

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
  String hex = "";
  for (int i = 0; i < payload.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (byte)payload[i]);
    hex += buf;
  }
  Serial.print("[TX] "); Serial.println(payload);
  loraSerial.println("radio tx " + hex);

  str = loraSerial.readStringUntil('\n');
  str.trim();
  Serial.print("[RN2483] "); Serial.println(str);
}

void loraCancelReceive() {
  loraSerial.println("radio rxstop");
  str = loraSerial.readStringUntil('\n');
  str.trim();
  Serial.print("[RN2483] "); Serial.println(str);
  delay(100);
}

String loraReceive(int timeout) {
  loraCancelReceive();
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
        int spaceIdx = line.lastIndexOf(' ');
        String hexData = line.substring(spaceIdx + 1);
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
  // Format: 02|DATA|00|temp=XX.X;hum=XX.X;bat=XX
  return NODE_ADDR "|DATA|" GATEWAY_ADDR "|temp=" +
         String(temp, 1) + ";hum=" + String(hum, 1) +
         ";bat=" + String(battery, 0);
}

String buildAckMessage() {
  return NODE_ADDR "|ACK|" GATEWAY_ADDR "|OK";
}

// ─────────────────────────────────────────────────────────────
// Protocol message parsers
// ─────────────────────────────────────────────────────────────

bool isSyncMessage(String msg) {
  return msg.startsWith(GATEWAY_ADDR "|SYNC|");
}

bool isAckMessage(String msg) {
  return msg.startsWith(GATEWAY_ADDR "|ACK|");
}

bool isCmdMessage(String msg) {
  return msg.startsWith(GATEWAY_ADDR "|CMD|");
}

String extractCmdValue(String msg) {
  int lastPipe = msg.lastIndexOf('|');
  return msg.substring(lastPipe + 1);
}

// ─────────────────────────────────────────────────────────────
// Battery
// ─────────────────────────────────────────────────────────────

float readBatteryPercent() {
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
  gpio_hold_en((gpio_num_t)RELAY_PIN);  // Hold relay pin state during deep sleep
  gpio_deep_sleep_hold_en();
  // No servo to detach — relay holds state on its own
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────
// Setup — runs once per wake cycle
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  // Relay pin setup
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

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
  Serial.println("[SYNC] Received - waiting before sending data");
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
  Serial.print("Bat=  "); Serial.print(battery);     Serial.println(" %");

  // ── Automatic climate control ─────────────────────────────────
  bool autoFanOn = (temperature > 78.0 || humidity > 90.0);
  setFan(autoFanOn);
   
  // ── Step 3: send DATA, wait for ACK, resend once if needed ──
  String dataMsg = buildDataMessage(temperature, humidity, battery);
  loraTransmit(dataMsg);
  
  String ack = loraReceive(TIMEOUT_ACK);
  if (!isAckMessage(ack)) {
    Serial.println("[WARN] No ACK - resending DATA once");
    loraTransmit(dataMsg);
    delay(200);
  } else {
    Serial.println("[ACK] Data acknowledged by gateway");
  }

  // ── Step 4: wait for CMD ─────────────────────────────────────
  String cmd = loraReceive(TIMEOUT_CMD);
  if (isCmdMessage(cmd)) {
    String action = extractCmdValue(cmd);
    action.trim();
    Serial.print("[CMD] Received: "); Serial.println(action);

    if (action == "fan=on") {
      setFan(true);
    } else if (action == "fan=off") {
      setFan(false);
    }

    // ── Step 5: ACK the CMD back to gateway ───────────────────
    loraTransmit(buildAckMessage());
    Serial.println("[ACK] Sent to gateway");
    delay(300);
  } else {
    Serial.println("[WARN] No CMD received within timeout");
  }

  // ── Step 6: back to sleep ────────────────────────────────────
  goToSleep();
}

void loop() {
  // Intentionally empty — deep sleep resets into setup()
}