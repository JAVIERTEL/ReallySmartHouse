#pragma once

struct SensorData {
    float temperature;
    float humidity;
    int soilMoisture;
};

void initSensors();
SensorData readSensors();