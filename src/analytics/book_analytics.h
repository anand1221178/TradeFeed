#pragma once

#include "../core/types.h"
#include "../core/order_book.h"
#include <cmath>
#include <array>

class BookAnalytics {
    uint64_t total_volume_ = 0;
    double   vwap_numer_   = 0.0;

    // VPIN (volume-synchronized probability of informed trading)
    static constexpr size_t VPIN_BUCKETS    = 50;
    static constexpr Quantity BUCKET_VOLUME = 10'000;
    Quantity current_buy_vol_  = 0;
    Quantity current_sell_vol_ = 0;
    Quantity current_bucket_vol_ = 0;
    std::array<double, VPIN_BUCKETS> vpin_history_{};
    size_t vpin_idx_ = 0;
    size_t vpin_count_ = 0;

public:
    void on_fill(Price price, Quantity qty, Side aggressor_side) {
        total_volume_ += qty;
        vwap_numer_ += static_cast<double>(price) * qty;

        if (aggressor_side == Side::Buy)
            current_buy_vol_ += qty;
        else
            current_sell_vol_ += qty;
        current_bucket_vol_ += qty;

        if (current_bucket_vol_ >= BUCKET_VOLUME) {
            Quantity total = current_buy_vol_ + current_sell_vol_;
            double imbalance = total > 0
                ? std::abs(static_cast<double>(current_buy_vol_) - current_sell_vol_) / total
                : 0.0;
            vpin_history_[vpin_idx_ % VPIN_BUCKETS] = imbalance;
            ++vpin_idx_;
            if (vpin_count_ < VPIN_BUCKETS) ++vpin_count_;
            current_buy_vol_ = current_sell_vol_ = current_bucket_vol_ = 0;
        }
    }

    double vwap() const {
        return total_volume_ > 0 ? vwap_numer_ / total_volume_ : 0.0;
    }

    uint64_t volume() const { return total_volume_; }

    double vpin() const {
        if (vpin_count_ == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < vpin_count_; ++i)
            sum += vpin_history_[i];
        return sum / vpin_count_;
    }

    static Price spread(const OrderBook& book) {
        if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE)
            return 0;
        return book.best_ask() - book.best_bid();
    }

    static double mid(const OrderBook& book) {
        if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE)
            return 0.0;
        return (book.best_bid() + book.best_ask()) / 2.0;
    }

    static double imbalance(const OrderBook& book) {
        if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE)
            return 0.0;
        Quantity bq = book.bid_qty_at(book.best_bid());
        Quantity aq = book.ask_qty_at(book.best_ask());
        Quantity total = bq + aq;
        return total > 0 ? static_cast<double>(static_cast<int64_t>(bq) - aq) / total : 0.0;
    }

    // Depth: sum of quantity across N best price levels
    static Quantity bid_depth(const OrderBook& book, int levels) {
        Quantity total = 0;
        Price p = book.best_bid();
        for (int i = 0; i < levels && p >= MIN_PRICE; ++i, --p) {
            total += book.bid_qty_at(p);
            if (p == MIN_PRICE) break;
        }
        return total;
    }

    static Quantity ask_depth(const OrderBook& book, int levels) {
        Quantity total = 0;
        Price p = book.best_ask();
        for (int i = 0; i < levels && p <= MAX_PRICE; ++i, ++p)
            total += book.ask_qty_at(p);
        return total;
    }
};
