#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define PET_TRACKER_UUID  "12345678-1234-1234-1234-123456789abc"
#define PET_TRACKER_NAME  "PetTracker-01"

BLEScan* pBLEScan       = nullptr;
BLEClient* pClient      = nullptr;
BLEAddress* trackerAddr = nullptr;

bool trackerFound     = false;
bool connected        = false;
unsigned long lastRSSI = 0;

// =========================
// Conection callbacks 
// =========================
class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    connected = true;
    Serial.println("[BLE] Connected to tracker!");
  }
  void onDisconnect(BLEClient* c) override {
    connected = false;
    Serial.println("[BLE] Disconnected from tracker");
    trackerFound = false;  // Vuelve a escanear
  }
};

// =========================
// Callback  scan — find the UUID of the tracker
// =========================
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (dev.haveServiceUUID() &&
        dev.isAdvertisingService(BLEUUID(PET_TRACKER_UUID))) {
      Serial.printf("[BLE] Tracker found! RSSI: %d\n", dev.getRSSI());
      trackerAddr  = new BLEAddress(dev.getAddress());
      trackerFound = true;
      pBLEScan->stop();
    }
  }
};

// =========================
// Conect the tracker
// =========================
bool connectToTracker() {
  Serial.println("[BLE] Connecting...");
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());

  if (!pClient->connect(*trackerAddr)) {
    Serial.println("[BLE] Connection FAILED");
    return false;
  }
  Serial.println("[BLE] Connected OK");
  return true;
}

// =========================
// Distance aproximate to RSSI
// =========================
float rssiToDistance(int rssi) {
  // Path-loss model: RSSI_1m = -69 dBm (típico ESP32 BLE)
  return pow(10.0, (-69.0 - rssi) / 20.0);
}

// =========================
// Setup
// =========================
void setup() {
  Serial.begin(115200);
  Serial.println("=== Pet Gateway booting... ===");

  BLEDevice::init("PetGateway");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

// =========================
// Loop
// =========================
void loop() {
  if (!trackerFound) {
    Serial.println("[BLE] Scanning for tracker...");
    pBLEScan->start(5, false);  // 5 segundos de scan
    pBLEScan->clearResults();
    delay(1000);
    return;
  }

  if (!connected) {
    connectToTracker();
    delay(1000);
    return;
  }

  // Tracker connected — read RSSI each 3 seconds
  if (millis() - lastRSSI > 3000) {
    int rssi = pClient->getRssi();
    float dist = rssiToDistance(rssi);
    Serial.printf("[GATEWAY] Dog RSSI: %d dBm  |  ~%.1f m\n", rssi, dist);
    // Aquí Giacomo añade el envío a Blynk
    lastRSSI = millis();
  }

  delay(1000);
}