#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

// Minimal WebSocket-over-TLS client, enough for Alpaca's market data stream.
//
// Why hand-rolled rather than a library: the matching engine deliberately has
// zero external dependencies, and a feed handler is a separate concern from
// matching (real venues run them as separate processes). Keeping the WebSocket
// here means only this binary links OpenSSL — the engine stays clean.
//
// Implements the subset of RFC 6455 the stream actually uses: the HTTP upgrade
// handshake, text/binary frame reassembly, client-side masking, and ping/pong.
// No extensions, no compression, no fragmented-send.
class AlpacaWebSocket {
public:
    // Free tier. Use "v2/sip" with a paid subscription, or the sandbox host.
    static constexpr const char* DEFAULT_HOST = "stream.data.alpaca.markets";
    static constexpr const char* DEFAULT_PATH = "/v2/iex";

    AlpacaWebSocket() = default;
    ~AlpacaWebSocket();

    AlpacaWebSocket(const AlpacaWebSocket&)            = delete;
    AlpacaWebSocket& operator=(const AlpacaWebSocket&) = delete;

    bool connect(const std::string& host, const std::string& path, int port = 443);
    bool authenticate(const std::string& key, const std::string& secret);
    bool subscribe(const std::vector<std::string>& symbols, bool trades, bool quotes);

    // Blocks for one frame. Returns false on close or error. Pings are answered
    // internally and do not surface to the caller.
    bool read_frame(std::string& out);

    bool send_text(std::string_view payload);
    void close();

    const std::string& last_error() const { return error_; }
    bool connected() const { return ssl_ != nullptr; }

private:
    void*       ssl_     = nullptr;   // SSL*     (opaque: keeps OpenSSL out of this header)
    void*       ctx_     = nullptr;   // SSL_CTX*
    int         fd_      = -1;
    std::string error_;
    std::string rx_;                  // carry-over bytes between frames

    bool tls_connect(const std::string& host, int port);
    bool http_upgrade(const std::string& host, const std::string& path);
    bool read_exact(size_t n);
    bool write_all(const void* data, size_t len);
    bool send_frame(uint8_t opcode, const void* payload, size_t len);
    bool await(std::string_view marker, const char* what);
    void fail(const std::string& what);
};
