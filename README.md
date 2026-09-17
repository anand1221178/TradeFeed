# TradeFeed

A limit order book and matching engine in C++20, built from scratch.

It does what an exchange does: accepts buy and sell orders, keeps the unmatched
ones in a price-ordered book, and executes trades under NASDAQ-style
**price-time priority**. Around it sits a lock-free transport layer, a TCP
gateway, and a market data feed that captures live prices and replays them back
through the engine.

**Measured on an i9-10900K: 55M orders/sec in-process, p50 of 8ns to insert an
order and 13ns to match one.**

Every design decision is explained in [`explanation.md`](explanation.md) — a
5,400-line technical book covering the C++, the hardware, the data structures,
and the market microstructure, with interview questions and exercises per
chapter.

---

## Architecture

```
                         ┌──────────────────────────────────────┐
  client ──TCP────────▶  │  Gateway thread        (pinned)      │
  (orders)               │    epoll / kqueue, 32-byte frames    │
                         └───────────────┬──────────────────────┘
                                         │  lock-free SPSC ring
                                         ▼
                         ┌──────────────────────────────────────┐
                         │  Matching engine thread (pinned)     │
                         │    price-time priority order book    │
                         └───────────────┬──────────────────────┘
                                         │  lock-free SPSC ring
  client ◀──TCP───────────────────────────┘
  (fills)

  Alpaca ──wss──▶ tradefeed-feed ──┬──▶ capture file (32B records)
   (IEX)          (separate binary) └──▶ TCP ──▶ Gateway
```

Two threads, two ring buffers, strict data ownership. The engine owns the book
and every order; the gateway owns the sockets. Nothing is written by both, so
there are no locks anywhere.

The engine is **single-threaded on purpose**. Parallelising a single book would
make execution order depend on thread scheduling, so identical inputs could
produce different trades — which breaks regulatory reconstruction, replay-based
recovery, and regression testing. Real venues shard by symbol, never within a
book.

---

## Quick start

```bash
cmake -B build && cmake --build build -j

./build/tradefeed 9000          # start the exchange
./build/bench_orderbook         # order book benchmarks
./build/bench_ring              # SPSC ring benchmarks
./build/feed_test               # feed parser self-test (no network needed)
```

On Linux, `./deploy_check.sh --bench` verifies the toolchain, CPU topology,
TSC, governor, and core isolation before building and benchmarking.

### Replaying real market data

```bash
export APCA_API_KEY_ID=...  APCA_API_SECRET_KEY=...   # free Alpaca paper account

./build/tradefeed-feed --capture session.bin AAPL NVDA SPY   # capture live
./build/tradefeed 9000 &
./build/tradefeed-feed --replay session.bin --connect 127.0.0.1:9000 --speed 0
```

`--speed 0` fires as fast as possible (benchmarking); `--speed 1` preserves the
original nanosecond timing.

---

## Benchmarks

i9-10900K, Arch Linux, GCC 16, `-O3 -march=native`, `performance` governor.
Timing via `rdtsc` with startup calibration; percentiles from raw sorted samples.

**Order book**

| Operation | p50 | p90 | p99 | p99.9 | max |
|-----------|-----|-----|-----|-------|-----|
| Insert (no match) | 8 ns | 30 ns | 88 ns | 380 ns | 29 µs |
| Cancel | 24 ns | 87 ns | 166 ns | 292 ns | 26 µs |
| Match | 13 ns | 23 ns | 30 ns | 42 ns | 84 ns |
| Mixed 70/20/10 — insert | 10 ns | 21 ns | 50 ns | 131 ns | 28 µs |
| Mixed 70/20/10 — cancel | 19 ns | 81 ns | 206 ns | 292 ns | 33 µs |
| Mixed 70/20/10 — match | 45 ns | 163 ns | 288 ns | 411 ns | 28 µs |

Engine throughput: **55M orders/sec**, 18 ns per order including ring pop,
dispatch, book operation, and fill emission.

**SPSC ring buffer**

| | distinct physical cores | SMT siblings |
|---|---|---|
| One-way latency p50 | 84 ns | 22 ns |
| Throughput | 52M msg/s | 206M msg/s |

Burst push 4.2 ns/op, pop 1.6 ns/op. The 4× gap between core placements is
cache coherence: siblings share L1/L2, so the message line never leaves the
core. The engine defaults to a distinct core anyway — protecting its L1
residency is worth more than 60 ns saved once per message.

The tens-of-microseconds maxima are scheduler preemption, not algorithmic;
`isolcpus` and `nohz_full` remove them.

---

## Design

The performance work is entirely about eliminating memory stalls.

| Decision | Instead of | Why |
|---|---|---|
| Direct-indexed price array | `std::map` | O(1) array access vs ~20 dependent cache misses walking a red-black tree |
| Direct-address order table (`id & mask`) | `unordered_map` | IDs are assigned sequentially, so hashing is wasted work; one AND and one load |
| Intrusive doubly-linked lists | `std::list` | zero allocation per insert, one cache miss per element, O(1) cancel by pointer |
| Pre-allocated order pool | `new` / `delete` | 1–2 ns vs 40–100 ns, no syscalls, no fragmentation, deterministic |
| Three-tier occupancy bitset | linear best-price scan | O(1) via `clz`/`ctz` regardless of book shape |
| Lock-free SPSC rings | mutex + condvar | no 1–10 µs contention, no priority inversion |
| Integer cents | `double` | exact comparison; a cent *is* the tick under Reg NMS |
| 64-byte `Order`, hot fields first | default layout | exactly one cache line, match-critical fields in the first 16 bytes |

Zero external dependencies in the engine. The Alpaca feed is a separate binary
and is the only thing that links OpenSSL.

### A worked example: a 1ms tail

`match_order` showed p99.9 of 57 ns against a **max of 1,073,882 ns** —
reproducible within 1.5% across runs, so deterministic rather than scheduling
noise.

Transparent huge pages were the first hypothesis. Disabling `khugepaged`
background collapse changed nothing, ruling it out.

Reproducible plus a *single* outlier means something that runs exactly once.
The benchmark seeds N resting orders and sends exactly N aggressive ones, so
the final order empties the book — and the best-price scan then walked from
price 100,099 to 1,000,000. Instrumenting it confirmed the shape directly:

```
Best-price scans: 100 calls, 900100 total steps, 899902 worst single scan
```

Ninety-nine scans cost 2 steps. One cost 899,902 — a 21.6 MB sweep of cold
memory. Replacing the scan with a three-tier bitset took the max to **84 ns**,
verified against a brute-force oracle after every benchmark phase.

Full write-up in [Chapter 13.8](explanation.md).

---

## Honest limitations

- **In-process numbers.** The 55M orders/sec excludes the network. Kernel TCP
  alone is 10–30 µs round trip — three orders of magnitude more than the
  engine. End-to-end latency has not been measured.
- **Not production-ready.** Eight known correctness gaps are catalogued in
  Chapter 32, including client-supplied `client_id` (impersonation) and ignored
  ring-full conditions (silent message drops).
- **Single symbol.** One book. Multi-symbol means one engine thread per shard.
- **Level-1 market data only.** The free IEX feed is top-of-book, so replayed
  depth is synthetic and the reconstruction over-trades, since it adds orders
  without modelling cancel/replace. Flow *shape* is real; depth is not.
- **Standard sockets, not kernel bypass.** AF_XDP is the documented next step —
  that is where the remaining latency actually is.

---

## Layout

```
src/core/       types, order pool, price level, price bitset, order book, matching engine
src/transport/  SPSC ring buffer, TCP gateway (epoll/kqueue)
src/codec/      wire protocol and internal message formats
src/analytics/  VWAP, VPIN, order book imbalance, depth
src/feed/       Alpaca WebSocket client, JSON normaliser, capture/replay
bench/          order book and ring buffer benchmarks
explanation.md  the book — 33 chapters, ~5,400 lines
```

## Requirements

C++20 (GCC 10+ / Clang 12+), CMake 3.20+. OpenSSL is optional and only needed
for the market data feed.
