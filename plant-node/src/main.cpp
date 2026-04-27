#include <Arduino.h>
#include "sensors.h"
#include "radio.h"
#include "plant_node.h"

unsigned long lastPrintTime = 0;

void setup() {
    Serial.begin(57600);
    Serial.println("=== plant-node booting... ===");

    initSensors();
    initPlantNode();
    initLoRa();
}

void loop() {
    checkLoRa();
    updatePlantNode();

    // Print sensor data every 5 seconds for debugging
    if (millis() - lastPrintTime >= 5000) {
        SensorData data = readSensors();
        Serial.print("T: ");
        Serial.println(data.temperature);
        Serial.print("H: ");
        Serial.println(data.humidity);
        Serial.print("L: ");
        Serial.println(data.light);
        Serial.print("S: ");
        Serial.println(data.water);
        lastPrintTime = millis();
    }
}
