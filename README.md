# ReallySmartHouse

ReallySmartHouse is a multi-node IoT smart-home system built with PlatformIO, the Arduino framework, ESP32 boards, and GitHub. Each node is responsible for one area of the home — sensing, detecting, or actuating — and the gateway ties everything together, forwarding data to the cloud and relaying commands back down.

**Live dashboard:** [ReallySmartHouse Dashboard](https://jackvisi.github.io/reallysmarthousedash/)

## Repository Structure

Each folder is an independent PlatformIO project with its own `platformio.ini`.

| Folder | Purpose | Main tech |
|---|---|---|
| `gateway` | Central coordinator: receives LoRa packets from all nodes, tracks the pet collar over BLE, and forwards everything to HiveMQ via MQTT over Wi-Fi. | ESP32, LoRa P2P, BLE, Wi-Fi, MQTT |
| `pet-tracker` | Wearable collar node. Advertises over BLE while near the gateway. Falls back to LoRaWAN via Cibicom when BLE is unavailable. | ESP32, BLE, RN2483, LoRaWAN |
| `plant-node` | Monitors temperature, humidity, and soil moisture. Controls a grow light. Uses deep sleep between cycles. | ESP32, LoRa, DHT |
| `air-node` | Room climate monitoring with fan and DC motor actuator. | ESP32, LoRa, DHT, DC motor |
| `mailbox` | Event-driven node that wakes on mail delivery, checks battery level, and sends an alert. | ESP32, LoRa, ADC |
| `shared` | Common protocol constants and headers used across nodes. | C/C++ headers |

### Data flow

1. Each node measures or detects something and sends a LoRa P2P packet to the gateway.
2. The gateway publishes the payload to HiveMQ Cloud over MQTT/TLS.
3. The dashboard subscribes to the relevant topics and displays live values.
4. Commands from the dashboard travel back through MQTT to the gateway, which relays them to the target node over LoRa.

### Pet tracker flow

The pet tracker operates in two modes depending on proximity to the gateway:

- **BLE mode (normal):** The collar advertises as `PetTracker-01`. The gateway scans periodically, connects, reads RSSI, and estimates distance. This is the primary channel.
- **LoRaWAN fallback:** If the gateway has not connected over BLE within the grace period, the collar joins the Cibicom LoRaWAN network via OTAA and transmits `PET_MISSING`. A Node.js bridge running on Railway receives the Cibicom uplink, decodes the hex payload, selects the antenna with the best RSSI, and publishes a JSON event to HiveMQ. When BLE is restored, the collar sends `PET_RETURNED` before switching back.

### LoRa P2P protocol

Nodes and the gateway exchange fixed-format text packets over a shared radio channel:

```
<sender>|<type>|<receiver>|<payload>
```

The gateway initiates each 10-minute cycle with a `SYNC` broadcast. Each node has a reserved slot in which it sends a `DATA` packet; the gateway acknowledges with `ACK`. Commands are sent as `CMD` packets after the data exchange. The mailbox operates outside the cycle and wakes autonomously on an interrupt.

## Requirements

- Visual Studio Code with the PlatformIO IDE extension
- Git
- An ESP32-based board for each node
- The sensors and radio modules required by the target node (see the node folder for details)

## Getting Started

Clone the repository:

```bash
git clone https://github.com/JAVIERTEL/ReallySmartHouse.git
cd ReallySmartHouse
```

Open **only the node folder** you want to build in VS Code — do not open the repository root as a PlatformIO project. PlatformIO will detect the project through the `platformio.ini` inside the node folder.

```
File → Open Folder → ReallySmartHouse/gateway
```

Build and upload as usual from the PlatformIO toolbar.

## Web Dashboard

The dashboard is hosted at [jackvisi.github.io/reallysmarthousedash](https://jackvisi.github.io/reallysmarthousedash/). It connects directly to HiveMQ Cloud using MQTT over WebSockets and requires no backend of its own.

- Sensor values update in real time as the gateway publishes new data.
- Command buttons publish to the relevant MQTT topic; the gateway picks them up and forwards them to the correct node over LoRa.

## MQTT Topics

| Topic | Direction | Content |
|---|---|---|
| `home/plant/data` | gateway → dashboard | Temperature, humidity, soil moisture |
| `home/air/data` | gateway → dashboard | Temperature, humidity |
| `home/mail/data` | gateway → dashboard | Mail count |
| `home/pet/status` | gateway → dashboard | BLE distance and zone |
| `iot/group7/lorawan/status` | Railway bridge → dashboard | LoRaWAN pet alarm (JSON) |
| `home/cmd/fan` | dashboard → gateway | Fan on/off |
| `home/cmd/light` | dashboard → gateway | Grow light on/off |
| `home/cmd/pet_recall` | dashboard → gateway | Trigger collar recall sound |
