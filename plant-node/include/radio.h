#pragma once

#include <Arduino.h>

void    initLoRa();
bool    loraSend(const String& packet);
String  loraReceive(unsigned long timeout);
String  strToHex(const String& s);