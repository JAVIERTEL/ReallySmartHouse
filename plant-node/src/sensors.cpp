#include <Arduino.h>
#include <DHT.h>
#include "sensors.h"
#include "pins.h"

#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);

// Starting calibration values.
// Adjust these after testing your real sensor.
static const int SOIL_DRY_RAW = 3200;
static const int SOIL_WET_RAW = 1300;
static const int SOIL_SAMPLES = 10;

static int readSoilRaw() {
    int sum = 0;

    for (int i = 0; i < SOIL_SAMPLES; i++) {
        sum += analogRead(SOIL_PIN);
        delay(5);
    }

    return sum / SOIL_SAMPLES;
}

static int convertSoilToPercent(int raw) {
    int pct = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
    pct = constrain(pct, 0, 100);
    return pct;
}

void initSensors() {
    dht.begin();

    analogReadResolution(12);
    analogSetPinAttenuation(SOIL_PIN, ADC_11db);
}

SensorData readSensors() {
    SensorData data;

    data.temperature = dht.readTemperature();
    data.humidity    = dht.readHumidity();

    int soilRaw = readSoilRaw();
    data.soilMoisture = convertSoilToPercent(soilRaw);

    return data;
}