#pragma once

#include <Arduino.h>

void initLoRa();
void sendLoRa(const String& payload);
void checkLoRa();