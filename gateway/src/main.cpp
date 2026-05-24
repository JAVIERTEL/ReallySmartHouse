/*
 * ReallySmartHouse gateway
 * Authors: Giacomo Visintin (s253622), Nicolò Costa (s253623)
 * This is the main gateway of our smart house. It collects data from the LoRa
 * nodes, pushes everything to the cloud through WiFi/MQTT, and also takes care
 * of the BLE pet tracker.
 */
#include <Arduino.h>

#include <HardwareSerial.h>
#include <WiFi.h>

#include <NimBLEDevice.h>

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// ---------------------- Hardware pins -------------------------
// Pins and serial port we use to talk to the RN2483 LoRa module
HardwareSerial loraSerial(1);

#define RXD2 18
#define TXD2 19
#define RST  23
#define LED_PIN 2

// ---------------------- BLE stuff ------------------------------
// Name and UUID that the pet collar uses while advertising itself
#define PET_TRACKER_UUID "12345678-1234-1234-1234-123456789abc" 
#define PET_TRACKER_NAME "PetTracker-01"

// Service and command UUIDs, these need to match the ones in the collar firmware
#define PET_SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define PET_CMD_CHAR_UUID       "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

// Rough RSSI thresholds to guess how far the collar is
// -50 dBm = very close, -70 dBm = around 5-10 m, -85 dBm = 15-25 m, -95 dBm = barely reachable
int RSSI_WARNING_THRESHOLD = -75;   
int RSSI_ALARM_THRESHOLD   = -90;   
// BLE timing
const unsigned long BLE_SCAN_INTERVAL = 5000UL;  // refresh the distance estimate every 5 seconds
const unsigned long BLE_SCAN_DURATION = 2;       // how long each scan lasts (seconds)
const unsigned long ALERT_COOLDOWN    = 30000UL; // wait a bit before re-sending the same alert

// ---------------------- WiFi and MQTT -----------------
// Credentials for the hotspot and the HiveMQ broker we use
const char* ssid     = "iPhone di Giacomo";
const char* password = "labarca123";

// HiveMQ Cloud broker
const char* MQTT_HOST = "14cae6d240b2426398a24b5f85cda552.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "group7";
const char* MQTT_PASS = "Groupgroup7";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// Topics the gateway publishes to and listens on
#define TOPIC_PLANT_DATA     "home/plant/data"
#define TOPIC_AIR_DATA       "home/air/data"
#define TOPIC_MAIL_DATA      "home/mail/data"
#define TOPIC_PET_STATUS     "home/pet/status"
#define TOPIC_CMD_FAN        "home/cmd/fan"
#define TOPIC_CMD_LIGHT      "home/cmd/light"
#define TOPIC_CMD_PET_RECALL "home/cmd/pet_recall"

// ---------------------- LoRa protocol --------------------
// Short IDs for each node in our network
#define NODE_ID_GW      "00"
#define NODE_ID_PLANT   "01"
#define NODE_ID_AIR     "02"
#define NODE_ID_MAILBOX "03"
#define BROADCAST       "FF"

// Timing for the LoRa cycle, all in milliseconds
const unsigned long CYCLE_PERIOD   = 600000UL; // a full cycle every 10 minutes
const unsigned long SLOT_DURATION = 3500UL;   // how long each node gets to talk
const unsigned long ACK_TIMEOUT   = 1200UL;   // max wait for a DATA packet from a node
const unsigned long REPLY_TIMEOUT = 1500UL;   // max wait for a reply after a request
// ---------------------- Runtime state --------------------------
// Variables that change while the gateway is running
unsigned long lastCycleStart = 0;
String rxBuffer;

struct PlantData { float temp; float hum; int soil; bool valid; };
struct AirData   { float temp; float hum; bool valid; };
struct MailData  { int mails; bool valid; };

PlantData plant = {0, 0, 0, false};
AirData   air   = {0, 0, false};
MailData  mail  = {0, false};

static int fan = 0;
static int light = 0;

volatile bool plantLight = false;

// ---------------------- BLE runtime state ----------------------
// Stuff we need to keep track of while talking to the collar
NimBLEScan* bleScan = nullptr;
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* petCmdChar = nullptr;

unsigned long lastScanTime = 0;

volatile bool fanCmdPending = false;
volatile bool lightCmdPending = false;

struct Packet {
  String sender;
  String type;
  String receiver;
  String payload;
  bool valid;
};

Packet parsePacket(const String& raw);

// ---------------------- LED helpers ----------------------------
// Quick helpers to flash the onboard LED so we can see what the gateway is doing
void led_on()  { digitalWrite(LED_PIN, HIGH); }
void led_off() { digitalWrite(LED_PIN, LOW);  }
void led_blink(int n) {
  for (int i = 0; i < n; i++) { led_on(); delay(80); led_off(); delay(80); }
}

// ---------------------- Low-level LoRa helpers -----------------

// Turns a regular text string into hex, which is what the RN2483 wants when transmitting
String strToHex(const String& s) {
  String hex = "";
  for (size_t i = 0; i < s.length(); i++) {
    char buf[3];
    sprintf(buf, "%02X", (uint8_t)s[i]);
    hex += buf;
  }
  return hex;
}

// Takes a hex payload coming from LoRa and turns it back into readable text
String hexToStr(const String& h) {
  String out = "";
  for (size_t i = 0; i + 1 < h.length(); i += 2) {
    char c = (char) strtol(h.substring(i, i + 2).c_str(), NULL, 16);
    out += c;
  }
  return out;
}

// Sends one packet over LoRa, then waits for the module to confirm it actually went out
bool loraSend(const String& packet) {
  Serial.print("[TX] "); Serial.println(packet);

  // If the module is in receive mode, stop it first
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  delay(20);

  String hex = strToHex(packet);
  loraSerial.println("radio tx " + hex);

  // First answer: the module says "ok, I accepted the command"
  loraSerial.setTimeout(500);
  String r1 = loraSerial.readStringUntil('\n');
  r1.trim();
  Serial.print("[DEBUG r1] '"); Serial.print(r1); Serial.println("'");
  if (r1.indexOf("ok") < 0) {
    Serial.print("[TX ERR r1] "); Serial.println(r1);
    return false;
  }
  // Second answer: the module tells us if the transmission really worked
  loraSerial.setTimeout(2000);
  String r2 = loraSerial.readStringUntil('\n');
  r2.trim();
  if (r2.indexOf("radio_tx_ok") < 0) {
    Serial.print("[TX ERR r2] "); Serial.println(r2);
    return false;
  }
  return true;
}

// Puts the radio in receive mode and waits until something comes in (or we time out)
// Returns the decoded text, or an empty string if nothing useful arrived
String loraReceive(unsigned long timeout) {
  // Make sure we are not already receiving from a previous call
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  delay(20);

  // Start continuous receive
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

  // Timed out without receiving anything
  loraSerial.println("radio rxstop");
  loraSerial.setTimeout(200);
  loraSerial.readStringUntil('\n');
  return "";
}

// ---------------------- Packet parsing -------------------------

// Splits an incoming string into its four parts: sender, type, receiver, payload
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

// Pulls a single numeric field out of a "key=value;key=value" style payload
float getField(const String& payload, const String& key, float defVal = 0) {
  int k = payload.indexOf(key + "=");
  if (k < 0) return defVal;
  int start = k + key.length() + 1;
  int end = payload.indexOf(';', start);
  if (end < 0) end = payload.length();
  return payload.substring(start, end).toFloat();
}

// ----------------------- Data handlers -------------------------------
// Saves the new plant readings locally and forwards the raw payload to MQTT
void handlePlantData(const String& payload) {
  plant.temp = getField(payload, "temp");
  plant.hum  = getField(payload, "hum");
  plant.soil = (int)getField(payload, "water");
  plant.valid = true;
  mqtt.publish(TOPIC_PLANT_DATA, payload.c_str(),true);
  Serial.print("[MQTT PUB] plant: "); Serial.println(payload);
}

// Same as above but for the air sensor data
void handleAirData(const String& payload) {
  air.temp = getField(payload, "temp");
  air.hum  = getField(payload, "hum");
  air.valid = true;
  mqtt.publish(TOPIC_AIR_DATA, payload.c_str(),true);
  Serial.print("[MQTT PUB] air: "); Serial.println(payload);
}

// Same idea for the mailbox node
void handleMailData(const String& payload) {
  mail.mails = (int)getField(payload, "mails");
  mail.valid = true;
  mqtt.publish(TOPIC_MAIL_DATA, payload.c_str(),true);
  Serial.print("[MQTT PUB] mail: "); Serial.println(payload);
}

// ------------------- BLE functions ---------------------

// Where we keep the current state of the pet tracker
NimBLEAddress* trackerAddr = nullptr;
bool trackerFound = false;
bool trackerConnected = false;
unsigned long lastRSSIRead = 0;

// How we sample and report the distance
#define RSSI_SAMPLES     10
#define SEND_INTERVAL_MS 30000   // publish the best distance every 30 seconds
#define SAMPLE_INTERVAL_MS 3000  // grab a new RSSI sample every 3 seconds

float         distBuffer[RSSI_SAMPLES];
int           distCount      = 0;
unsigned long lastSampleTime = 0;
unsigned long lastSendTime   = 0;

// Rough RSSI-to-distance conversion using a basic path-loss model
float rssiToDistance(int rssi) {
  return pow(10.0, (-69.0 - rssi) / 20.0);
}

// Returns the smallest distance from the current sample window (the closest we have seen)
float getMinDistance() {
  if (distCount == 0) return -1;
  float minDist = distBuffer[0];
  for (int i = 1; i < distCount; i++) {
    if (distBuffer[i] < minDist) minDist = distBuffer[i];
  }
  return minDist;
}

// Updates our flags whenever the BLE client connects or disconnects
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    trackerConnected = true;
    Serial.println("[BLE] Connected to tracker!");
  }
  void onDisconnect(NimBLEClient* c) override {
    trackerConnected = false;
    trackerFound = false;
  }
};


// Called for every BLE advertisement we hear: if it is the collar, we save its address
class PetScanCallback : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    if (dev->haveName() && dev->getName() == PET_TRACKER_NAME) {
      if (trackerAddr != nullptr) delete trackerAddr;
      trackerAddr = new NimBLEAddress(dev->getAddress());
      trackerFound = true;
      bleScan->stop();
    }
  }
};

// Opens a BLE connection to whatever tracker address we found in the last scan
bool connectToTracker() {
  if (!trackerAddr) return false;
  bleClient = NimBLEDevice::createClient();
  bleClient->setClientCallbacks(new ClientCallbacks());
  return bleClient->connect(*trackerAddr);
}


// Sets up the BLE stack and gets the scanner ready to look for the collar
void initBLE() {
  Serial.println("Initing BLE");
  NimBLEDevice::init("SmartHomeGW");
  bleScan = NimBLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new PetScanCallback());
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  Serial.println("BLE ready");
}

// Connects to the collar just long enough to send a command, then drops the link
bool connectAndSendCommand(const String& cmd) {
  Serial.printf("[BLE] Connecting to send: %s\n", cmd.c_str());

  // Quick scan to grab the collar address
  bleScan->start(2, false);
  NimBLEScanResults results = bleScan->getResults();
  NimBLEAddress* targetAddr = nullptr;
  for (int i = 0; i < results.getCount(); i++) {       // loop through everything we found
    NimBLEAdvertisedDevice d = results.getDevice(i);      // grab the i-th device
    if (d.haveName() && d.getName() == PET_TRACKER_NAME) {
      targetAddr = new BLEAddress(d.getAddress());
      break;
    }
  }
  bleScan->clearResults();

  if (!targetAddr) {
    Serial.println("[BLE] Collar not found");
    return false;
  }

  bleClient = NimBLEDevice::createClient();
  if (!bleClient->connect(*targetAddr)) {
    Serial.println("[BLE] Connect failed");
    delete targetAddr;
    return false;
  }

  NimBLERemoteService* svc = bleClient->getService(PET_SERVICE_UUID);
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

  delay(100);
  bleClient->disconnect();
  delete targetAddr;
  return true;
}

// -------------------- Cycle phases ---------------------
// Tells every node "hey, a new cycle is starting now"
void sendSync() {
  loraSend(String(NODE_ID_GW) + "|SYNC|" + BROADCAST + "|cycle_start");
}

// Sends back a quick ACK so the node knows we received its data
void sendAck(const String& to) {
  loraSend(String(NODE_ID_GW) + "|ACK|" + to + "|ok");
}

// Runs the time slot for one node: waits for its data, ACKs it, and sends pending commands
void runNodeSlot(const String& expectedSender, void (*handler)(const String&), const String& expectedReceiver, int apriTutto) {
  unsigned long slotStart = millis();
  while (millis() - slotStart < SLOT_DURATION) {
    String raw = loraReceive(ACK_TIMEOUT);
    if (raw.length() == 0) continue;
    Packet p = parsePacket(raw);
    if (!p.valid) continue;

    if (p.sender == expectedSender && p.type == "DATA" && p.receiver == expectedReceiver) {
      //Serial.println("[SLOT] MATCH! Sending ACK...");
      handler(p.payload);
      sendAck(p.sender);
      // If we have a pending command for this node, send it now that it is awake
      if (fanCmdPending && p.sender == NODE_ID_AIR) {
        fanCmdPending = false;
        if (fan == 0){
          String cmd ="fan=off";
          delay(100);
          loraSend(String(NODE_ID_GW) + "|CMD|" + NODE_ID_AIR + "|" + cmd);
        } else {
          String cmd ="fan=on";
          delay(100);
          loraSend(String(NODE_ID_GW) + "|CMD|" + NODE_ID_AIR + "|" + cmd);
        }
      }
      if (lightCmdPending && p.sender == NODE_ID_PLANT) {
        lightCmdPending = false;
        String cmd = "light=on";
        delay(100);
        loraSend(String(NODE_ID_GW) + "|CMD|" + NODE_ID_PLANT + "|" + cmd);
      }
      return;
    } else {
      Serial.println("[SLOT] NO MATCH - skipping");
    }
  }
}

// Quick check for mailbox messages, kept short so we do not block the main loop
void mailboxListening(){
String raw = loraReceive(1000); // keep this short, the main loop has other things to do
  if (raw.length() == 0) return;

  Packet p = parsePacket(raw);
  if (!p.valid) return;

  if (p.sender == NODE_ID_MAILBOX && p.type == "DATA" && p.receiver == NODE_ID_GW) {
    sendAck(p.sender);
    handleMailData(p.payload);
  }
}

// ---------------------- MQTT handling ----------------------------

// Fires whenever a command comes in from MQTT, we just stash it and send it later
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  
  Serial.printf("[MQTT] %s -> %s\n", topic, msg.c_str());
  
  String t = String(topic);
  if (t == TOPIC_CMD_FAN) {
    fan = msg.toInt();
    fanCmdPending = true;
  }
  else if (t == TOPIC_CMD_LIGHT) {
    light = msg.toInt();
    lightCmdPending = true;
  }
}

// Tries up to three times to connect to the broker and subscribes to the command topics
void mqttConnect() {
  int attempts = 0;
  while (!mqtt.connected() && attempts < 3) {
    attempts++;
    Serial.printf("[MQTT] Attempt %d to %s:%d\n", attempts, MQTT_HOST, MQTT_PORT);
    
    String clientId = "gateway-" + String(random(0xffff), HEX);
    
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("[MQTT] Connected!");
      mqtt.subscribe(TOPIC_CMD_FAN);
      mqtt.subscribe(TOPIC_CMD_LIGHT);
      mqtt.subscribe(TOPIC_CMD_PET_RECALL);
      return;
    }
    
    int state = mqtt.state();
    Serial.printf("[MQTT] Failed, state=%d\n", state);
    // -4 = timeout
    // -2 = connection attempt failed  
    // -1 = client disconnected
    //  5 = broker rejected our credentials
    
    delay(3000);
  }
}
String str;
// Resets the RN2483 and configures it for the frequency and settings our project uses
void initLoRa() {
  pinMode(RST, OUTPUT);

  loraSerial.begin(57600, SERIAL_8N1, RXD2, TXD2);
  loraSerial.setTimeout(2000);

  bool moduleReady = false;

  for (int attempt = 0; attempt < 5; attempt++) {
    Serial.printf("[LoRa] Init attempt %d...\n", attempt + 1);

    // Hardware reset of the module
    digitalWrite(RST, HIGH);
    delay(100);
    digitalWrite(RST, LOW);
    delay(500);
    digitalWrite(RST, HIGH);
    delay(2000);

    // Throw away anything left over in the serial buffer
    while (loraSerial.available()) loraSerial.read();
    delay(100);

    // The module sometimes prints a boot message, grab it if it does
    String boot = loraSerial.readStringUntil('\n');
    boot.trim();
    if (boot.length() > 0) {
      Serial.print("[BOOT] "); Serial.println(boot);
    }

    // Ask for the firmware version, this also tells us the module is alive
    while (loraSerial.available()) loraSerial.read();
    loraSerial.println("sys get ver");
    String ver = loraSerial.readStringUntil('\n');
    ver.trim();
    Serial.print("[VER] "); Serial.println(ver);

    if (ver.startsWith("RN2483")) {
      moduleReady = true;
      break;
    }

    Serial.println("[LoRa] No response, retrying...");
  }

  if (!moduleReady) {
    Serial.println("[LoRa] FAILED after 5 attempts!");
    return;
  }

  // Module is responding, push all our radio settings
  loraSerial.setTimeout(1000);

  loraSerial.println("mac pause");
  Serial.println(loraSerial.readStringUntil('\n'));

  loraSerial.println("radio set mod lora");      loraSerial.readStringUntil('\n');
  loraSerial.println("radio set freq 869100000"); loraSerial.readStringUntil('\n');
  loraSerial.println("radio set pwr 14");         loraSerial.readStringUntil('\n');
  loraSerial.println("radio set sf sf7");         loraSerial.readStringUntil('\n');
  loraSerial.println("radio set afcbw 41.7");     loraSerial.readStringUntil('\n');
  loraSerial.println("radio set rxbw 20.8");      loraSerial.readStringUntil('\n');
  loraSerial.println("radio set prlen 8");        loraSerial.readStringUntil('\n');
  loraSerial.println("radio set crc on");         loraSerial.readStringUntil('\n');
  loraSerial.println("radio set iqi off");        loraSerial.readStringUntil('\n');
  loraSerial.println("radio set cr 4/5");         loraSerial.readStringUntil('\n');
  loraSerial.println("radio set wdt 60000");      loraSerial.readStringUntil('\n');
  loraSerial.println("radio set sync 12");        loraSerial.readStringUntil('\n');
  loraSerial.println("radio set bw 125");         loraSerial.readStringUntil('\n');

  Serial.println("[LoRa] Ready");
}

// ------------------- Setup and main loop ---------------------------
// Boots everything up: LoRa first, then WiFi/MQTT, and BLE last
void setup() {
  pinMode(RST, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(57600);
  delay(500);

  Serial.println("Starting the gateway...");
  delay(1500);
  initLoRa();
  // Heads up: do NOT start BLE here, it can mess with the TLS handshake

  Serial.print("Connecting WiFi");
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(8,8,8,8));
  WiFi.begin(ssid, password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK");
    Serial.println(WiFi.localIP());

    espClient.setInsecure();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(512);
    mqtt.setKeepAlive(60);   // long keepalive so we survive the slower LoRa cycles
    mqttConnect();
  }

  // Now it is safe to start BLE, after TLS is already up
  initBLE();
  led_blink(3);

  lastCycleStart = millis() - CYCLE_PERIOD;
}

// The big main loop: keeps MQTT alive, watches the collar, and runs the LoRa cycle on time
void loop() {
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (WiFi.status() == WL_CONNECTED) {
    mqttConnect();
  }

  unsigned long now = millis();

  bool inCycle = (now - lastCycleStart < 8000); // roughly two LoRa slots, plus a little buffer

  // --- BLE: keep an eye on the collar between cycles ---
  if (!inCycle) {
    // Scan for the tracker every 5 seconds until we find it
    if (!trackerConnected && now - lastScanTime >= 5000) {
      lastScanTime = now;
      bleScan->start(1, false);
      bleScan->clearResults();
    }

    // Do not hammer the connect call, give it some breathing room
    static unsigned long lastConnectAttempt = 0;
    if (trackerFound && !trackerConnected && now - lastConnectAttempt > 5000) {
      lastConnectAttempt = now;
      connectToTracker();
    }

    // Once we are connected, grab one distance sample every few seconds
    if (trackerConnected && now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
      int rssi = bleClient->getRssi();
      float dist = rssiToDistance(rssi);

      if (distCount < RSSI_SAMPLES) {
        distBuffer[distCount++] = dist;
      }
      lastSampleTime = now;
    }

    // Every so often, publish the closest distance from the last window
    if (trackerConnected && now - lastSendTime >= SEND_INTERVAL_MS) {
      float minDist = getMinDistance();
      if (minDist >= 0) {
        Serial.printf("[GATEWAY] Sending min distance: %.2f m\n", minDist);
        
        // Turn the distance into a simple zone for the dashboard.
        // We assume around -69 dBm at 1 metre.
        // zone 0: safe (under 10 m)
        // zone 1: warning (roughly 10-20 m)
        // zone 2: alarm (over 20 m)
        int zone = 0;
        if (minDist > 20.0) zone = 2;  // alarm
        else if (minDist > 10.0) zone = 1;  // warning
        
        String petMsg = "distance=" + String(minDist, 2) + ";zone=" + String(zone);
        mqtt.publish(TOPIC_PET_STATUS, petMsg.c_str(),true);
      }
      // Reset the window and start collecting samples again
      distCount    = 0;
      lastSendTime = now;
    }
  }

  delay(500);
  
  if (now - lastCycleStart >= CYCLE_PERIOD) {
    lastCycleStart = now;
    Serial.println("\n===== NEW CYCLE =====");
    led_on();

    // 1. Broadcast SYNC so every node knows a new cycle just started
    sendSync();

    // 2. Plant node slot
    runNodeSlot(NODE_ID_PLANT, handlePlantData, NODE_ID_GW, light);

    // 3. Air node slot
    runNodeSlot(NODE_ID_AIR, handleAirData, NODE_ID_GW, fan);

    led_off();
    Serial.println("===== CYCLE END =====");
  }

  // The mailbox node is not in the fixed cycle, so we just listen for it on the side
  mailboxListening();
}
