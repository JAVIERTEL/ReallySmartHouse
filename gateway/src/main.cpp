/*
 * ReallySmartHouse — gateway
 * Owner   : Giacomo Visintin (s253622)
 * Function: Central Gateway - collects data from all LoRa nodes and forwards to cloud via WiFi
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include "lora_protocol.h"

void setup() {
    Serial.begin(115200);
    Serial.println("=== gateway booting... ===");

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
