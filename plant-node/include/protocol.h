#pragma once

// Protocol identifiers
#define NODE_ID       "01"
#define GATEWAY_ID    "00"
#define BROADCAST_ID  "FF"

// Protocol timeouts in milliseconds
#define SYNC_TIMEOUT_MS   60000
#define ACK_TIMEOUT_MS    3000
#define CMD_TIMEOUT_MS    5000

// Retransmission
#define MAX_RETRY_COUNT   3