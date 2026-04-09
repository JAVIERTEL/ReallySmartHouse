/*
 * ReallySmartHouse — plant-node
 * Owner   : Simone Panella (s253125)
 * Function: Plant Monitoring Node - light, temperature, humidity, soil moisture
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include "lora_protocol.h"

void setup() {
    Serial.begin(115200);
    Serial.println("=== plant-node booting... ===");

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
