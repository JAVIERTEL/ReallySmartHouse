/*
 * ReallySmartHouse — pet-tracker
 * Owner   : Sofía Da Silva (s253625)
 * Function: Pet Tracker Node - GPS outdoor tracking + BLE indoor detection
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include "lora_protocol.h"

void setup() {
    Serial.begin(115200);
    Serial.println("=== pet-tracker booting... ===");

    if (!LoRa.begin(LORA_FREQUENCY)) {
        Serial.println("[ERROR] LoRa init failed!");
        while (true) {}
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setTxPower(LORA_TX_POWER);
    Serial.println("[OK] LoRa ready");
}

void loop() {
    delay(1000);
}
