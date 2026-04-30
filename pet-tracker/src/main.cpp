#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <HardwareSerial.h>

// =========================
// BLE config
// =========================
#define PET_TRACKER_NAME  "PetTracker-01"
#define PET_TRACKER_UUID  "12345678-1234-1234-1234-123456789abc"

// =========================
// RN2483 config
// =========================
#define RN2483_RX  19
#define RN2483_TX  18
#define RN2483_RST 23
static const long RN_BAUD = 57600;

// =========================
// LoRaWAN OTAA credentials
// Integrated with Cibicom
// =========================
#define LORAWAN_DEV_EUI  "0004A30B01060D5E"
#define LORAWAN_APP_EUI  "BE7A000000001465"
#define LORAWAN_APP_KEY  "B28794FB8DCA14544A6F7D290A154B6D"

// =========================
// Timeouts
// =========================
static const unsigned long GRACE_TIMEOUT_MS  = 15000;
static const unsigned long LORAWAN_INTERVAL  = 60000;
static const unsigned long JOIN_RETRY_MS     = 30000;  // wait before retrying join

// =========================
// System state
// =========================
enum TrackerState { BLE_ACTIVE, GRACE_PERIOD, LORAWAN_BACKUP };
TrackerState state = BLE_ACTIVE;

unsigned long gracePeriodStart = 0;
unsigned long lastLoRaSend     = 0;
unsigned long lastJoinAttempt  = 0;   // ← NEW: prevents join spam
bool loraJoined = false;

BLEServer*      pServer      = nullptr;
BLEAdvertising* pAdvertising = nullptr;
HardwareSerial  loraSerial(1);

volatile bool gatewayConnected = false;

// =========================
// BLE Server Callbacks
// =========================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    gatewayConnected = true;
    Serial.println("[BLE] Gateway connected!");
  }
  void onDisconnect(BLEServer* pServer) override {
    gatewayConnected = false;
    Serial.println("[BLE] Gateway disconnected!");
    BLEDevice::startAdvertising();
  }
};

// =========================
// RN2483 helpers
// =========================
String readRnLine(uint32_t timeoutMs = 2000) {
  unsigned long start = millis();
  String s = "";
  while (millis() - start < timeoutMs) {
    while (loraSerial.available()) {
      char c = (char)loraSerial.read();
      Serial.print(c);  // Debug: print every byte received
      if (c == '\r') continue;
      if (c == '\n') { if (s.length() > 0) return s; }
      else s += c;
    }
  }
  if (s.length() > 0) return s;  // Return partial data even if no newline
  return s;
}

bool rnCommand(const String& cmd, const char* expected = "ok", uint32_t timeoutMs = 2000) {
  loraSerial.println(cmd);
  String resp = readRnLine(timeoutMs);
  Serial.println("[RN] > " + cmd + "  < " + resp);
  if (expected == nullptr) return true;
  return resp.indexOf(expected) >= 0;
}

// =========================
// BLE setup
// =========================
void setupBLE() {
  BLEDevice::init(PET_TRACKER_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(PET_TRACKER_UUID);
  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(PET_TRACKER_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinInterval(160);
  pAdvertising->setMaxInterval(240);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Broadcasting as " + String(PET_TRACKER_NAME));
}

// =========================
// LoRaWAN init (OTAA)
// =========================
bool initLoRaWAN() {
  lastJoinAttempt = millis();  // record attempt time

  pinMode(RN2483_RST, OUTPUT);
  digitalWrite(RN2483_RST, LOW);  delay(200);
  digitalWrite(RN2483_RST, HIGH); delay(1000);
  readRnLine(1500);  // consume boot banner

  // Debug: Verificar que el módulo responde
  Serial.println("[DEBUG] Testing RN2483 communication...");
  rnCommand("sys get ver", nullptr);
  rnCommand("mac get dr", "ok");
  rnCommand("mac get adr", "ok");
  delay(500);
  Serial.println("[DEBUG] RN2483 communication OK");

  loraSerial.println("mac get deveui");
  String hwEUI = readRnLine(1000);
  Serial.println("[RN] Module DevEUI: " + hwEUI);

  rnCommand("mac set deveui " LORAWAN_DEV_EUI, "ok");
  rnCommand("mac set appeui " LORAWAN_APP_EUI, "ok");
  rnCommand("mac set appkey " LORAWAN_APP_KEY, "ok");
  rnCommand("mac set adr on", "ok");
  rnCommand("mac set dr 5",   "ok");

  loraSerial.println("mac join otaa");
  Serial.println("[LoRa] Joining LoRaWAN...");

  String r1 = readRnLine(5000);
  String r2 = readRnLine(15000);
  Serial.println("[LoRa] Join R1: " + r1 + "  R2: " + r2);

  if (r1.indexOf("accepted") >= 0 || r2.indexOf("accepted") >= 0) {
    Serial.println("[LoRa] Joined successfully!");
    return true;
  }
  Serial.println("[LoRa] Join failed — will retry in 30s");
  return false;
}

// =========================
// LoRaWAN transmit
// =========================
String asciiToHex(const String& s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (uint8_t)s[i]);
    out += buf;
  }
  return out;
}

void sendLoRaWAN(const String& msg) {
  String cmd = "mac tx uncnf 1 " + asciiToHex(msg);
  loraSerial.println(cmd);
  String r1 = readRnLine(2000);
  String r2 = readRnLine(6000);
  Serial.println("[LoRa] TX '" + msg + "' -> " + r1 + " / " + r2);
}

// =========================
// Setup
// =========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Pet Tracker booting... ===");

  loraSerial.begin(RN_BAUD, SERIAL_8N1, RN2483_RX, RN2483_TX);
  loraSerial.setTimeout(1000);

  setupBLE();

  Serial.println("[LoRa] Standby — waiting for BLE loss");
  lastLoRaSend = millis();
}

// =========================
// Loop — state machine
// =========================
void loop() {
  unsigned long now   = millis();
  bool          bleOK = gatewayConnected;

  switch (state) {

    case BLE_ACTIVE:
      if (!bleOK) {
        gracePeriodStart = now;
        state = GRACE_PERIOD;
        Serial.println("[STATE] BLE lost -> GRACE_PERIOD");
      }
      break;

    case GRACE_PERIOD:
      if (bleOK) {
        state = BLE_ACTIVE;
        Serial.println("[STATE] BLE recovered -> BLE_ACTIVE");
      } else if (now - gracePeriodStart > GRACE_TIMEOUT_MS) {
        // Only attempt join if not already joined AND retry window has passed
        if (!loraJoined && (now - lastJoinAttempt > JOIN_RETRY_MS)) {
          Serial.println("[STATE] Grace timeout -> activating LoRaWAN backup");
          loraJoined = initLoRaWAN();
        }
        if (loraJoined) {
          sendLoRaWAN("PET_MISSING");
          lastLoRaSend = now;
          state = LORAWAN_BACKUP;
        }
      }
      break;

    case LORAWAN_BACKUP:
      if (bleOK) {
        if (loraJoined) sendLoRaWAN("PET_RETURNED");
        state = BLE_ACTIVE;
        Serial.println("[STATE] Back in BLE range -> BLE_ACTIVE");
      } else if (now - lastLoRaSend > LORAWAN_INTERVAL) {
        sendLoRaWAN("PET_MISSING");
        lastLoRaSend = now;
      }
      break;
  }

  delay(2000);
}