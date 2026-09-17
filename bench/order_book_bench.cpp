#include "../src/core/order_book.h"
#include "../src/core/matching_engine.h"
#include "../src/codec/sbe_messages.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>
#include <chrono>

struct LatencyStats {
    std::vector<uint64_t> samples;

    void record(uint64_t ticks) { samples.push_back(ticks); }

    void print(const char* label, double ticks_per_ns) {
        if (samples.empty()) return;
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();
        auto ns = [&](size_t idx) { return samples[idx] / ticks_per_ns; };

        std::printf("  %-20s  n=%-8zu  p50=%6.0fns  p90=%6.0fns  p99=%6.0fns  p99.9=%6.0fns  min=%6.0fns  max=%6.0fns\n",
                    label, n,
                    ns(n * 50 / 100), ns(n * 90 / 100), ns(n * 99 / 100),
                    ns(n * 999 / 1000), ns(0), ns(n - 1));
    }
};

static double calibrate_tsc() {
    auto t0_wall = std::chrono::steady_clock::now();
    uint64_t t0_tsc = rdtsc();
    volatile int sink = 0;
    for (int i = 0; i < 100'000'000; ++i) sink += i;
    (void)sink;
    uint64_t t1_tsc = rdtsc();
    auto t1_wall = std::chrono::steady_clock::now();

    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1_wall - t0_wall).count();
    return (t1_tsc - t0_tsc) / wall_ns;
}

static void bench_add_order() {
    std::printf("\n=== Add Order (no matching) ===\n");
    OrderBook book(MAX_ORDERS);
    LatencyStats stats;
    double tpn = calibrate_tsc();

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(100'000, 200'000);

    for (OrderId i = 1; i <= 500'000; ++i) {
        Price p = price_dist(rng);
        Side s  = (i % 2 == 0) ? Side::Buy : Side::Sell;
        // Ensure no crossing: bids at low prices, asks at high prices
        if (s == Side::Buy)  p = price_dist(rng) - 50'000;
        else                 p = price_dist(rng) + 50'000;

        uint64_t t0 = rdtsc();
        book.add_order(i, 1, s, OrderType::Limit, p, 100, t0);
        uint64_t t1 = rdtsc();
        stats.record(t1 - t0);
    }
    stats.print("add_order", tpn);
}

static void bench_cancel_order() {
    std::printf("\n=== Cancel Order ===\n");
    OrderBook book(MAX_ORDERS);
    LatencyStats stats;
    double tpn = calibrate_tsc();

    // Pre-populate
    for (OrderId i = 1; i <= 200'000; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        Price p = s == Side::Buy ? 90'000 + (i % 1000) : 110'000 + (i % 1000);
        book.add_order(i, 1, s, OrderType::Limit, p, 100, 0);
    }

    // Cancel random subset
    std::mt19937 rng(123);
    std::vector<OrderId> ids(200'000);
    std::iota(ids.begin(), ids.end(), 1);
    std::shuffle(ids.begin(), ids.end(), rng);

    for (size_t i = 0; i < 100'000; ++i) {
        uint64_t t0 = rdtsc();
        book.cancel_order(ids[i]);
        uint64_t t1 = rdtsc();
        stats.record(t1 - t0);
    }
    stats.print("cancel_order", tpn);
}

static void bench_matching() {
    std::printf("\n=== Matching (aggressive orders cross the spread) ===\n");
    OrderBook book(MAX_ORDERS);
    LatencyStats stats;
    double tpn = calibrate_tsc();

    // Seed the book with resting ask orders
    for (OrderId i = 1; i <= 100'000; ++i) {
        Price p = 100'000 + (i % 100);
        book.add_order(i, 1, Side::Sell, OrderType::Limit, p, 10, 0);
    }

    // Send aggressive buy orders that cross the spread
    for (OrderId i = 100'001; i <= 200'000; ++i) {
        book.clear_fills();
        uint64_t t0 = rdtsc();
        book.add_order(i, 2, Side::Buy, OrderType::Limit, 100'100, 10, t0);
        uint64_t t1 = rdtsc();
        stats.record(t1 - t0);
    }
    stats.print("match_order", tpn);
    std::printf("  Total matches: %llu\n",
                static_cast<unsigned long long>(book.match_count()));
}

static void bench_mixed_workload() {
    std::printf("\n=== Mixed Workload (70%% add, 20%% cancel, 10%% match) ===\n");
    OrderBook book(MAX_ORDERS);
    LatencyStats add_stats, cancel_stats, match_stats;
    double tpn = calibrate_tsc();

    std::mt19937 rng(999);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<Price> bid_dist(95'000, 99'999);
    std::uniform_int_distribution<Price> ask_dist(100'001, 105'000);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    OrderId next_id = 1;
    std::vector<OrderId> active_bids, active_asks;
    active_bids.reserve(100'000);
    active_asks.reserve(100'000);

    for (int i = 0; i < 1'000'000; ++i) {
        int action = action_dist(rng);
        book.clear_fills();

        if (action < 70) {
            // Add a resting order (no crossing)
            Side s = (next_id % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = (s == Side::Buy) ? bid_dist(rng) : ask_dist(rng);
            Quantity q = qty_dist(rng);

            uint64_t t0 = rdtsc();
            book.add_order(next_id, 1, s, OrderType::Limit, p, q, t0);
            uint64_t t1 = rdtsc();
            add_stats.record(t1 - t0);

            if (s == Side::Buy)  active_bids.push_back(next_id);
            else                 active_asks.push_back(next_id);
            ++next_id;

        } else if (action < 90 && (!active_bids.empty() || !active_asks.empty())) {
            // Cancel a random order
            auto& pool = (rng() % 2 == 0 && !active_bids.empty()) ? active_bids : active_asks;
            if (pool.empty()) continue;
            size_t idx = rng() % pool.size();
            OrderId cid = pool[idx];
            pool[idx] = pool.back();
            pool.pop_back();

            uint64_t t0 = rdtsc();
            book.cancel_order(cid);
            uint64_t t1 = rdtsc();
            cancel_stats.record(t1 - t0);

        } else {
            // Aggressive order that crosses the spread
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = (s == Side::Buy) ? 105'000 : 95'000;
            Quantity q = qty_dist(rng);

            uint64_t t0 = rdtsc();
            book.add_order(next_id++, 2, s, OrderType::Limit, p, q, t0);
            uint64_t t1 = rdtsc();
            match_stats.record(t1 - t0);
        }
    }

    add_stats.print("add (resting)", tpn);
    cancel_stats.print("cancel", tpn);
    match_stats.print("match (aggressive)", tpn);
    std::printf("  Total matches: %llu\n",
                static_cast<unsigned long long>(book.match_count()));
}

static void bench_engine_throughput() {
    std::printf("\n=== Engine Process Throughput ===\n");
    static InboundRing in;
    static OutboundRing out;
    MatchingEngine engine(in, out);
    double tpn = calibrate_tsc();

    // Pre-fill inbound ring with new orders
    constexpr int N = 200'000;
    for (int i = 0; i < N; ++i) {
        InboundMessage msg{};
        msg.type       = MessageType::NewOrder;
        msg.side       = (i % 2 == 0) ? Side::Buy : Side::Sell;
        msg.order_type = OrderType::Limit;
        msg.client_id  = 1;
        msg.price      = (msg.side == Side::Buy) ? 95'000 + (i % 100) : 105'000 + (i % 100);
        msg.quantity   = 10;
        msg.recv_tsc   = rdtsc();
        in.push(msg);
    }

    uint64_t t0 = rdtsc();
    InboundMessage msg;
    while (in.pop(msg))
        engine.process(msg);
    uint64_t t1 = rdtsc();

    double total_ns = (t1 - t0) / tpn;
    std::printf("  Processed %d orders in %.0f ns\n", N, total_ns);
    std::printf("  Throughput: %.0f orders/sec\n", N / (total_ns / 1e9));
    std::printf("  Avg latency: %.0f ns/order\n", total_ns / N);

    // Drain outbound
    OutboundMessage outmsg;
    int out_count = 0;
    while (out.pop(outmsg)) ++out_count;
    std::printf("  Outbound messages: %d\n", out_count);
}

int main() {
    std::printf("TradeFeed Order Book Benchmarks\n");
    std::printf("================================\n");
    std::printf("Calibrating TSC...\n");

    bench_add_order();
    bench_cancel_order();
    bench_matching();
    bench_mixed_workload();
    bench_engine_throughput();

    std::printf("\nDone.\n");
    return 0;
}
