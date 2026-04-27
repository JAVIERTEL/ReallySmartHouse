#pragma once

// LoRa UART
#define LORA_RX 18
#define LORA_TX 19
#define LORA_RST 23

// Temperature & Humidity, Water sensor
#define DHT_PIN    4
#define WATER_PIN  2

// led actuator
#define LED_PIN    5

// Light sensor
#define LDR_PIN 34

// Node config
#define NODE_ID "01"
#define GATEWAY_ID "00"
#define BROADCAST_ID "FF"

// Protocol timing
#define ACK_TIMEOUT_MS 3000
#define MAX_RETRY_COUNT 3