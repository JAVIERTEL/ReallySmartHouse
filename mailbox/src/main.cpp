/*
 * ReallySmartHouse — mailbox
 * Owner   : Ignacio Díez (s253632)
 * Function: Mailbox Node - motion sensor + weight scale, solar powered
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include "lora_protocol.h"

void setup() {
    Serial.begin(115200);
    Serial.println("=== mailbox booting... ===");

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
