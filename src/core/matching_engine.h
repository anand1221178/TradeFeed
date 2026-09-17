#pragma once

#include "order_book.h"
#include "../transport/spsc_ring.h"
#include "../codec/sbe_messages.h"
#include <atomic>

using InboundRing  = SPSCRing<InboundMessage,  RING_SIZE>;
using OutboundRing = SPSCRing<OutboundMessage, RING_SIZE>;

class MatchingEngine {
    OrderBook   book_;
    InboundRing&  inbound_;
    OutboundRing& outbound_;
    OrderId   next_id_      = 1;
    uint64_t  match_number_ = 1;

    void emit_accept(ClientId c, OrderId id, Timestamp tsc);
    void emit_reject(ClientId c, OrderId id, uint8_t reason, Timestamp tsc);
    void emit_cancel(ClientId c, OrderId id, Timestamp tsc);
    void emit_fills(Timestamp tsc);

public:
    MatchingEngine(InboundRing& in, OutboundRing& out)
        : inbound_(in), outbound_(out) {}

    void run(std::atomic<bool>& running);
    void process(const InboundMessage& msg);

    const OrderBook& book() const { return book_; }
};
