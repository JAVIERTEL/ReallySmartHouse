#pragma once

struct SensorData {
    float temperature;
    float humidity;
    int   light;
    int   water;
};

void initSensors();
SensorData readSensors();