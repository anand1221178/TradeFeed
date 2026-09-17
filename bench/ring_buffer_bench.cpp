#include "../src/transport/spsc_ring.h"
#include "../src/core/types.h"

#include <cstdio>
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <chrono>

struct alignas(CACHE_LINE) TestMsg {
    uint64_t sequence;
    uint64_t tsc;
    char payload[48];
};

static double calibrate_tsc() {
    auto t0_wall = std::chrono::steady_clock::now();
    uint64_t t0_tsc = rdtsc();
    volatile int sink = 0;
    for (int i = 0; i < 100'000'000; ++i) sink += i;
    uint64_t t1_tsc = rdtsc();
    auto t1_wall = std::chrono::steady_clock::now();
    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1_wall - t0_wall).count();
    return (t1_tsc - t0_tsc) / wall_ns;
}

static void bench_single_thread_throughput() {
    std::printf("\n=== Single-Thread Push/Pop Throughput ===\n");
    SPSCRing<TestMsg, 1 << 16> ring;
    double tpn = calibrate_tsc();

    constexpr int N = 10'000'000;
    TestMsg msg{};

    uint64_t t0 = rdtsc();
    for (int i = 0; i < N; ++i) {
        msg.sequence = i;
        ring.push(msg);
        ring.pop(msg);
    }
    uint64_t t1 = rdtsc();

    double total_ns = (t1 - t0) / tpn;
    std::printf("  %d push+pop pairs in %.0f ns\n", N, total_ns);
    std::printf("  %.1f ns per round-trip\n", total_ns / N);
    std::printf("  %.0f M ops/sec\n", N / (total_ns / 1e9) / 1e6);
}

static void bench_burst_throughput() {
    std::printf("\n=== Burst Fill/Drain Throughput ===\n");
    constexpr size_t CAP = 1 << 16;
    SPSCRing<TestMsg, CAP> ring;
    double tpn = calibrate_tsc();

    TestMsg msg{};
    constexpr int ROUNDS = 100;

    uint64_t push_total = 0, pop_total = 0;

    for (int r = 0; r < ROUNDS; ++r) {
        uint64_t t0 = rdtsc();
        for (size_t i = 0; i < CAP; ++i) {
            msg.sequence = i;
            ring.push(msg);
        }
        uint64_t t1 = rdtsc();
        push_total += (t1 - t0);

        t0 = rdtsc();
        for (size_t i = 0; i < CAP; ++i)
            ring.pop(msg);
        t1 = rdtsc();
        pop_total += (t1 - t0);
    }

    size_t total_ops = static_cast<size_t>(ROUNDS) * CAP;
    std::printf("  Push: %.1f ns/op (%.0f M ops/sec)\n",
                (push_total / tpn) / total_ops,
                total_ops / ((push_total / tpn) / 1e9) / 1e6);
    std::printf("  Pop:  %.1f ns/op (%.0f M ops/sec)\n",
                (pop_total / tpn) / total_ops,
                total_ops / ((pop_total / tpn) / 1e9) / 1e6);
}

static void bench_cross_core_latency() {
    std::printf("\n=== Cross-Core Latency (Producer -> Consumer) ===\n");
    SPSCRing<TestMsg, 1 << 16> ring;
    double tpn = calibrate_tsc();

    constexpr int N = 1'000'000;
    std::vector<uint64_t> latencies(N);
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};

    std::thread consumer([&] {
        ready.store(true);
        TestMsg msg;
        for (int i = 0; i < N; ++i) {
            while (!ring.pop(msg)) {}
            latencies[i] = rdtsc() - msg.tsc;
        }
        done.store(true);
    });

    while (!ready.load()) {}

    TestMsg msg{};
    for (int i = 0; i < N; ++i) {
        msg.sequence = i;
        msg.tsc = rdtsc();
        while (!ring.push(msg)) {}
    }

    consumer.join();

    std::sort(latencies.begin(), latencies.end());
    auto ns = [&](size_t idx) { return latencies[idx] / tpn; };

    std::printf("  n=%d  p50=%.0fns  p90=%.0fns  p99=%.0fns  p99.9=%.0fns  min=%.0fns  max=%.0fns\n",
                N,
                ns(N * 50 / 100), ns(N * 90 / 100), ns(N * 99 / 100),
                ns(N * 999 / 1000), ns(0), ns(N - 1));
}

static void bench_throughput_cross_core() {
    std::printf("\n=== Cross-Core Throughput ===\n");
    SPSCRing<TestMsg, 1 << 16> ring;
    double tpn = calibrate_tsc();

    constexpr int N = 10'000'000;
    std::atomic<bool> ready{false};

    std::thread consumer([&] {
        ready.store(true);
        TestMsg msg;
        for (int i = 0; i < N; ++i)
            while (!ring.pop(msg)) {}
    });

    while (!ready.load()) {}

    TestMsg msg{};
    uint64_t t0 = rdtsc();
    for (int i = 0; i < N; ++i) {
        msg.sequence = i;
        while (!ring.push(msg)) {}
    }
    uint64_t t1 = rdtsc();
    consumer.join();

    double total_ns = (t1 - t0) / tpn;
    std::printf("  %d messages in %.0f ns\n", N, total_ns);
    std::printf("  %.0f M msgs/sec\n", N / (total_ns / 1e9) / 1e6);
}

int main() {
    std::printf("TradeFeed SPSC Ring Buffer Benchmarks\n");
    std::printf("======================================\n");
    std::printf("Calibrating TSC...\n");

    bench_single_thread_throughput();
    bench_burst_throughput();
    bench_cross_core_latency();
    bench_throughput_cross_core();

    std::printf("\nDone.\n");
    return 0;
}
