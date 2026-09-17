#include "order_book.h"
#include <algorithm>

OrderBook::OrderBook(size_t pool_capacity)
    : bids_(new PriceLevel[PRICE_LEVELS]())
    , asks_(new PriceLevel[PRICE_LEVELS]())
    , order_table_(new Order*[MAX_ORDERS]())
    , pool_(pool_capacity) {
    fills_.reserve(64);
}

bool OrderBook::add_order(OrderId id, ClientId client, Side side, OrderType type,
                          Price price, Quantity qty, Timestamp ts) {
    if (pool_.available() == 0) return false;
    if (type == OrderType::Limit && (price < MIN_PRICE || price > MAX_PRICE)) return false;

    Order* o    = pool_.allocate();
    o->id         = id;
    o->client_id  = client;
    o->price      = price;
    o->quantity   = qty;
    o->filled_qty = 0;
    o->side       = side;
    o->type       = type;
    o->timestamp  = ts;
    o->prev       = nullptr;
    o->next       = nullptr;

    reg(o);

    if (type == OrderType::Market)
        match_market(o);
    else
        match_limit(o);

    if (!o->is_filled() && type == OrderType::Limit) {
        if (side == Side::Buy) {
            bids_[idx(price)].push(o);
            if (price > best_bid_) best_bid_ = price;
        } else {
            asks_[idx(price)].push(o);
            if (price < best_ask_) best_ask_ = price;
        }
    } else {
        unreg(o);
        pool_.deallocate(o);
    }

    return true;
}

bool OrderBook::cancel_order(OrderId id) {
    Order* o = lookup(id);
    if (!o) return false;

    Price p = o->price;
    if (o->side == Side::Buy) {
        bids_[idx(p)].remove(o);
        if (bids_[idx(p)].empty() && p == best_bid_) scan_best_bid();
    } else {
        asks_[idx(p)].remove(o);
        if (asks_[idx(p)].empty() && p == best_ask_) scan_best_ask();
    }

    unreg(o);
    pool_.deallocate(o);
    return true;
}

bool OrderBook::modify_order(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
    Order* o = lookup(id);
    if (!o) return false;

    Side     side   = o->side;
    ClientId client = o->client_id;
    cancel_order(id);
    return add_order(id, client, side, OrderType::Limit, new_price, new_qty, ts);
}

void OrderBook::drain_level(PriceLevel& level, Order* aggressor, Side aggressor_side) {
    while (!level.empty() && !aggressor->is_filled()) {
        Order* resting  = level.front();
        Quantity fill_q = std::min(aggressor->remaining(), resting->remaining());

        aggressor->fill(fill_q);
        resting->fill(fill_q);
        level.adjust_qty(-static_cast<int32_t>(fill_q));

        if (aggressor_side == Side::Buy)
            fills_.push_back({aggressor->id, resting->id,
                              aggressor->client_id, resting->client_id,
                              resting->price, fill_q});
        else
            fills_.push_back({resting->id, aggressor->id,
                              resting->client_id, aggressor->client_id,
                              resting->price, fill_q});
        ++match_count_;

        if (resting->is_filled()) {
            level.remove(resting);
            unreg(resting);
            pool_.deallocate(resting);
        }
    }
}

void OrderBook::match_limit(Order* aggressor) {
    if (aggressor->side == Side::Buy) {
        while (!aggressor->is_filled() && best_ask_ <= aggressor->price && best_ask_ <= MAX_PRICE) {
            drain_level(asks_[idx(best_ask_)], aggressor, Side::Buy);
            if (asks_[idx(best_ask_)].empty()) scan_best_ask();
        }
    } else {
        while (!aggressor->is_filled() && best_bid_ >= aggressor->price && best_bid_ != INVALID_PRICE) {
            drain_level(bids_[idx(best_bid_)], aggressor, Side::Sell);
            if (bids_[idx(best_bid_)].empty()) scan_best_bid();
        }
    }
}

void OrderBook::match_market(Order* aggressor) {
    if (aggressor->side == Side::Buy) {
        while (!aggressor->is_filled() && best_ask_ <= MAX_PRICE) {
            drain_level(asks_[idx(best_ask_)], aggressor, Side::Buy);
            if (asks_[idx(best_ask_)].empty()) scan_best_ask();
        }
    } else {
        while (!aggressor->is_filled() && best_bid_ != INVALID_PRICE) {
            drain_level(bids_[idx(best_bid_)], aggressor, Side::Sell);
            if (bids_[idx(best_bid_)].empty()) scan_best_bid();
        }
    }
}

// ponytail: linear scan from last best. Typically 1-3 ticks in normal markets.
// Upgrade to hierarchical bitset (__builtin_clzll) if profiling shows this matters.
void OrderBook::scan_best_bid() {
    for (Price p = best_bid_; p >= MIN_PRICE; --p) {
        if (!bids_[idx(p)].empty()) { best_bid_ = p; return; }
        if (p == MIN_PRICE) break;
    }
    best_bid_ = INVALID_PRICE;
}

void OrderBook::scan_best_ask() {
    for (Price p = best_ask_; p <= MAX_PRICE; ++p) {
        if (!asks_[idx(p)].empty()) { best_ask_ = p; return; }
    }
    best_ask_ = MAX_PRICE + 1;
}
