#pragma once

#include <Arduino.h>
#include "sensors.h"

void initPlantNode();
void handleCommand(const String& msg);
String buildDataMessage(const SensorData& data);
void updatePlantNode();