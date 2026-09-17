#include "../src/transport/spsc_ring.h"
#include "../src/core/types.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <chrono>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// Cross-core numbers are meaningless unless the threads actually stay on
// distinct cores. Override with PRODUCER_CORE / CONSUMER_CORE.
static int producer_core() {
    const char* e = std::getenv("PRODUCER_CORE");
    return e ? std::atoi(e) : 1;
}
static int consumer_core() {
    const char* e = std::getenv("CONSUMER_CORE");
    return e ? std::atoi(e) : 2;
}

static void pin_to(int core) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core;
#endif
}

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
    (void)sink;
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

// True one-way core-to-core latency: exactly one message in flight, so the
// measurement is transfer cost, not queueing delay.
static void bench_ping_pong_latency() {
    std::printf("\n=== Core-to-Core Latency (ping-pong, 1 msg in flight) ===\n");
    static SPSCRing<TestMsg, 1024> req;
    static SPSCRing<TestMsg, 1024> resp;
    double tpn = calibrate_tsc();

    constexpr int N = 200'000;
    std::vector<uint64_t> rtt(N);
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};

    std::thread responder([&] {
        pin_to(consumer_core());
        ready.store(true);
        TestMsg m;
        while (!stop.load(std::memory_order_relaxed)) {
            if (req.pop(m))
                while (!resp.push(m)) {}
        }
    });

    while (!ready.load()) {}
    pin_to(producer_core());

    TestMsg m{};
    for (int i = 0; i < 2000; ++i) {          // warmup
        m.sequence = i;
        while (!req.push(m)) {}
        while (!resp.pop(m)) {}
    }

    for (int i = 0; i < N; ++i) {
        m.sequence = i;
        uint64_t t0 = rdtsc();
        while (!req.push(m)) {}
        while (!resp.pop(m)) {}
        rtt[i] = rdtsc() - t0;
    }

    stop.store(true, std::memory_order_relaxed);
    responder.join();

    std::sort(rtt.begin(), rtt.end());
    auto ow = [&](size_t i) { return rtt[i] / tpn / 2.0; };   // one-way = rtt / 2
    std::printf("  n=%d  p50=%.0fns  p90=%.0fns  p99=%.0fns  p99.9=%.0fns  min=%.0fns\n",
                N, ow(N*50/100), ow(N*90/100), ow(N*99/100), ow(N*999/1000), ow(0));
    std::printf("  (round-trip p50 = %.0fns)\n", rtt[N/2] / tpn);
}

static void bench_cross_core_latency() {
    std::printf("\n=== Queueing Latency (saturated producer — NOT transfer latency) ===\n");
    SPSCRing<TestMsg, 1 << 16> ring;
    double tpn = calibrate_tsc();

    constexpr int N = 1'000'000;
    std::vector<uint64_t> latencies(N);
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};

    std::thread consumer([&] {
        pin_to(consumer_core());
        ready.store(true);
        TestMsg msg;
        for (int i = 0; i < N; ++i) {
            while (!ring.pop(msg)) {}
            uint64_t delta = rdtsc() - msg.tsc;
            // Cross-core TSC read can appear to go backwards; treat wrap as 0.
            latencies[i] = (delta > (1ULL << 62)) ? 0 : delta;
        }
        done.store(true);
    });

    while (!ready.load()) {}
    pin_to(producer_core());

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
        pin_to(consumer_core());
        ready.store(true);
        TestMsg msg;
        for (int i = 0; i < N; ++i)
            while (!ring.pop(msg)) {}
    });

    while (!ready.load()) {}
    pin_to(producer_core());

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
#ifdef __linux__
    std::printf("Cross-core tests: producer=cpu%d consumer=cpu%d "
                "(override with PRODUCER_CORE/CONSUMER_CORE)\n",
                producer_core(), consumer_core());
#else
    std::printf("Cross-core tests: unpinned (no affinity API on this platform)\n");
#endif
    std::printf("Calibrating TSC...\n");

    bench_single_thread_throughput();
    bench_burst_throughput();
    bench_ping_pong_latency();
    bench_cross_core_latency();
    bench_throughput_cross_core();

    std::printf("\nDone.\n");
    return 0;
}
