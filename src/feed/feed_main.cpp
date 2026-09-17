#include "alpaca_ws.h"
#include "feed_normalizer.h"
#include "../codec/sbe_messages.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: tradefeed-feed [options] SYMBOL...\n"
        "\n"
        "  --capture FILE     write 32-byte CaptureRecords to FILE\n"
        "  --replay FILE      replay FILE instead of connecting to Alpaca\n"
        "  --connect HOST:PORT  send synthesised orders to a running exchange\n"
        "  --speed N          replay speed multiplier (0 = as fast as possible)\n"
        "  --trades-only      subscribe to trades only\n"
        "  --quotes-only      subscribe to quotes only\n"
        "  --feed PATH        stream path (default /v2/iex, free tier)\n"
        "  --limit N          stop after N records\n"
        "\n"
        "Credentials come from APCA_API_KEY_ID and APCA_API_SECRET_KEY.\n"
        "\n"
        "examples:\n"
        "  tradefeed-feed --capture aapl.bin AAPL MSFT\n"
        "  tradefeed-feed --replay aapl.bin --connect 127.0.0.1:9000 --speed 0\n");
}

int dial(const std::string& hostport) {
    const size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) { std::fprintf(stderr, "bad --connect\n"); return -1; }
    const std::string host = hostport.substr(0, colon);
    const int port = std::atoi(hostport.c_str() + colon + 1);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return -1; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        std::fprintf(stderr, "bad host %s\n", host.c_str());
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::perror("connect");
        ::close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool send_wire(int fd, const WireMessage& w) {
    size_t sent = 0;
    while (sent < sizeof(w)) {
        const ssize_t n = ::send(fd, reinterpret_cast<const char*>(&w) + sent,
                                 sizeof(w) - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Map a normalised tick onto orders the matching engine understands.
//
// The free IEX feed is level 1 only — top of book, no depth — so this is a
// reconstruction, not a replica. A quote becomes a resting bid and ask at the
// quoted prices; a trade becomes an aggressive order that crosses. Flow shape
// (burstiness, price clustering, size distribution) is real, which is the
// point; the depth behind the touch is not.
int emit_orders(int fd, const CaptureRecord& r, ClientId client) {
    WireMessage w{};
    w.type       = static_cast<uint8_t>(MessageType::NewOrder);
    w.order_type = static_cast<uint8_t>(OrderType::Limit);
    w.client_id  = client;
    w.timestamp  = r.venue_ns;

    int sent = 0;
    if (r.type == static_cast<uint8_t>(TickType::Quote)) {
        w.side     = static_cast<uint8_t>(Side::Buy);
        w.price    = r.p1;
        w.quantity = r.s1 ? r.s1 * 100 : 100;          // sizes are round lots
        if (!send_wire(fd, w)) return sent; ++sent;

        w.side     = static_cast<uint8_t>(Side::Sell);
        w.price    = r.p2;
        w.quantity = r.s2 ? r.s2 * 100 : 100;
        if (!send_wire(fd, w)) return sent; ++sent;
    } else {
        // A trade prints without a side; alternate so neither book side starves.
        static bool buy = true;
        buy = !buy;
        w.side     = static_cast<uint8_t>(buy ? Side::Buy : Side::Sell);
        w.price    = r.p1;
        w.quantity = r.s1 ? r.s1 : 1;
        if (!send_wire(fd, w)) return sent; ++sent;
    }
    return sent;
}

int run_replay(const std::string& path, int fd, double speed, uint64_t limit) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::perror("fopen"); return 1; }

    CaptureRecord r{};
    uint64_t n = 0, orders = 0, prev_ns = 0;

    while (g_running.load(std::memory_order_relaxed) &&
           std::fread(&r, sizeof(r), 1, f) == 1) {

        if (speed > 0.0 && prev_ns && r.venue_ns > prev_ns) {
            const uint64_t gap = static_cast<uint64_t>((r.venue_ns - prev_ns) / speed);
            if (gap > 1000) ::usleep(static_cast<useconds_t>(gap / 1000));
        }
        prev_ns = r.venue_ns;

        if (fd >= 0) orders += static_cast<uint64_t>(emit_orders(fd, r, 1));
        if (++n % 10000 == 0)
            std::fprintf(stderr, "\rreplayed %llu ticks, %llu orders",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(orders));
        if (limit && n >= limit) break;
    }

    std::fclose(f);
    std::fprintf(stderr, "\nreplayed %llu ticks, sent %llu orders\n",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(orders));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    std::string capture, replay, connect_to;
    std::string path = AlpacaWebSocket::DEFAULT_PATH;
    std::vector<std::string> symbols;
    bool trades = true, quotes = true;
    double speed = 1.0;
    uint64_t limit = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };

        if      (a == "--capture")     capture    = next();
        else if (a == "--replay")      replay     = next();
        else if (a == "--connect")     connect_to = next();
        else if (a == "--feed")        path       = next();
        else if (a == "--speed")       speed      = std::atof(next());
        else if (a == "--limit")       limit      = std::strtoull(next(), nullptr, 10);
        else if (a == "--trades-only") quotes     = false;
        else if (a == "--quotes-only") trades     = false;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { usage(); return 1; }
        else symbols.push_back(a);
    }

    int fd = -1;
    if (!connect_to.empty()) {
        fd = dial(connect_to);
        if (fd < 0) return 1;
        std::fprintf(stderr, "connected to exchange at %s\n", connect_to.c_str());
    }

    if (!replay.empty()) {
        const int rc = run_replay(replay, fd, speed, limit);
        if (fd >= 0) ::close(fd);
        return rc;
    }

    if (symbols.empty()) { usage(); return 1; }

    const char* key    = std::getenv("APCA_API_KEY_ID");
    const char* secret = std::getenv("APCA_API_SECRET_KEY");
    if (!key || !secret) {
        std::fprintf(stderr, "set APCA_API_KEY_ID and APCA_API_SECRET_KEY\n");
        return 1;
    }

    std::FILE* out = nullptr;
    if (!capture.empty()) {
        out = std::fopen(capture.c_str(), "wb");
        if (!out) { std::perror("fopen"); return 1; }
    }

    AlpacaWebSocket ws;
    if (!ws.connect(AlpacaWebSocket::DEFAULT_HOST, path)) {
        std::fprintf(stderr, "connect: %s\n", ws.last_error().c_str());
        return 1;
    }
    if (!ws.authenticate(key, secret)) {
        std::fprintf(stderr, "auth: %s\n", ws.last_error().c_str());
        return 1;
    }
    if (!ws.subscribe(symbols, trades, quotes)) {
        std::fprintf(stderr, "subscribe: %s\n", ws.last_error().c_str());
        return 1;
    }
    std::fprintf(stderr, "streaming %zu symbol(s) from %s\n", symbols.size(), path.c_str());

    std::string frame;
    uint64_t ticks = 0, orders = 0, skipped = 0;

    while (g_running.load(std::memory_order_relaxed) && ws.read_frame(frame)) {
        feed::for_each_object(frame, [&](std::string_view obj) {
            CaptureRecord r{};
            if (!feed::normalize(obj, r)) { ++skipped; return; }
            ++ticks;
            if (out) std::fwrite(&r, sizeof(r), 1, out);
            if (fd >= 0) orders += static_cast<uint64_t>(emit_orders(fd, r, 1));
        });

        if (ticks && ticks % 500 == 0) {
            std::fprintf(stderr, "\rticks %llu  orders %llu  skipped %llu",
                         static_cast<unsigned long long>(ticks),
                         static_cast<unsigned long long>(orders),
                         static_cast<unsigned long long>(skipped));
            if (out) std::fflush(out);
        }
        if (limit && ticks >= limit) break;
    }

    std::fprintf(stderr, "\ndone: %llu ticks, %llu orders, %llu skipped\n",
                 static_cast<unsigned long long>(ticks),
                 static_cast<unsigned long long>(orders),
                 static_cast<unsigned long long>(skipped));

    if (out) std::fclose(out);
    if (fd >= 0) ::close(fd);
    ws.close();
    return 0;
}
