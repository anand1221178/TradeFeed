#pragma once

#include "../core/types.h"
#include "../codec/sbe_messages.h"
#include "spsc_ring.h"

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <atomic>
#include <array>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

using InboundRing  = SPSCRing<InboundMessage,  RING_SIZE>;
using OutboundRing = SPSCRing<OutboundMessage, RING_SIZE>;

constexpr int    MAX_CLIENTS    = 64;
constexpr int    LISTEN_BACKLOG = 16;
constexpr size_t WIRE_MSG_SIZE  = sizeof(WireMessage);

class Gateway {
    int  listen_fd_ = -1;
    int  event_fd_  = -1;
    bool ready_     = false;

    InboundRing&  inbound_;
    OutboundRing& outbound_;

    struct ClientState {
        int  fd        = -1;
        bool active    = false;
        char read_buf[WIRE_MSG_SIZE];
        size_t read_pos = 0;
    };
    std::array<ClientState, MAX_CLIENTS> clients_{};

    void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void set_nodelay(int fd) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    int find_slot() {
        for (int i = 0; i < MAX_CLIENTS; ++i)
            if (!clients_[i].active) return i;
        return -1;
    }

    void accept_client() {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) return;

        int slot = find_slot();
        if (slot < 0) { close(fd); return; }

        set_nonblocking(fd);
        set_nodelay(fd);

        clients_[slot].fd       = fd;
        clients_[slot].active   = true;
        clients_[slot].read_pos = 0;

#ifdef __linux__
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(event_fd_, EPOLL_CTL_ADD, fd, &ev);
#else
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#endif
    }

    void disconnect(int slot) {
#ifdef __linux__
        epoll_ctl(event_fd_, EPOLL_CTL_DEL, clients_[slot].fd, nullptr);
#else
        struct kevent ev;
        EV_SET(&ev, clients_[slot].fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);
#endif
        close(clients_[slot].fd);
        clients_[slot].fd     = -1;
        clients_[slot].active = false;
    }

    int slot_for_fd(int fd) {
        for (int i = 0; i < MAX_CLIENTS; ++i)
            if (clients_[i].active && clients_[i].fd == fd) return i;
        return -1;
    }

    void read_client(int fd) {
        int slot = slot_for_fd(fd);
        if (slot < 0) return;
        auto& c = clients_[slot];

        for (;;) {
            ssize_t n = recv(fd, c.read_buf + c.read_pos,
                             WIRE_MSG_SIZE - c.read_pos, 0);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    disconnect(slot);
                return;
            }
            c.read_pos += n;

            if (c.read_pos >= WIRE_MSG_SIZE) {
                WireMessage wire;
                std::memcpy(&wire, c.read_buf, WIRE_MSG_SIZE);
                c.read_pos = 0;

                InboundMessage msg{};
                msg.type       = static_cast<MessageType>(wire.type);
                msg.side       = static_cast<Side>(wire.side);
                msg.order_type = static_cast<OrderType>(wire.order_type);
                msg.client_id  = wire.client_id;
                msg.order_id   = wire.order_id;
                msg.price      = wire.price;
                msg.quantity   = wire.quantity;
                msg.recv_tsc   = rdtsc();

                inbound_.push(msg);
            }
        }
    }

    void drain_outbound() {
        OutboundMessage out;
        while (outbound_.pop(out)) {
            WireMessage wire{};
            wire.type      = static_cast<uint8_t>(out.type);
            wire.client_id = out.client_id;
            wire.order_id  = out.order_id;
            wire.price     = out.price;
            wire.quantity  = out.quantity;
            wire.timestamp = out.engine_tsc;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients_[i].active && clients_[i].fd >= 0) {
                    int flags = 0;
#ifdef __linux__
                    flags = MSG_NOSIGNAL;
#endif
                    send(clients_[i].fd, &wire, WIRE_MSG_SIZE, flags);
                }
            }
        }
    }

public:
    Gateway(InboundRing& in, OutboundRing& out, int port = 9000)
        : inbound_(in), outbound_(out) {

        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { std::perror("socket"); return; }

        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef __APPLE__
        setsockopt(listen_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("bind");
            return;
        }
        if (listen(listen_fd_, LISTEN_BACKLOG) < 0) {
            std::perror("listen");
            return;
        }
        set_nonblocking(listen_fd_);

#ifdef __linux__
        event_fd_ = epoll_create1(0);
        if (event_fd_ < 0) { std::perror("epoll_create1"); return; }
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (epoll_ctl(event_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
            std::perror("epoll_ctl");
            return;
        }
#else
        event_fd_ = kqueue();
        if (event_fd_ < 0) { std::perror("kqueue"); return; }
        struct kevent ev;
        EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (kevent(event_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
            std::perror("kevent");
            return;
        }
#endif
        ready_ = true;
    }

    // False if any setup step failed. Callers must not run() a gateway that
    // never bound — it would spin on a socket it does not own.
    bool ready() const { return ready_; }

    ~Gateway() {
        for (int i = 0; i < MAX_CLIENTS; ++i)
            if (clients_[i].active) close(clients_[i].fd);
        if (listen_fd_ >= 0) close(listen_fd_);
        if (event_fd_ >= 0) close(event_fd_);
    }

    void run(std::atomic<bool>& running) {
        while (running.load(std::memory_order_relaxed)) {
#ifdef __linux__
            epoll_event events[64];
            int n = epoll_wait(event_fd_, events, 64, 1);
            for (int i = 0; i < n; ++i) {
                if (events[i].data.fd == listen_fd_)
                    accept_client();
                else
                    read_client(events[i].data.fd);
            }
#else
            struct kevent events[64];
            struct timespec ts = {0, 1000000}; // 1ms timeout
            int n = kevent(event_fd_, nullptr, 0, events, 64, &ts);
            for (int i = 0; i < n; ++i) {
                int fd = static_cast<int>(events[i].ident);
                if (fd == listen_fd_)
                    accept_client();
                else
                    read_client(fd);
            }
#endif
            drain_outbound();
        }
    }
};
