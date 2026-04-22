/*
 * ReallySmartHouse � gateway
 * Owner   : Giacomo Visintin (s253622)
 * Function: Central Gateway - collects data from all LoRa nodes and forwards to cloud via WiFi
 */
#include <Arduino.h>

#include <HardwareSerial.h>
#include <WiFi.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define BLYNK_TEMPLATE_ID "TMPL5ZzQfERwH"
#define BLYNK_TEMPLATE_NAME "Quickstart TemplateCopy"
#define BLYNK_AUTH_TOKEN "qFFhJWNX9VNj6ZFnX4sSWXDzqG70BmqG"
#include <BlynkSimpleEsp32.h>

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// ====================== HARDWARE CONFIG ======================
HardwareSerial loraSerial(1);

#define RXD2 18
#define TXD2 19
#define RST  23
#define LED_PIN 2

// ====================== BLE CONFIG ===========================
// Nome del collare che faremo advertising lato pet node
#define PET_COLLAR_NAME "PetCollar"

// UUID del servizio e caratteristica per i comandi (devono combaciare col collare)
#define PET_SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define PET_CMD_CHAR_UUID       "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

// Soglie RSSI (più negativo = più lontano)
// Valori tipici: -50 dBm = vicinissimo, -70 dBm ≈ 5-10m, -85 dBm ≈ 15-25m, -95 dBm = quasi fuori range
int RSSI_WARNING_THRESHOLD = -75;   // range 1: avviso, l'utente può richiamare il cane
int RSSI_ALARM_THRESHOLD   = -90;   // range 2: allarme + suono automatico

// Timing BLE
const unsigned long BLE_SCAN_INTERVAL = 5000UL;  // ogni 5s rivaluta la distanza
const unsigned long BLE_SCAN_DURATION = 2;       // durata scansione in secondi 
const unsigned long ALERT_COOLDOWN    = 30000UL; // non rispammare notifiche

// ====================== NETWORK CONFIG =======================
const char* ssid     = "iPhone di Giacomo";
const char* password = "labarca123";
const char* auth     = BLYNK_AUTH_TOKEN;

// ====================== PROTOCOL CONFIG ======================
#define NODE_ID_GW      "00"
#define NODE_ID_PLANT   "01"
#define NODE_ID_AIR     "02"
#define NODE_ID_MAILBOX "03"
#define BROADCAST       "FF"

// Cycle timing (ms)
const unsigned long CYCLE_PERIOD   = 600000UL; // 10 minutes
const unsigned long SLOT_DURATION = 4000UL;   // 4s per slot
const unsigned long ACK_TIMEOUT   = 1200UL;   // attesa DATA dal nodo
const unsigned long REPLY_TIMEOUT = 1500UL;   // attesa RESP a REQ
// ====================== STATE VARIABLES ======================
unsigned long lastCycleStart = 0;
String rxBuffer;

struct PlantData { float temp; float hum; int soil; bool valid; };
struct AirData   { float temp; float hum; bool valid; };
struct MailData  { int mails; bool valid; };

PlantData plant = {0, 0, 0, false};
AirData   air   = {0, 0, false};
MailData  mail  = {0, false};

volatile bool plantLight = false;

// ====================== BLE STATE ============================
BLEScan* bleScan = nullptr;
BLEClient* bleClient = nullptr;
BLERemoteCharacteristic* petCmdChar = nullptr;

bool petConnected = false;
int  lastRssi = 0;
bool lastRssiValid = false;
unsigned long lastScanTime = 0;
unsigned long lastWarningAlert = 0;
unsigned long lastAlarmAlert = 0;

enum PetZone { ZONE_SAFE, ZONE_WARNING, ZONE_ALARM, ZONE_UNKNOWN };
PetZone currentZone = ZONE_UNKNOWN;

volatile bool userRecallReq = false;  // utente preme "richiama cane" da Blynk

struct Packet {
  String sender;
  String type;
  String receiver;
  String payload;
  bool valid;
};

Packet parsePacket(const String& raw);

// ====================== LED HELPERS ==========================
void led_on()  { digitalWrite(LED_PIN, HIGH); }
void led_off() { digitalWrite(LED_PIN, LOW);  }
void led_blink(int n) {
  for (int i = 0; i < n; i++) { led_on(); delay(80); led_off(); delay(80); }
}

// ====================== LoRa LOW LEVEL =======================
String loraCmd(const String& cmd, unsigned long timeout = 1000) {
  loraSerial.println(cmd);
  loraSerial.setTimeout(timeout);
  return loraSerial.readStringUntil('\n');
}

// Convert ASCII string to hex (RN2483 radio tx wants hex payload)
String strToHex(const String& s) {
  String hex = "";
  for (size_t i = 0; i < s.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (uint8_t)s[i]);
    hex += buf;
  }
  return hex;
}

// Convert hex back to ASCII
String hexToStr(const String& h) {
  String out = "";
  for (size_t i = 0; i + 1 < h.length(); i += 2) {
    char c = (char) strtol(h.substring(i, i + 2).c_str(), NULL, 16);
    out += c;
  }
  return out;
}

// Send a packet via LoRa (blocking)
bool loraSend(const String& packet) {
  Serial.print("[TX] "); Serial.println(packet);

  // Stop any ongoing rx (ignora risposta)
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  delay(20);

  String hex = strToHex(packet);
  loraSerial.println("radio tx " + hex);

  // First response: "ok"
  loraSerial.setTimeout(500);
  String r1 = loraSerial.readStringUntil('\n');
  r1.trim();
  Serial.print("[DEBUG r1] '"); Serial.print(r1); Serial.println("'");
  if (r1.indexOf("ok") < 0) {
    Serial.print("[TX ERR r1] "); Serial.println(r1);
    return false;
  }
  // Second response: "radio_tx_ok" or "radio_err"
  loraSerial.setTimeout(2000);
  String r2 = loraSerial.readStringUntil('\n');
  r2.trim();
  if (r2.indexOf("radio_tx_ok") < 0) {
    Serial.print("[TX ERR r2] "); Serial.println(r2);
    return false;
  }
  return true;
}

// Put radio in continuous rx and wait up to timeout for a packet
// Returns decoded ASCII packet or empty string
String loraReceive(unsigned long timeout) {
  // Clean state
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  delay(20);

  // Start rx
  loraSerial.println("radio rx 0");
  loraSerial.setTimeout(500);
  String ack = loraSerial.readStringUntil('\n');
  ack.trim();
  Serial.print("[DEBUG rx ack] '"); Serial.print(ack); Serial.println("'");
  if (ack.indexOf("ok") < 0) {
    Serial.print("[RX ERR] "); Serial.println(ack);
    return "";
  }

  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (loraSerial.available()) {
      String line = loraSerial.readStringUntil('\n');
      line.trim();
      if (line.startsWith("radio_rx")) {
        int sp = line.indexOf(' ');
        if (sp < 0) return "";
        String hex = line.substring(sp + 1);
        hex.trim();
        String decoded = hexToStr(hex);
        Serial.print("[RX] "); Serial.println(decoded);
        return decoded;
      } else if (line.indexOf("radio_err") >= 0) {
        return "";
      }
    }
    delay(5);
  }

  // Timeout
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  return "";
}

// ====================== PACKET PARSING =======================

Packet parsePacket(const String& raw) {
  Packet p = {"", "", "", "", false};
  int i1 = raw.indexOf('|');
  int i2 = raw.indexOf('|', i1 + 1);
  int i3 = raw.indexOf('|', i2 + 1);
  if (i1 < 0 || i2 < 0 || i3 < 0) return p;
  p.sender   = raw.substring(0, i1);
  p.type     = raw.substring(i1 + 1, i2);
  p.receiver = raw.substring(i2 + 1, i3);
  p.payload  = raw.substring(i3 + 1);
  p.valid    = true;
  return p;
}

// Extract "key=value;..." field as float
float getField(const String& payload, const String& key, float defVal = 0) {
  int k = payload.indexOf(key + "=");
  if (k < 0) return defVal;
  int start = k + key.length() + 1;
  int end = payload.indexOf(';', start);
  if (end < 0) end = payload.length();
  return payload.substring(start, end).toFloat();
}

// ====================== DATA HANDLERS ========================
void handlePlantData(const String& payload) {
  plant.temp = getField(payload, "temp");
  plant.hum  = getField(payload, "hum");
  plant.soil = (int)getField(payload, "soil");
  plant.valid = true;
  Blynk.virtualWrite(V0, plant.temp);
  Blynk.virtualWrite(V1, plant.hum);
  Blynk.virtualWrite(V2, plant.soil);
}

void handleAirData(const String& payload) {
  air.temp = getField(payload, "temp");
  air.hum  = getField(payload, "hum");
  air.valid = true;
  Blynk.virtualWrite(V3, air.temp);
  Blynk.virtualWrite(V4, air.hum);
}

void handleMailData(const String& payload) {
  mail.mails = (int)getField(payload, "mails");
  mail.valid = true;
  Blynk.virtualWrite(V5, mail.mails);
}

// ====================== BLE FUNCTIONS ========================

// Callback di scansione: cerca il collare e salva l'RSSI
class PetScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (dev.haveName() && dev.getName() == PET_COLLAR_NAME) {
      lastRssi = dev.getRSSI();
      lastRssiValid = true;
      Serial.printf("[BLE] Pet collar found, RSSI=%d dBm\n", lastRssi);
      bleScan->stop();
    }
  }
};

void initBLE() {
  Serial.println("Initing BLE");
  BLEDevice::init("SmartHomeGW");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new PetScanCallback());
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  Serial.println("BLE ready");
}

// Connessione on-demand al collare per inviare un comando
bool connectAndSendCommand(const String& cmd) {
  Serial.printf("[BLE] Connecting to send: %s\n", cmd.c_str());

  // Trova l'indirizzo via scan veloce
  BLEScanResults results = bleScan->start(2, false);   // ← puntatore
  BLEAddress* targetAddr = nullptr;
  for (int i = 0; i < results.getCount(); i++) {       // ← freccia
    BLEAdvertisedDevice d = results.getDevice(i);      // ← freccia
    if (d.haveName() && d.getName() == PET_COLLAR_NAME) {
      targetAddr = new BLEAddress(d.getAddress());
      break;
    }
  }
  bleScan->clearResults();

  if (!targetAddr) {
    Serial.println("[BLE] Collar not found");
    return false;
  }

  bleClient = BLEDevice::createClient();
  if (!bleClient->connect(*targetAddr)) {
    Serial.println("[BLE] Connect failed");
    delete targetAddr;
    return false;
  }

  BLERemoteService* svc = bleClient->getService(PET_SERVICE_UUID);
  if (!svc) {
    Serial.println("[BLE] Service not found");
    bleClient->disconnect();
    delete targetAddr;
    return false;
  }

  BLERemoteCharacteristic* ch = svc->getCharacteristic(PET_CMD_CHAR_UUID);
  if (!ch || !ch->canWrite()) {
    Serial.println("[BLE] Characteristic not writable");
    bleClient->disconnect();
    delete targetAddr;
    return false;
  }

  // Formato comando: "CMD|<payload>"
  String msg = "CMD|" + cmd;
  ch->writeValue(msg.c_str(), msg.length());
  Serial.printf("[BLE] Sent: %s\n", msg.c_str());

  delay(100);
  bleClient->disconnect();
  delete targetAddr;
  return true;
}

// Scansione periodica RSSI
void scanPetCollar() {
  lastRssiValid = false;
  bleScan->start(BLE_SCAN_DURATION, false);
  bleScan->clearResults();
}

// Valuta la zona in base all'RSSI e scatena gli alert
void evaluatePetZone() {
  PetZone newZone;

  if (!lastRssiValid) {
    newZone = ZONE_UNKNOWN;
  } else if (lastRssi >= RSSI_WARNING_THRESHOLD) {
    newZone = ZONE_SAFE;
  } else if (lastRssi >= RSSI_ALARM_THRESHOLD) {
    newZone = ZONE_WARNING;
  } else {
    newZone = ZONE_ALARM;
  }

  // Aggiorna Blynk con RSSI e zona
  if (lastRssiValid) {
    Blynk.virtualWrite(V30, lastRssi);
  }
  Blynk.virtualWrite(V31, (int)newZone);

  unsigned long now = millis();

  // Zona WARNING: notifica Blynk, l'utente decide se richiamare
  if (newZone == ZONE_WARNING && (now - lastWarningAlert > ALERT_COOLDOWN)) {
    lastWarningAlert = now;
    Serial.println("[PET] WARNING zone");
    Blynk.logEvent("pet_warning", "Il cane si sta allontanando. Richiamalo?");
  }

  // Zona ALARM: notifica + suono automatico
  if (newZone == ZONE_ALARM && (now - lastAlarmAlert > ALERT_COOLDOWN)) {
    lastAlarmAlert = now;
    Serial.println("[PET] ALARM zone - auto sound");
    Blynk.logEvent("pet_alarm", "Cane fuori area! Suono attivato.");
    connectAndSendCommand("sound=on");
  }

  // Log cambio zona
  if (newZone != currentZone) {
    Serial.printf("[PET] Zone: %d -> %d (RSSI=%d)\n",currentZone, newZone, lastRssi);
    currentZone = newZone;
  }
}

// Gestione richiesta utente di richiamare il cane (dal bottone Blynk)
void handlePetRecall() {
  if (!userRecallReq) return;
  userRecallReq = false;
  Serial.println("[PET] User requested recall");
  if (connectAndSendCommand("sound=on")) {
    Blynk.logEvent("pet_recall", "Richiamo inviato al collare");
  } else {
    Blynk.logEvent("pet_recall_fail", "Impossibile contattare il collare");
  }
}

// ====================== CYCLE PHASES =========================
void sendSync() {
  loraSend(String(NODE_ID_GW) + "|SYNC|" + BROADCAST + "|cycle_start");
}

void sendAck(const String& to) {
  loraSend(String(NODE_ID_GW) + "|ACK|" + to + "|ok");
}

// Wait for a DATA packet from a specific node in its slot
void runNodeSlot(const String& expectedSender,void (*handler)(const String&),const String& expectedReceiver, int apriTutto) {
  unsigned long slotStart = millis();
  while (millis() - slotStart < SLOT_DURATION) {
    String raw = loraReceive(ACK_TIMEOUT);
    if (raw.length() == 0) continue;
    Packet p = parsePacket(raw);
    if (!p.valid) continue;
    if (p.sender == expectedSender && p.type == "DATA" && p.receiver == expectedReceiver) {
      handler(p.payload);
      sendAck(p.sender);

      if (apriTutto == 1) {
      loraSend(String(NODE_ID_GW) + "|CMD|" + p.sender + "|light=on");
      }

      return;
    }
  }
}

void mailboxListening(){
String raw = loraReceive(100); // short timeout
  if (raw.length() == 0) return;

  Packet p = parsePacket(raw);
  if (!p.valid) return;

  if (p.sender == NODE_ID_MAILBOX && p.type == "DATA" && p.receiver == NODE_ID_GW) {
    sendAck(p.sender);
    handleMailData(p.payload);
  }
}

// ====================== BLYNK HANDLERS =======================


// Bottone "Richiama cane" (si attiva in zona WARNING)
BLYNK_WRITE(V32) { if (param.asInt()) userRecallReq = true; }

// Opzionale: permetti di regolare soglie da Blynk
BLYNK_WRITE(V33) { RSSI_WARNING_THRESHOLD = param.asInt(); }
BLYNK_WRITE(V34) { RSSI_ALARM_THRESHOLD   = param.asInt(); }

static int fan = 0;
BLYNK_WRITE(V20) { // fan control to air node
  int value = param.asInt();
  
  if (value != fan) {  // se il valore cambia
    fan = value;       // aggiorna fan (1→1, 0→0)
  }
}

static int light = 0;
BLYNK_WRITE(V21) { // fan control to air node
  int value = param.asInt();
  
  if (value != light) {  
    light = value; 
  }
}


String str;
// ====================== LoRa INIT ============================
void initLoRa() {
  
  digitalWrite(RST, LOW);
  delay(400);
  digitalWrite(RST, HIGH);
  delay(1000);

  loraSerial.begin(57600, SERIAL_8N1, RXD2, TXD2);
  loraSerial.setTimeout(1000);
  delay(1000);

  Serial.println("Initing LoRa");
  
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  loraSerial.println("sys get ver");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("mac pause");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set mod lora");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set freq 869100000");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set pwr 14");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set sf sf7");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set afcbw 41.7");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set rxbw 20.8");  // Receiver bandwidth can be adjusted here. Lower BW equals better link budget / SNR (less noise). 
  str = loraSerial.readStringUntil('\n');   // However, the system becomes more sensitive to frequency drift (due to temp) and PPM crystal inaccuracy. 
  Serial.println(str);
  
  loraSerial.println("radio set prlen 8");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set crc on");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set iqi off");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set cr 4/5"); // Maximum reliability is 4/8 ~ overhead ratio of 2.0
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set wdt 60000"); //disable for continuous reception
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set sync 12");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);
  
  loraSerial.println("radio set bw 125");
  str = loraSerial.readStringUntil('\n');
  Serial.println(str);

  Serial.println("LoRa ready");
}

// ====================== SETUP / LOOP =========================
void setup() {
  pinMode(RST, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(57600);
  delay(500);

  initLoRa();
  //initBLE();
  led_blink(3);

  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK");
    Blynk.config(auth);
    Blynk.connect();
  } else {
    Serial.println("WiFi FAILED - continuing offline");
  }

  lastCycleStart = millis() - CYCLE_PERIOD; // trigger first cycle immediately
}

void loop() {
  Blynk.run();

  unsigned long now = millis();

  bool inCycle = (now - lastCycleStart < 10000); // ~2 slots + margin

  // --- BLE: scansione periodica del collare ---
  //if (!inCycle && now - lastScanTime >= BLE_SCAN_INTERVAL) {
   // lastScanTime = now;
   // scanPetCollar();
   // evaluatePetZone();
  //}

  // --- BLE: richiesta utente di richiamo ---
  //handlePetRecall();
  
  if (now - lastCycleStart >= CYCLE_PERIOD) {
    lastCycleStart = now;
    Serial.println("\n===== NEW CYCLE =====");
    led_on();

    // 1. Broadcast SYNC
    sendSync();

    // 2. Slot 1: Plant node
    runNodeSlot(NODE_ID_PLANT, handlePlantData, NODE_ID_GW, light);

    // 3. Slot 2: Air node
    runNodeSlot(NODE_ID_AIR, handleAirData, NODE_ID_GW, fan);

    led_off();
    Serial.println("===== CYCLE END =====");
  }

  // Mailbox
  mailboxListening();
}

