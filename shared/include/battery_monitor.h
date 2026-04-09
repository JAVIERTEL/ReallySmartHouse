#pragma once
#include <Arduino.h>

#define BATTERY_PIN        34
#define BATTERY_LOW_PCT    20
#define BATTERY_FULL_MV    4200
#define BATTERY_EMPTY_MV   3200

inline float getBatteryPercent() {
    int raw = analogRead(BATTERY_PIN);
    float mv = (raw / 4095.0f) * 3300.0f * 2.0f;
    float pct = ((mv - BATTERY_EMPTY_MV) / (float)(BATTERY_FULL_MV - BATTERY_EMPTY_MV)) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}
