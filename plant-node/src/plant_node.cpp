#include <Arduino.h>
#include "plant_node.h"
#include "pins.h"
#include "radio.h"
#include "sensors.h"


enum NodeState {
    IDLE,
    WAIT_ACK
};

static NodeState nodeState = IDLE;
static bool relayState = false;

static String lastPayload = "";
static unsigned long lastSendTime = 0;
static int retryCount = 0;



void initPlantNode() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}



String buildDataMessage(const SensorData& data) {

    float t = isnan(data.temperature) ? -1 : data.temperature;
    float h = isnan(data.humidity) ? -1 : data.humidity;




    /*
    String payload = "TYPE=PLANT;";
    payload += "T=" + String(t, 1);
    payload += ";H=" + String(h, 1);
    payload += ";L=" + String(data.light);
    payload += ";S=" + String(data.water);
    payload += ";R=" + String(relayState);

    return payload;
    */

    String msg = String(NODE_ID) + "|DATA|" + String(GATEWAY_ID) + "|";
    msg += "temp:=" + String(t, 1);
    msg += ";hum:=" + String(h, 1);
    msg += ";water:=" + String(data.water);

    return msg;

}

static bool isSyncMessage(const String& msg) {
    String expected = String(GATEWAY_ID) + "|SYNC|" + String(BROADCAST_ID) + "|CYCLE_START";
    return msg == expected;
}

static bool isAckForMe(const String& msg) {
    String expected = String(GATEWAY_ID) + "|ACK|" + String(NODE_ID) + "|OK";
    return msg == expected;
}

void handleCommand(const String& msg) {

    // =========================
    // SYNC from gateway
    // =========================
    if (isSyncMessage(msg)) {
        Serial.println("[PROTO] SYNC received");

        SensorData data = readSensors();

        Serial.print("T: ");
        Serial.println(data.temperature);

        Serial.print("H: ");
        Serial.println(data.humidity);

        Serial.print("S: ");
        Serial.println(data.water);

        lastPayload = buildDataMessage(data);

        Serial.print("[PAYLOAD] ");
        Serial.println(lastPayload);

        sendLoRa(lastPayload);

        nodeState = WAIT_ACK;
        retryCount = 0;
        lastSendTime = millis();

        return;
    }

    // =========================
    // ACK from gateway
    // =========================
    if (isAckForMe(msg)) {
        Serial.println("[PROTO] ACK received");

        nodeState = IDLE;
        retryCount = 0;
        lastPayload = "";

        return;
    }

    // =========================
    // Led control commands
    // =========================
    if (msg == "00|CMD|01|LIGHT_ON") {
        relayState = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[CMD] LIGHT ON");
        return;
    }

    if (msg == "00|CMD|01|LIGHT_OFF") {
        relayState = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("[CMD] LIGHT OFF");
        return;
    }

    Serial.print("[PROTO] Unknown message: ");
    Serial.println(msg);
}


void updatePlantNode() {
    if (nodeState == WAIT_ACK) {
        if (millis() - lastSendTime >= ACK_TIMEOUT_MS) {

            if (retryCount < MAX_RETRY_COUNT) {
                retryCount++;

                Serial.print("[PROTO] RETRY #");
                Serial.println(retryCount);

                sendLoRa(lastPayload);
                lastSendTime = millis();
            } else {
                Serial.println("[PROTO] ACK timeout, giving up");

                nodeState = IDLE;
                retryCount = 0;
                lastPayload = "";
            }
        }
    }
}