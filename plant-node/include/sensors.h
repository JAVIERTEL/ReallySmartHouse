#pragma once

struct SensorData {
    float temperature;
    float humidity;
    int water;
    int light;
};

void initSensors();
SensorData readSensors();