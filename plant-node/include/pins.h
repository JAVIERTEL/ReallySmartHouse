#pragma once

// ── LoRa RN2483 (UART2) ──────────────────────────────────────
#define LORA_RX     18   // ESP32 RX ← RN2483 TX
#define LORA_TX     19   // ESP32 TX → RN2483 RX
#define LORA_RST    23   // RN2483 reset

// ── DHT11 ─────────────────────────────────────────────────────
#define DHT_PIN     4

// ── LED ───────────────────────────────────
#define LED_PIN     2

// ── Sensors ───────────────────────────────────────────────────
#define LDR_PIN     35   // light sensor (ADC, input-only pin)
#define WATER_PIN   34   // water/soil sensor (ADC, input-only pin)