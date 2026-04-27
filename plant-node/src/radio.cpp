#include <Arduino.h>
#include "radio.h"
#include "pins.h"
#include "plant_node.h"


HardwareSerial LoRaSerial(2);

void initLoRa() {
digitalWrite(LORA_RST, LOW);
  delay(400);
  digitalWrite(LORA_RST, HIGH);
  delay(1000);

  LoRaSerial.begin(57600, SERIAL_8N1, LORA_RX, LORA_TX);
  LoRaSerial.setTimeout(1000);
  delay(1000);

  Serial.println("Initing LoRa");
  
  String str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  LoRaSerial.println("sys get ver");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("mac pause");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set mod lora");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set freq 869100000");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set pwr 14");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set sf sf7");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set afcbw 41.7");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set rxbw 20.8");  // Receiver bandwidth can be adjusted here. Lower BW equals better link budget / SNR (less noise). 
  str = LoRaSerial.readStringUntil('\n');   // However, the system becomes more sensitive to frequency drift (due to temp) and PPM crystal inaccuracy. 
  Serial.println(str);
  
  LoRaSerial.println("radio set prlen 8");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set crc on");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set iqi off");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set cr 4/5"); // Maximum reliability is 4/8 ~ overhead ratio of 2.0
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set wdt 60000"); //disable for continuous reception
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set sync 12");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);
  
  LoRaSerial.println("radio set bw 125");
  str = LoRaSerial.readStringUntil('\n');
  Serial.println(str);

  Serial.println("LoRa ready");
}

bool sendLora(const String& packet) {
  Serial.print("[TX] "); Serial.println(packet);

  // Stop any ongoing rx (ignora risposta)
  LoRaSerial.println("radio rxstop");
  LoRaSerial.setTimeout(200);
  LoRaSerial.readStringUntil('\n');
  delay(20);

  String hex = strToHex(packet);
  LoRaSerial.println("radio tx " + hex);

  // First response: "ok"
  LoRaSerial.setTimeout(500);
  String r1 = LoRaSerial.readStringUntil('\n');
  r1.trim();
  Serial.print("[DEBUG r1] '"); Serial.print(r1); Serial.println("'");
  if (r1.indexOf("ok") < 0) {
    Serial.print("[TX ERR r1] "); Serial.println(r1);
    return false;
  }
  // Second response: "radio_tx_ok" or "radio_err"
  LoRaSerial.setTimeout(2000);
  String r2 = LoRaSerial.readStringUntil('\n');
  r2.trim();
  if (r2.indexOf("radio_tx_ok") < 0) {
    Serial.print("[TX ERR r2] "); Serial.println(r2);
    return false;
  }
  return true;
}

String strToHex(const String& s) {
  String hex = "";
  for (size_t i = 0; i < s.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (uint8_t)s[i]);
    hex += buf;
  }
  return hex;
}

void checkLoRa() {

    LoRaSerial.println("radio rx 0"); 
    int timeout = 4000; // ms

    unsigned long start = millis();
    while (millis() - start < timeout) {
        if (LoRaSerial.available()) {
            String line = LoRaSerial.readStringUntil('\n');
            line.trim();
            if (line.startsWith("radio_rx")) {
                // Format: radio_rx  <hexdata>
                int spaceIdx = line.lastIndexOf(' ');
                String hexData = line.substring(spaceIdx + 1);

                // Decode hex to ASCII string
                String decoded = "";
                for (int i = 0; i < hexData.length(); i += 2) {
                    decoded += (char)strtol(hexData.substring(i, i + 2).c_str(), nullptr, 16);
                }
                Serial.print("[RX] "); Serial.println(decoded);
                if (isSyncMessage(decoded)){
                    SendData();
                    continue;
                }
            }
            if (line == "radio_err") {
                Serial.println("[RX] radio_err - timeout or bad packet");
                return;
            }
        }
    }

    /*while (LoRaSerial.available()) {
        String msg = LoRaSerial.readStringUntil('\n');
        msg.trim();

        if(msg.length() == 0){
            return;
        }

        Serial.print("[RX] ");
        Serial.println(msg);

        handleCommand(msg);
    }*/

    /*if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if(cmd.length() == 0){
            return;
        }

        Serial.print("[SIM CMD] ");
        Serial.println(cmd);

        handleCommand(cmd);

    }*/
}

void SendData() {
    SensorData data = readSensors();
    String payload = buildDataMessage(data);
    sendLoRa(payload);
}