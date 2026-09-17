#pragma once

#include "../core/types.h"

// Internal ring buffer messages: cache-line aligned for SPSC ring.
// Gateway translates between wire format and these.

struct alignas(CACHE_LINE) InboundMessage {
    MessageType type;
    Side        side;
    OrderType   order_type;
    uint8_t     pad_;
    ClientId    client_id;
    OrderId     order_id;
    Price       price;
    Quantity    quantity;
    Timestamp   recv_tsc;
};

struct alignas(CACHE_LINE) OutboundMessage {
    MessageType type;
    Side        aggressor_side;
    uint8_t     reject_reason;
    uint8_t     pad_;
    ClientId    client_id;
    OrderId     order_id;
    Price       price;
    Quantity    quantity;
    OrderId     match_number;
    Timestamp   engine_tsc;
};

// Wire format: packed structs for TCP. Fixed 32 bytes per message
// so the gateway reads/writes exact frames with no length prefix.
#pragma pack(push, 1)

struct WireMessage {
    uint8_t  type;
    uint8_t  side;
    uint8_t  order_type;
    uint8_t  pad;
    uint32_t client_id;
    uint64_t order_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t timestamp;
};
static_assert(sizeof(WireMessage) == 32, "wire message must be exactly 32 bytes");

#pragma pack(pop)
