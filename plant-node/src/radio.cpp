#include <Arduino.h>
#include "radio.h"
#include "pins.h"
#include "plant_node.h"


HardwareSerial LoRaSerial(2);

void initLoRa() {
    LoRaSerial.begin(115200, SERIAL_8N1, LORA_RX, LORA_TX);
    Serial.println("[OK] LoRa UART ready @115200");
}

void sendLoRa(const String& payload) {
    LoRaSerial.println(payload);

    Serial.print("[TX] ");
    Serial.println(payload);
}

void checkLoRa() {

    while (LoRaSerial.available()) {
        String msg = LoRaSerial.readStringUntil('\n');
        msg.trim();

        if(msg.length() == 0){
            return;
        }

        Serial.print("[RX] ");
        Serial.println(msg);

        handleCommand(msg);
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if(cmd.length() == 0){
            return;
        }

        Serial.print("[SIM CMD] ");
        Serial.println(cmd);

        handleCommand(cmd);

    }
}