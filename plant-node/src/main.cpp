#include <Arduino.h>
#include "sensors.h"
#include "radio.h"
#include "plant_node.h"

void setup() {
    Serial.begin(115200);
    Serial.println("=== plant-node booting... ===");

    initSensors();
    initPlantNode();
    initLoRa();
}

void loop() {
    checkLoRa();
    updatePlantNode();
}
