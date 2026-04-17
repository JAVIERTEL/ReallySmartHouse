#include <Arduino.h>
#include <DHT.h>
#include "sensors.h"
#include "pins.h"

#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);

static const int LIGHT_DARK_RAW   = 3500;
static const int LIGHT_BRIGHT_RAW = 500;
static const int LIGHT_SAMPLES    = 10;



static int readLightRaw() {
    int sum = 0;

    for (int i = 0; i < LIGHT_SAMPLES; i++) {
        sum += analogRead(LDR_PIN);
        delay(5);
    }

    return sum / LIGHT_SAMPLES;
}

static int convertLightToPercent(int raw) {
    int pct = map(raw, LIGHT_DARK_RAW, LIGHT_BRIGHT_RAW, 0, 100);
    pct = constrain(pct, 0, 100);
    return pct;
}

void initSensors() {
    dht.begin();
}

SensorData readSensors() {
    SensorData data;

    data.temperature = dht.readTemperature();
    data.humidity = dht.readHumidity();

    // water sensor
    data.water = analogRead(WATER_PIN);

    int rawLight = readLightRaw();
    data.light = convertLightToPercent(rawLight);

    return data;
}