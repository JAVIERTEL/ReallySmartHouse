/*
 * Plant Node (ID: 01)
 * 
 *   1. Wake from deep sleep
 *   2. Init LoRa + sensors
 *   3. Wait for SYNC from gateway
 *   4. Read sensors, send DATA immediately (slot 1 = plant)
 *   5. Wait for ACK from gateway
 *   6. Wait for CMD from gateway (e.g. light on/off)
 *   7. Go back to deep sleep
 *
 * 
 * The node wakes a bit early to be ready for the SYNC.
 */

#include <Arduino.h>
#include "sensors.h"
#include "radio.h"
#include "pins.h"

// ── Deep sleep config ─────────────────────────────────────────
// Gateway cycle is 10 minutes (600s)-- Wake 1min before
#define CYCLE_MINUTES     10
#define EARLY_WAKE_SEC    60
#define SLEEP_US          ((CYCLE_MINUTES * 60 - EARLY_WAKE_SEC) * 1000000ULL)

// ── Protocol helpers ──────────────────────────────────────────

static bool isSyncMessage(const String& msg) {
    return msg.startsWith(String(GATEWAY_ID) + "|SYNC|" + String(BROADCAST_ID));
}

static bool isAckForMe(const String& msg) {
    // Gateway sends: 00|ACK|01|ok
    return msg.startsWith(String(GATEWAY_ID) + "|ACK|" + String(NODE_ID));
}

static bool isCmdForMe(const String& msg) {
    return msg.startsWith(String(GATEWAY_ID) + "|CMD|" + String(NODE_ID));
}

static String extractCmdPayload(const String& msg) {
    int lastPipe = msg.lastIndexOf('|');
    if (lastPipe < 0) return "";
    return msg.substring(lastPipe + 1);
}

static String buildDataMessage(const SensorData& data) {
    float t = isnan(data.temperature) ? -1 : data.temperature;
    float h = isnan(data.humidity)    ? -1 : data.humidity;

    String msg = String(NODE_ID) + "|DATA|" + String(GATEWAY_ID) + "|";
    msg += "temp=" + String(t, 1);
    msg += ";hum=" + String(h, 1);
    msg += ";water=" + String(data.water);
    return msg;
}

// ── LED / relay control ───────────────────────────────────────

static void handleCmd(const String& payload) {
    Serial.print("[CMD] "); Serial.println(payload);

    if (payload == "light=on") {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[LED] ON");
    }
    else if (payload == "light=off") {
        digitalWrite(LED_PIN, LOW);
        Serial.println("[LED] OFF");
    }
}

// ── Deep sleep ────────────────────────────────────────────────

static void goToSleep() {
    Serial.printf("[SLEEP] Sleeping for %d seconds\n", (int)(SLEEP_US / 1000000));
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_US);
    esp_deep_sleep_start();
}

// ── Setup: runs once per wake cycle ───────────────────────────

void setup() {
    Serial.begin(57600);
    delay(200);
    Serial.println("\n=== Plant Node waking up ===");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    initSensors();
    initLoRa();

    // ── Step 1: Wait for SYNC from gateway ────────────────────
    Serial.println("[PLANT] Waiting for SYNC...");
    String syncMsg = loraReceive(SYNC_TIMEOUT);

    if (!isSyncMessage(syncMsg)) {
        Serial.println("[ERROR] No SYNC received - going back to sleep");
        goToSleep();
        return;  // never reached (deep sleep resets)
    }
    Serial.println("[PLANT] SYNC received!");

    // ── Step 2: Read sensors ──────────────────────────────────
    SensorData data = readSensors();
    Serial.printf("[SENSORS] T=%.1f H=%.1f W=%d L=%d\n",
                  data.temperature, data.humidity, data.water, data.light);

    // ── Step 3: Send DATA immediately (plant = slot 1, no delay)
    String dataMsg = buildDataMessage(data);
    loraSend(dataMsg);

    // ── Step 4: Wait for ACK ──────────────────────────────────
    String ackMsg = loraReceive(ACK_TIMEOUT_MS);
    if (isAckForMe(ackMsg)) {
        Serial.println("[PLANT] ACK received");
    } else {
        // Retry once
        Serial.println("[PLANT] No ACK - retrying DATA once");
        loraSend(dataMsg);
        ackMsg = loraReceive(ACK_TIMEOUT_MS);
        if (isAckForMe(ackMsg)) {
            Serial.println("[PLANT] ACK received (retry)");
        } else {
            Serial.println("[PLANT] No ACK after retry - continuing anyway");
        }
    }

    // ── Step 5: Wait for CMD (optional, gateway may send light=on)
    String cmdMsg = loraReceive(CMD_TIMEOUT_MS);
    if (isCmdForMe(cmdMsg)) {
        String payload = extractCmdPayload(cmdMsg);
        handleCmd(payload);
    } else {
        Serial.println("[PLANT] No CMD received (normal)");
    }

    // ── Step 6: Sleep ─────────────────────────────────────────
    goToSleep();
}

// ── Loop: never reached with deep sleep ───────────────────────

void loop() {
    // Deep sleep resets into setup() every cycle
}