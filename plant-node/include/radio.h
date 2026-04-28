#pragma once
#include <Arduino.h>

// Protocol identifiers
#define NODE_ID       "01"
#define GATEWAY_ID    "00"
#define BROADCAST_ID  "FF"

// Timeouts (ms)
#define SYNC_TIMEOUT    60000  // max wait for SYNC after wake
#define ACK_TIMEOUT_MS  2000   // max wait for ACK after DATA
#define CMD_TIMEOUT_MS  2000   // max wait for CMD after ACK

void    initLoRa();
bool    loraSend(const String& packet);
String  loraReceive(unsigned long timeout);
String  strToHex(const String& s);