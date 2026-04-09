#pragma once
#define LORA_FREQUENCY     868E6
#define LORA_BANDWIDTH     125E3
#define LORA_SF            7
#define LORA_TX_POWER      14

#define NODE_GATEWAY       0x00
#define NODE_PET_TRACKER   0x01
#define NODE_AQUARIUM      0x02
#define NODE_PLANT         0x03
#define NODE_AIR           0x04
#define NODE_MAILBOX       0x05

#define SLOT_DURATION_MS   2000
#define EVENT_WINDOW_MS    3000
#define CYCLE_TOTAL_MS     (6 * SLOT_DURATION_MS + EVENT_WINDOW_MS)

#define MSG_DATA           0x01
#define MSG_ACK            0x02
#define MSG_ALERT          0x03
#define MSG_DOWNLINK       0x04
#define MSG_SYNC_BEACON    0x05

typedef struct {
    uint8_t  node_id;
    uint8_t  msg_type;
    uint16_t sequence;
    uint8_t  payload[48];
    uint8_t  payload_len;
} LoRaPacket;
