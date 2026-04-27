#include <Arduino.h>
#include "radio.h"
#include "pins.h"

HardwareSerial LoRaSerial(2);

// ── Hex conversion ────────────────────────────────────────────

String strToHex(const String& s) {
    String hex = "";
    for (size_t i = 0; i < s.length(); i++) {
        char buf[3];
        sprintf(buf, "%02X", (uint8_t)s[i]);
        hex += buf;
    }
    return hex;
}

static String hexToStr(const String& h) {
    String out = "";
    for (size_t i = 0; i + 1 < h.length(); i += 2) {
        char c = (char)strtol(h.substring(i, i + 2).c_str(), nullptr, 16);
        out += c;
    }
    return out;
}

// ── Init ──────────────────────────────────────────────────────

void initLoRa() {
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(400);
    digitalWrite(LORA_RST, HIGH);
    delay(1000);

    LoRaSerial.begin(57600, SERIAL_8N1, LORA_RX, LORA_TX);
    LoRaSerial.setTimeout(1000);
    delay(1000);

    Serial.println("[LoRa] Initialising...");

    // Boot message
    String str = LoRaSerial.readStringUntil('\n');
    Serial.println(str);

    LoRaSerial.println("sys get ver");
    str = LoRaSerial.readStringUntil('\n');
    Serial.print("[VER] "); Serial.println(str);

    LoRaSerial.println("mac pause");
    str = LoRaSerial.readStringUntil('\n');
    Serial.println(str);

    // Radio parameters (must match gateway)
    LoRaSerial.println("radio set mod lora");    LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set freq 869100000"); LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set pwr 14");      LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set sf sf7");      LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set afcbw 41.7");  LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set rxbw 20.8");   LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set prlen 8");     LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set crc on");      LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set iqi off");     LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set cr 4/5");      LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set wdt 60000");   LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set sync 12");     LoRaSerial.readStringUntil('\n');
    LoRaSerial.println("radio set bw 125");      LoRaSerial.readStringUntil('\n');

    Serial.println("[LoRa] Ready");
}

// ── Send ──────────────────────────────────────────────────────

bool loraSend(const String& packet) {
    Serial.print("[TX] "); Serial.println(packet);

    // Stop any ongoing rx
    LoRaSerial.println("radio rxstop");
    LoRaSerial.setTimeout(200);
    LoRaSerial.readStringUntil('\n');
    delay(20);

    String hex = strToHex(packet);
    LoRaSerial.println("radio tx " + hex);

    // Response 1: "ok" (command accepted)
    LoRaSerial.setTimeout(500);
    String r1 = LoRaSerial.readStringUntil('\n');
    r1.trim();
    if (r1.indexOf("ok") < 0) {
        Serial.print("[TX ERR r1] "); Serial.println(r1);
        return false;
    }

    // Response 2: "radio_tx_ok" (transmission complete)
    LoRaSerial.setTimeout(2000);
    String r2 = LoRaSerial.readStringUntil('\n');
    r2.trim();
    if (r2.indexOf("radio_tx_ok") < 0) {
        Serial.print("[TX ERR r2] "); Serial.println(r2);
        return false;
    }

    return true;
}

// ── Receive ───────────────────────────────────────────────────

String loraReceive(unsigned long timeout) {
    // Clean state
    LoRaSerial.println("radio rxstop");
    LoRaSerial.setTimeout(200);
    LoRaSerial.readStringUntil('\n');
    delay(20);

    // Start rx
    LoRaSerial.println("radio rx 0");
    LoRaSerial.setTimeout(500);
    String ack = LoRaSerial.readStringUntil('\n');
    ack.trim();
    if (ack.indexOf("ok") < 0) {
        Serial.print("[RX ERR] "); Serial.println(ack);
        return "";
    }

    unsigned long start = millis();
    while (millis() - start < timeout) {
        if (LoRaSerial.available()) {
            String line = LoRaSerial.readStringUntil('\n');
            line.trim();

            if (line.startsWith("radio_rx")) {
                int sp = line.indexOf(' ');
                if (sp < 0) return "";
                String hex = line.substring(sp + 1);
                hex.trim();
                String decoded = hexToStr(hex);
                Serial.print("[RX] "); Serial.println(decoded);
                return decoded;
            }
            else if (line.indexOf("radio_err") >= 0) {
                return "";
            }
        }
        delay(5);
    }

    // Timeout: stop rx
    LoRaSerial.println("radio rxstop");
    LoRaSerial.setTimeout(200);
    LoRaSerial.readStringUntil('\n');
    return "";
}