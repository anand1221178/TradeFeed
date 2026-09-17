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
            PriceLevel& lvl = bids_[idx(price)];
            if (lvl.empty()) bid_bits_.set(idx(price));
            lvl.push(o);
            if (price > best_bid_) best_bid_ = price;
        } else {
            PriceLevel& lvl = asks_[idx(price)];
            if (lvl.empty()) ask_bits_.set(idx(price));
            lvl.push(o);
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
        if (bids_[idx(p)].empty()) {
            bid_bits_.clear(idx(p));
            if (p == best_bid_) scan_best_bid();
        }
    } else {
        asks_[idx(p)].remove(o);
        if (asks_[idx(p)].empty()) {
            ask_bits_.clear(idx(p));
            if (p == best_ask_) scan_best_ask();
        }
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
            if (asks_[idx(best_ask_)].empty()) {
                ask_bits_.clear(idx(best_ask_));
                scan_best_ask();
            }
        }
    } else {
        while (!aggressor->is_filled() && best_bid_ >= aggressor->price && best_bid_ != INVALID_PRICE) {
            drain_level(bids_[idx(best_bid_)], aggressor, Side::Sell);
            if (bids_[idx(best_bid_)].empty()) {
                bid_bits_.clear(idx(best_bid_));
                scan_best_bid();
            }
        }
    }
}

void OrderBook::match_market(Order* aggressor) {
    if (aggressor->side == Side::Buy) {
        while (!aggressor->is_filled() && best_ask_ <= MAX_PRICE) {
            drain_level(asks_[idx(best_ask_)], aggressor, Side::Buy);
            if (asks_[idx(best_ask_)].empty()) {
                ask_bits_.clear(idx(best_ask_));
                scan_best_ask();
            }
        }
    } else {
        while (!aggressor->is_filled() && best_bid_ != INVALID_PRICE) {
            drain_level(bids_[idx(best_bid_)], aggressor, Side::Sell);
            if (bids_[idx(best_bid_)].empty()) {
                bid_bits_.clear(idx(best_bid_));
                scan_best_bid();
            }
        }
    }
}

// ponytail: linear scan from last best. Typically 1-3 ticks in normal markets.
// Upgrade to hierarchical bitset (__builtin_clzll) if profiling shows this matters.
bool OrderBook::validate_bbo() const {
    Price bb = INVALID_PRICE;
    for (Price p = MAX_PRICE; p >= MIN_PRICE; --p) {
        if (!bids_[idx(p)].empty()) { bb = p; break; }
        if (p == MIN_PRICE) break;
    }
    Price ba = MAX_PRICE + 1;
    for (Price p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        if (!asks_[idx(p)].empty()) { ba = p; break; }
    }
    return bb == best_bid_ && ba == best_ask_;
}

// O(1): three count-leading/trailing-zero lookups through the tiered bitset,
// independent of how far the next occupied level is.
void OrderBook::scan_best_bid() {
    ++scan_calls_;
    const size_t i = bid_bits_.highest();
    best_bid_ = (i == PriceBitset::NONE)
              ? INVALID_PRICE
              : static_cast<Price>(i + MIN_PRICE);
}

void OrderBook::scan_best_ask() {
    ++scan_calls_;
    const size_t i = ask_bits_.lowest();
    best_ask_ = (i == PriceBitset::NONE)
              ? MAX_PRICE + 1
              : static_cast<Price>(i + MIN_PRICE);
}
