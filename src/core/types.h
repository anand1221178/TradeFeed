#pragma once

#include <cstdint>
#include <cstddef>

constexpr size_t CACHE_LINE = 64;

using Price     = uint32_t;
using Quantity  = uint32_t;
using OrderId   = uint64_t;
using ClientId  = uint32_t;
using Timestamp = uint64_t;

constexpr Price  INVALID_PRICE = 0;
constexpr Price  MIN_PRICE     = 1;
constexpr Price  MAX_PRICE     = 1'000'000;
constexpr size_t PRICE_LEVELS  = MAX_PRICE - MIN_PRICE + 1;
constexpr size_t MAX_ORDERS    = 1 << 20;
constexpr size_t ORDER_MASK    = MAX_ORDERS - 1;
constexpr size_t RING_SIZE     = 1 << 18;

enum class Side      : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit = 0, Market = 1 };

enum class MessageType : uint8_t {
    NewOrder       = 1,
    CancelOrder    = 2,
    ModifyOrder    = 3,
    OrderAccepted  = 10,
    OrderCanceled  = 11,
    OrderExecuted  = 12,
    OrderRejected  = 13,
};

inline Timestamp rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return 0;
#endif
}

// Hot fields first: price/qty checked every match attempt.
// Then list pointers for traversal. Then identity for fill/ack.
struct alignas(CACHE_LINE) Order {
    Price     price;
    Quantity  quantity;
    Quantity  filled_qty;
    Side      side;
    OrderType type;

    Order*    prev = nullptr;
    Order*    next = nullptr;

    OrderId   id;
    ClientId  client_id;
    Timestamp timestamp;

    Quantity remaining() const { return quantity - filled_qty; }
    bool is_filled()     const { return filled_qty >= quantity; }
    void fill(Quantity q)      { filled_qty += q; }
};
