#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>

BLEServer* pServer = nullptr;
BLEAdvertising* pAdvertising = nullptr;

void setup() {
  Serial.begin(115200);
  Serial.println("Starting PET_BEACON...");

  BLEDevice::init("PET_BEACON");
  pServer = BLEDevice::createServer();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  Serial.println("PET_BEACON advertising");
}

void loop() {
  delay(2000);
}