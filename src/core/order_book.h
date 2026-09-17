#pragma once

#include "types.h"
#include "order_pool.h"
#include "price_level.h"
#include <vector>
#include <memory>

struct Fill {
    OrderId  bid_id;
    OrderId  ask_id;
    ClientId bid_client;
    ClientId ask_client;
    Price    price;
    Quantity quantity;
};

class OrderBook {
    // Heap-allocated: bids_ + asks_ + order_table_ = ~56MB.
    // Cannot live on the stack (default 8MB limit).
    std::unique_ptr<PriceLevel[]> bids_;
    std::unique_ptr<PriceLevel[]> asks_;
    std::unique_ptr<Order*[]>     order_table_;

    OrderPool pool_;

    Price best_bid_ = INVALID_PRICE;
    Price best_ask_ = MAX_PRICE + 1;

    uint64_t match_count_ = 0;
    std::vector<Fill> fills_;

    // Instrumentation for the best-price scan (Chapter 13.7's known weakness).
    uint64_t scan_calls_ = 0;
    uint64_t scan_steps_ = 0;
    uint64_t scan_worst_ = 0;

    size_t idx(Price p) const { return p - MIN_PRICE; }

    Order* lookup(OrderId id) const {
        Order* o = order_table_[id & ORDER_MASK];
        return (o && o->id == id) ? o : nullptr;
    }
    void reg(Order* o)   { order_table_[o->id & ORDER_MASK] = o; }
    void unreg(Order* o) { order_table_[o->id & ORDER_MASK] = nullptr; }

    void match_limit(Order* aggressor);
    void match_market(Order* aggressor);
    void scan_best_bid();
    void scan_best_ask();
    void drain_level(PriceLevel& level, Order* aggressor, Side aggressor_side);

public:
    explicit OrderBook(size_t pool_capacity = MAX_ORDERS);

    bool add_order(OrderId id, ClientId client, Side side, OrderType type,
                   Price price, Quantity qty, Timestamp ts);
    bool cancel_order(OrderId id);
    bool modify_order(OrderId id, Price new_price, Quantity new_qty, Timestamp ts);

    Price    best_bid()       const { return best_bid_; }
    Price    best_ask()       const { return best_ask_; }
    Quantity bid_qty_at(Price p) const { return bids_[idx(p)].total_qty(); }
    Quantity ask_qty_at(Price p) const { return asks_[idx(p)].total_qty(); }

    const std::vector<Fill>& fills() const { return fills_; }
    void clear_fills() { fills_.clear(); }
    uint64_t match_count() const { return match_count_; }
    size_t active_orders() const { return pool_.capacity() - pool_.available(); }

    uint64_t scan_calls() const { return scan_calls_; }
    uint64_t scan_steps() const { return scan_steps_; }
    uint64_t scan_worst() const { return scan_worst_; }
};
