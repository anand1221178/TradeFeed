#include "matching_engine.h"

void MatchingEngine::run(std::atomic<bool>& running) {
    InboundMessage msg;
    while (running.load(std::memory_order_relaxed)) {
        if (inbound_.pop(msg))
            process(msg);
    }
}

void MatchingEngine::process(const InboundMessage& msg) {
    Timestamp tsc = rdtsc();

    switch (msg.type) {
    case MessageType::NewOrder: {
        OrderId id = next_id_++;
        book_.clear_fills();

        if (book_.add_order(id, msg.client_id, msg.side, msg.order_type,
                            msg.price, msg.quantity, msg.recv_tsc)) {
            emit_accept(msg.client_id, id, tsc);
            emit_fills(tsc);
        } else {
            emit_reject(msg.client_id, id, 1, tsc);
        }
        break;
    }
    case MessageType::CancelOrder: {
        if (book_.cancel_order(msg.order_id))
            emit_cancel(msg.client_id, msg.order_id, tsc);
        else
            emit_reject(msg.client_id, msg.order_id, 2, tsc);
        break;
    }
    case MessageType::ModifyOrder: {
        book_.clear_fills();
        if (book_.modify_order(msg.order_id, msg.price, msg.quantity, msg.recv_tsc)) {
            emit_accept(msg.client_id, msg.order_id, tsc);
            emit_fills(tsc);
        } else {
            emit_reject(msg.client_id, msg.order_id, 3, tsc);
        }
        break;
    }
    default:
        break;
    }
}

void MatchingEngine::emit_accept(ClientId c, OrderId id, Timestamp tsc) {
    OutboundMessage m{};
    m.type       = MessageType::OrderAccepted;
    m.client_id  = c;
    m.order_id   = id;
    m.engine_tsc = tsc;
    outbound_.push(m);
}

void MatchingEngine::emit_reject(ClientId c, OrderId id, uint8_t reason, Timestamp tsc) {
    OutboundMessage m{};
    m.type          = MessageType::OrderRejected;
    m.client_id     = c;
    m.order_id      = id;
    m.reject_reason = reason;
    m.engine_tsc    = tsc;
    outbound_.push(m);
}

void MatchingEngine::emit_cancel(ClientId c, OrderId id, Timestamp tsc) {
    OutboundMessage m{};
    m.type       = MessageType::OrderCanceled;
    m.client_id  = c;
    m.order_id   = id;
    m.engine_tsc = tsc;
    outbound_.push(m);
}

void MatchingEngine::emit_fills(Timestamp tsc) {
    for (const auto& f : book_.fills()) {
        OutboundMessage m{};
        m.type         = MessageType::OrderExecuted;
        m.price        = f.price;
        m.quantity     = f.quantity;
        m.match_number = match_number_++;
        m.engine_tsc   = tsc;

        m.order_id  = f.bid_id;
        m.client_id = f.bid_client;
        outbound_.push(m);

        m.order_id  = f.ask_id;
        m.client_id = f.ask_client;
        outbound_.push(m);
    }
}
