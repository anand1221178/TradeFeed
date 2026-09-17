#include "alpaca_ws.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cstring>
#include <cstdio>

namespace {

constexpr uint8_t OP_TEXT   = 0x1;
constexpr uint8_t OP_BINARY = 0x2;
constexpr uint8_t OP_CLOSE  = 0x8;
constexpr uint8_t OP_PING   = 0x9;
constexpr uint8_t OP_PONG   = 0xA;

std::string base64(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? tbl[v & 0x3F]        : '=';
    }
    return out;
}

std::string json_array(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += '"'; s += v[i]; s += '"';
    }
    return s + "]";
}

} // namespace

AlpacaWebSocket::~AlpacaWebSocket() { close(); }

void AlpacaWebSocket::fail(const std::string& what) {
    error_ = what;
    const unsigned long e = ERR_get_error();
    if (e) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        error_ += ": ";
        error_ += buf;
    }
}

bool AlpacaWebSocket::tls_connect(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    std::snprintf(portstr, sizeof(portstr), "%d", port);

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        fail("getaddrinfo failed for " + host);
        return false;
    }

    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) { freeaddrinfo(res); fail("socket"); return false; }

    if (::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        fail("connect to " + host);
        return false;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { fail("SSL_CTX_new"); return false; }
    ctx_ = ctx;

    // Verify the server certificate. Skipping this would leave the feed open to
    // a MITM rewriting prices, which is a trading-critical failure, not a nicety.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        fail("SSL_CTX_set_default_verify_paths");
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) { fail("SSL_new"); return false; }
    ssl_ = ssl;

    SSL_set_fd(ssl, fd_);
    SSL_set_tlsext_host_name(ssl, host.c_str());          // SNI
    if (SSL_set1_host(ssl, host.c_str()) != 1) {          // hostname check
        fail("SSL_set1_host");
        return false;
    }

    if (SSL_connect(ssl) != 1) { fail("TLS handshake"); return false; }
    return true;
}

bool AlpacaWebSocket::http_upgrade(const std::string& host, const std::string& path) {
    uint8_t nonce[16];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) { fail("RAND_bytes"); return false; }
    const std::string key = base64(nonce, sizeof(nonce));

    std::string req;
    req += "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";

    if (!write_all(req.data(), req.size())) return false;

    // Read until the end of the header block.
    while (rx_.find("\r\n\r\n") == std::string::npos) {
        char buf[1024];
        const int n = SSL_read(static_cast<SSL*>(ssl_), buf, sizeof(buf));
        if (n <= 0) { fail("handshake read"); return false; }
        rx_.append(buf, static_cast<size_t>(n));
    }

    if (rx_.compare(0, 12, "HTTP/1.1 101") != 0) {
        fail("upgrade rejected: " + rx_.substr(0, rx_.find("\r\n")));
        return false;
    }
    rx_.erase(0, rx_.find("\r\n\r\n") + 4);   // keep any frame bytes that arrived
    return true;
}

bool AlpacaWebSocket::connect(const std::string& host, const std::string& path, int port) {
    return tls_connect(host, port) && http_upgrade(host, path);
}

bool AlpacaWebSocket::write_all(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        const int n = SSL_write(static_cast<SSL*>(ssl_), p + sent,
                                static_cast<int>(len - sent));
        if (n <= 0) { fail("SSL_write"); return false; }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool AlpacaWebSocket::read_exact(size_t n) {
    while (rx_.size() < n) {
        char buf[16384];
        const int got = SSL_read(static_cast<SSL*>(ssl_), buf, sizeof(buf));
        if (got <= 0) { fail("SSL_read"); return false; }
        rx_.append(buf, static_cast<size_t>(got));
    }
    return true;
}

bool AlpacaWebSocket::send_frame(uint8_t opcode, const void* payload, size_t len) {
    std::string f;
    f += static_cast<char>(0x80 | opcode);             // FIN + opcode

    // RFC 6455: every client->server frame must be masked.
    if (len < 126) {
        f += static_cast<char>(0x80 | len);
    } else if (len <= 0xFFFF) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>((len >> 8) & 0xFF);
        f += static_cast<char>(len & 0xFF);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            f += static_cast<char>((len >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    if (RAND_bytes(mask, 4) != 1) { fail("RAND_bytes"); return false; }
    f.append(reinterpret_cast<char*>(mask), 4);

    const auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < len; ++i)
        f += static_cast<char>(src[i] ^ mask[i & 3]);

    return write_all(f.data(), f.size());
}

bool AlpacaWebSocket::send_text(std::string_view payload) {
    return send_frame(OP_TEXT, payload.data(), payload.size());
}

bool AlpacaWebSocket::read_frame(std::string& out) {
    for (;;) {
        if (!read_exact(2)) return false;

        const uint8_t b0 = static_cast<uint8_t>(rx_[0]);
        const uint8_t b1 = static_cast<uint8_t>(rx_[1]);
        const uint8_t opcode = b0 & 0x0F;
        const bool    masked = (b1 & 0x80) != 0;        // servers must not mask
        uint64_t len = b1 & 0x7F;
        size_t   hdr = 2;

        if (len == 126) {
            if (!read_exact(4)) return false;
            len = (static_cast<uint8_t>(rx_[2]) << 8) | static_cast<uint8_t>(rx_[3]);
            hdr = 4;
        } else if (len == 127) {
            if (!read_exact(10)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | static_cast<uint8_t>(rx_[2 + i]);
            hdr = 10;
        }
        if (masked) hdr += 4;

        if (!read_exact(hdr + len)) return false;
        std::string payload = rx_.substr(hdr, len);
        rx_.erase(0, hdr + len);

        switch (opcode) {
        case OP_TEXT:
        case OP_BINARY:
            out = std::move(payload);
            return true;
        case OP_PING:
            // Must echo the payload back, or the server drops the connection.
            if (!send_frame(OP_PONG, payload.data(), payload.size())) return false;
            break;
        case OP_PONG:
            break;
        case OP_CLOSE:
            error_ = "server closed connection";
            return false;
        default:
            break;
        }
    }
}

// Alpaca sends {"T":"success","msg":"connected"} the moment the socket opens,
// before any client message. Responses are therefore not one-to-one with
// requests, so wait for the specific message rather than reading a single frame.
bool AlpacaWebSocket::await(std::string_view marker, const char* what) {
    std::string reply;
    for (int i = 0; i < 10; ++i) {          // bounded: never spin on a chatty server
        if (!read_frame(reply)) return false;

        if (reply.find(marker) != std::string_view::npos) return true;

        if (reply.find(R"("T":"error")") != std::string::npos) {
            error_ = std::string(what) + " rejected: " + reply;
            return false;
        }
        // anything else (e.g. the connect greeting) is not ours; keep reading
    }
    error_ = std::string(what) + ": no response after 10 frames";
    return false;
}

bool AlpacaWebSocket::authenticate(const std::string& key, const std::string& secret) {
    const std::string msg =
        R"({"action":"auth","key":")" + key + R"(","secret":")" + secret + R"("})";
    if (!send_text(msg)) return false;
    return await(R"("msg":"authenticated")", "auth");
}

bool AlpacaWebSocket::subscribe(const std::vector<std::string>& symbols,
                                bool trades, bool quotes) {
    std::string msg = R"({"action":"subscribe")";
    if (trades) msg += R"(,"trades":)" + json_array(symbols);
    if (quotes) msg += R"(,"quotes":)" + json_array(symbols);
    msg += "}";

    if (!send_text(msg)) return false;
    return await(R"("T":"subscription")", "subscribe");
}

void AlpacaWebSocket::close() {
    if (ssl_) {
        send_frame(OP_CLOSE, nullptr, 0);
        SSL_shutdown(static_cast<SSL*>(ssl_));
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
    if (ctx_) { SSL_CTX_free(static_cast<SSL_CTX*>(ctx_)); ctx_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}
