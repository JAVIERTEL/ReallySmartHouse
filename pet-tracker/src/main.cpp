#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <HardwareSerial.h>

// =========================
// BLE config
// =========================
static const char* TARGET_BLE_NAME = "PET_BEACON";
static const int SCAN_TIME_SECONDS = 3;
static const unsigned long BLE_TIMEOUT_MS = 15000;
static const unsigned long RESCAN_DELAY_MS = 2000;

// =========================
// RN2483 config
// =========================
#define RN2483_RX 19   // ESP32 RX  <- RN2483 TX
#define RN2483_TX 18   // ESP32 TX  -> RN2483 RX
#define RN2483_RST 23  // ESP32 GPIO -> RN2483 RST

static const long RN_BAUD = 57600;

// LoRa P2P params: deben coincidir con el receptor
static const char* LORA_FREQ = "869100000";
static const char* LORA_SF   = "sf12";
static const char* LORA_BW   = "125";
static const char* LORA_SYNC = "12";

BLEScan* pBLEScan = nullptr;
HardwareSerial loraSerial(1);

bool beaconSeenInCurrentScan = false;
bool petAtHome = false;
bool missingSent = false;

unsigned long lastSeenBLE = 0;

// =========================
// Helpers
// =========================
String readRnLine(uint32_t timeoutMs = 1500) {
  unsigned long start = millis();
  String s = "";

  while (millis() - start < timeoutMs) {
    while (loraSerial.available()) {
      char c = (char)loraSerial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (s.length() > 0) return s;
      } else {
        s += c;
      }
    }
  }
  return s;
}

String asciiToHex(const String& input) {
  const char* hex = "0123456789ABCDEF";
  String out = "";
  for (size_t i = 0; i < input.length(); i++) {
    uint8_t b = (uint8_t)input[i];
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

bool rnCommand(const String& cmd, const char* expected = "ok", uint32_t timeoutMs = 1500) {
  loraSerial.println(cmd);
  String resp = readRnLine(timeoutMs);

  Serial.print("[RN2483 CMD] ");
  Serial.println(cmd);
  Serial.print("[RN2483 RSP] ");
  Serial.println(resp);

  if (expected == nullptr) return true;
  return resp.indexOf(expected) >= 0;
}

bool rn2483Init() {
  pinMode(RN2483_RST, OUTPUT);
  digitalWrite(RN2483_RST, LOW);
  delay(200);
  digitalWrite(RN2483_RST, HIGH);
  delay(500);

  String bootMsg = readRnLine(1500);
  if (bootMsg.length()) {
    Serial.print("[RN2483 BOOT] ");
    Serial.println(bootMsg);
  }

  rnCommand("sys get ver", nullptr, 1500);

  if (!rnCommand("mac pause", nullptr, 1500)) return false;
  if (!rnCommand("radio set mod lora")) return false;
  if (!rnCommand(String("radio set freq ") + LORA_FREQ)) return false;
  if (!rnCommand("radio set pwr 14")) return false;
  if (!rnCommand(String("radio set sf ") + LORA_SF)) return false;
  if (!rnCommand("radio set afcbw 41.7")) return false;
  if (!rnCommand(String("radio set rxbw ") + LORA_BW)) return false;
  if (!rnCommand("radio set prlen 8")) return false;
  if (!rnCommand("radio set crc on")) return false;
  if (!rnCommand("radio set iqi off")) return false;
  if (!rnCommand("radio set cr 4/5")) return false;
  if (!rnCommand("radio set wdt 60000")) return false;
  if (!rnCommand(String("radio set sync ") + LORA_SYNC)) return false;
  if (!rnCommand(String("radio set bw ") + LORA_BW)) return false;

  Serial.println("[RN2483] Ready for LoRa P2P");
  return true;
}

bool sendRn2483Message(const String& msg) {
  String hexPayload = asciiToHex(msg);

  loraSerial.println("radio tx " + hexPayload);

  String r1 = readRnLine(1500);
  String r2 = readRnLine(3000);

  Serial.print("[LoRa TX msg] ");
  Serial.println(msg);
  Serial.print("[LoRa TX hex] ");
  Serial.println(hexPayload);
  Serial.print("[LoRa R1] ");
  Serial.println(r1);
  Serial.print("[LoRa R2] ");
  Serial.println(r2);

  return (r1.indexOf("ok") >= 0) && (r2.indexOf("radio_tx_ok") >= 0 || r2.indexOf("radio tx ok") >= 0);
}

// =========================
// BLE callback
// =========================
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String name = advertisedDevice.getName().c_str();

    Serial.print("Found device: ");
    Serial.print(name);
    Serial.print(" | MAC: ");
    Serial.print(advertisedDevice.getAddress().toString().c_str());
    Serial.print(" | RSSI: ");
    Serial.println(advertisedDevice.getRSSI());

    if (name == TARGET_BLE_NAME) {
      beaconSeenInCurrentScan = true;
      lastSeenBLE = millis();

      Serial.println("TARGET FOUND!");
    }
  }
};

// =========================
// Setup
// =========================
void setupBLE() {
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("[BLE] Scanner ready");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== Pet Tracker booting... ===");

  setupBLE();

  loraSerial.begin(RN_BAUD, SERIAL_8N1, RN2483_RX, RN2483_TX);
  loraSerial.setTimeout(1000);

  if (!rn2483Init()) {
    Serial.println("[ERROR] RN2483 init failed");
  }

  lastSeenBLE = millis();
}

// =========================
// Loop
// =========================
void loop() {
  beaconSeenInCurrentScan = false;

  BLEScanResults foundDevices = pBLEScan->start(SCAN_TIME_SECONDS, false);
  Serial.print("[BLE] Devices found: ");
  Serial.println(foundDevices.getCount());
  pBLEScan->clearResults();

  unsigned long now = millis();

  if (beaconSeenInCurrentScan) {
    if (!petAtHome) {
      petAtHome = true;
      missingSent = false;
      Serial.println("[STATE] PET RETURNED");
      sendRn2483Message("PET_RETURNED");
    } else {
      Serial.println("[STATE] PET AT HOME");
    }
  } else {
    if (petAtHome && (now - lastSeenBLE > BLE_TIMEOUT_MS)) {
      petAtHome = false;
      Serial.println("[STATE] PET MISSING");
    } else if (petAtHome) {
      Serial.println("[STATE] PET AT HOME");
    }
  }

  if (!petAtHome && !missingSent && (now - lastSeenBLE > BLE_TIMEOUT_MS)) {
    sendRn2483Message("PET_MISSING");
    missingSent = true;
  }

  delay(RESCAN_DELAY_MS);
}





//Code for BLE connection and scanning, without LoRa functionality. It just prints the found devices and whether the target beacon is detected or not.



// #include <Arduino.h>
// #include <BLEDevice.h>
// #include <BLEUtils.h>
// #include <BLEScan.h>
// #include <BLEAdvertisedDevice.h>

// static const char* TARGET_BLE_NAME = "PET_BEACON";
// BLEScan* pBLEScan = nullptr;
// bool beaconFound = false;

// class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
//   void onResult(BLEAdvertisedDevice advertisedDevice) override {
//     String name = advertisedDevice.getName().c_str();

//     Serial.print("Found device: ");
//     Serial.print(name);
//     Serial.print(" | MAC: ");
//     Serial.print(advertisedDevice.getAddress().toString().c_str());
//     Serial.print(" | RSSI: ");
//     Serial.println(advertisedDevice.getRSSI());

//     if (name == TARGET_BLE_NAME) {
//       beaconFound = true;
//       Serial.println("TARGET FOUND!");
//     }
//   }
// };

// void setup() {
//   Serial.begin(115200);
//   Serial.println("Scanning...");

//   BLEDevice::init("");
//   pBLEScan = BLEDevice::getScan();
//   pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
//   pBLEScan->setActiveScan(true);
//   pBLEScan->setInterval(100);
//   pBLEScan->setWindow(99);
// }

// void loop() {
//   beaconFound = false;

//   BLEScanResults foundDevices = pBLEScan->start(3, false);

//   Serial.print("Devices found: ");
//   Serial.println(foundDevices.getCount());

//   if (beaconFound) {
//     Serial.println("PET AT HOME");
//   } else {
//     Serial.println("PET NOT DETECTED");
//   }

//   Serial.println("Scan done!");
//   pBLEScan->clearResults();
//   delay(2000);
// }