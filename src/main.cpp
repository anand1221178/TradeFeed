#include "core/matching_engine.h"
#include "transport/socket_transport.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

static std::atomic<bool> g_running{true};

static int core_from_env(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : fallback;
}

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

#ifdef __linux__
// Best-effort: the process runs correctly without either, just with worse
// tail latency. SCHED_FIFO needs CAP_SYS_NICE or root.
static void pin_thread(const char* name, int core) {
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (core >= ncpu) {
        std::fprintf(stderr, "warning: %s core %d >= %ld online CPUs, not pinning\n",
                     name, core, ncpu);
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
        std::fprintf(stderr, "warning: %s failed to pin to core %d\n", name, core);

    sched_param sp{};
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        std::fprintf(stderr, "warning: %s SCHED_FIFO unavailable "
                             "(run as root or grant CAP_SYS_NICE)\n", name);
}
#endif

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = std::atoi(argv[1]);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    static InboundRing  inbound_ring;
    static OutboundRing outbound_ring;

    std::printf("TradeFeed exchange starting on port %d\n", port);
    std::printf("  Ring capacity: %zu\n", InboundRing::capacity());
    std::printf("  Order pool:    %zu\n", MAX_ORDERS);
    std::printf("  Price range:   %u - %u\n", MIN_PRICE, MAX_PRICE);

    MatchingEngine engine(inbound_ring, outbound_ring);
    Gateway gateway(inbound_ring, outbound_ring, port);

    if (!gateway.ready()) {
        std::fprintf(stderr, "gateway failed to start on port %d\n", port);
        return 1;
    }

    // Distinct physical cores by default. SMT siblings give a ~4x faster ring
    // handoff but share L1/L2, so the gateway evicts the engine's working set.
    // Override with ENGINE_CORE / GATEWAY_CORE to measure the tradeoff.
    [[maybe_unused]] const int engine_core  = core_from_env("ENGINE_CORE",  1);
    [[maybe_unused]] const int gateway_core = core_from_env("GATEWAY_CORE", 2);

    std::thread engine_thread([&] {
#ifdef __linux__
        pin_thread("engine", engine_core);
#endif
        engine.run(g_running);
    });

    std::thread gateway_thread([&] {
#ifdef __linux__
        pin_thread("gateway", gateway_core);
#endif
        gateway.run(g_running);
    });

    // Poll at 100ms so Ctrl-C is noticed promptly; print every 5s.
    constexpr int TICKS_PER_PRINT = 50;
    for (int tick = 0; g_running.load(std::memory_order_relaxed); ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (tick % TICKS_PER_PRINT != 0) continue;

        const auto& book = engine.book();
        std::printf("\r[orders: %zu | matches: %llu | bid: %u | ask: %u | spread: %u]",
                    book.active_orders(),
                    static_cast<unsigned long long>(book.match_count()),
                    book.best_bid(),
                    book.best_ask(),
                    book.best_ask() > book.best_bid() ? book.best_ask() - book.best_bid() : 0);
        std::fflush(stdout);
    }

    std::printf("\nShutting down...\n");
    engine_thread.join();
    gateway_thread.join();
    std::printf("Total matches: %llu\n",
                static_cast<unsigned long long>(engine.book().match_count()));
    return 0;
}
