# TradeFeed: From Zero to a Low-Latency Exchange

## A Textbook on Building a Matching Engine in Modern C++

---

## Preface: How to Read This Book

This book teaches you everything needed to write TradeFeed from a blank directory, and to defend every line of it in an interview. It assumes you can write a `for` loop and know what a function is. It assumes nothing else.

Every chapter follows the same shape:

1. **The concept in general** — with a small, self-contained example unrelated to trading.
2. **Why it matters for latency** — what it costs in nanoseconds, and why.
3. **How TradeFeed uses it** — the real code, line by line.
4. **Interview questions** — what you will be asked, and the answer you should give.
5. **Exercises** — things to build or break to prove you understand.

The book is organised so that each part builds on the last:

| Part | Topic | What you'll be able to do afterwards |
|------|-------|--------------------------------------|
| I | C++ Foundations | Read any line of the codebase and know what every token does |
| II | Hardware | Explain why one version of code is 10x faster than another that "does the same thing" |
| III | Data Structures | Choose the right container and justify it with cache-line arithmetic |
| IV | Concurrency | Write a correct lock-free queue and explain the memory model |
| V | Market Microstructure | Explain what an exchange does and how orders match |
| VI | The System | Walk through every file of TradeFeed |
| VII | Interview & Beyond | Answer the hard questions; know the upgrade paths |

Do the exercises. Reading about cache lines is not the same as watching a benchmark go from 400ns to 5ns because you added `alignas(64)`.

### The Golden Rule of Low Latency

> **Every nanosecond has a cause.** If code is slow, something physical happened: a cache miss, a branch mispredict, a system call, a lock, an allocation. The job is to find the physical cause and remove it.

This book is about learning to see those physical causes.

---

# PART I — C++ FOUNDATIONS

---

## Chapter 1: Files, Headers, and the Preprocessor

### 1.1 What happens when you compile

When you run `c++ main.cpp`, four separate programs run in sequence:

```
main.cpp ──[preprocessor]──▶ main.i ──[compiler]──▶ main.s ──[assembler]──▶ main.o ──[linker]──▶ a.out
            (text substitution)        (C++ → assembly)      (assembly → machine code)   (combine .o files)
```

1. **Preprocessor**: handles every line starting with `#`. It is a dumb text-substitution tool — it does not understand C++. It pastes files, replaces macros, and deletes code under false `#if`s. Its output is a single enormous text file called a *translation unit*.
2. **Compiler**: turns that translation unit into assembly. It only sees one translation unit at a time. It has no idea other `.cpp` files exist.
3. **Assembler**: turns assembly into an object file (`.o`) — machine code with a table of "symbols I define" and "symbols I need".
4. **Linker**: takes all the `.o` files, matches every "symbol I need" to exactly one "symbol I define", and writes the executable.

This model explains almost every confusing C++ build error you will ever see.

### 1.2 `#include`

```cpp
#include <cstdint>      // angle brackets: search system/compiler include paths
#include "types.h"      // quotes: search the current file's directory first, then system paths
```

`#include` literally copies the entire contents of the named file into the current file at that line. Nothing more. If `types.h` is 60 lines, your translation unit grows by 60 lines.

Because it is literal pasting, if `a.h` includes `types.h` and `b.h` includes `types.h`, and `main.cpp` includes both `a.h` and `b.h`, then `types.h` is pasted **twice** into `main.cpp`. Every `struct Order { ... }` inside it is now defined twice, and the compiler errors with *redefinition of 'Order'*.

### 1.3 `#pragma once`

```cpp
#pragma once
```

This is the fix. It tells the preprocessor: *if you have already pasted this file into this translation unit, skip it.* The second `#include "types.h"` becomes a no-op.

The traditional (and ISO-standard) way is an *include guard*:

```cpp
#ifndef TRADEFEED_TYPES_H
#define TRADEFEED_TYPES_H
// ... contents ...
#endif
```

The first time through, `TRADEFEED_TYPES_H` is undefined, so the body is included and the macro is defined. The second time, the macro exists, `#ifndef` is false, and the body is skipped.

**Why `#pragma once` over include guards?**

| | `#pragma once` | Include guard |
|---|---|---|
| Standard? | No (but supported by GCC, Clang, MSVC, ICC — every compiler you'll use) | Yes |
| Typo risk | None | Copy-paste a header and forget to rename the macro → silent header disappearance |
| Speed | Compiler can skip re-opening the file entirely | Compiler must open, scan to `#endif` |
| Failure mode | Same file reachable via two paths (symlinks, network mounts) may be included twice | None |

For a project you control, `#pragma once` is the pragmatic choice. In an interview, mention both and the tradeoff.

**What `#pragma` means in general**: a "pragmatic" directive — an instruction to the compiler that is not part of the language. Other examples in this codebase: `#pragma pack(push, 1)` (Chapter 3).

### 1.4 Headers vs source files

```
types.h          ← declarations + small inline definitions; included by many files
order_book.h     ← class declaration (what the class looks like)
order_book.cpp   ← class definition (what the methods do); compiled once
```

A **declaration** says "this thing exists and has this shape":
```cpp
bool cancel_order(OrderId id);      // declaration: name, parameters, return type
```

A **definition** says "here is the actual code / storage":
```cpp
bool OrderBook::cancel_order(OrderId id) { ... }   // definition
```

You may declare something as many times as you like. You may define it exactly **once** across the entire program. This is the **One Definition Rule (ODR)**. Violate it and the linker reports *duplicate symbol*.

So: put declarations in headers (they get pasted everywhere — fine, declarations can repeat). Put definitions in `.cpp` files (compiled once — one definition).

### 1.5 The exception: `inline` and header-only code

TradeFeed's `spsc_ring.h`, `price_level.h`, `order_pool.h`, `book_analytics.h`, and `socket_transport.h` are *header-only*: the full method bodies live in the header. Doesn't that violate the ODR when two `.cpp` files include them?

No, for two reasons:

1. **Methods defined inside a class body are implicitly `inline`.**
   ```cpp
   class PriceLevel {
       Order* front() const { return head_; }   // implicitly inline
   };
   ```
2. **Free functions can be marked `inline` explicitly:**
   ```cpp
   inline Timestamp rdtsc() { ... }
   ```

`inline` here does **not** mean "please inline this function into the caller" (the compiler decides that on its own with `-O2`+). Its actual meaning is: *"this definition may appear in multiple translation units; they are all identical; linker, pick one."* It's an ODR exemption.

**Why header-only for the hot path?** Because the compiler can only inline (in the optimisation sense) a function whose body it can see. If `PriceLevel::push` lived in `price_level.cpp`, then when compiling `order_book.cpp` the compiler sees only the declaration — it must emit a real `call` instruction. A `call` costs ~1–2ns plus register spilling plus a missed opportunity for the optimiser to see across the boundary. For a 3-line function called millions of times a second, that matters. (Link-time optimisation, `-flto`, can partially recover this, but header-only is the simplest guarantee.)

**Why not header-only for everything?** `order_book.cpp` and `matching_engine.cpp` are large. If they were headers, every file that included them would recompile them. Compile time matters for iteration speed. Rule of thumb: tiny hot functions → header; large methods → `.cpp`.

### 1.6 `static` at file scope

In `main.cpp`:
```cpp
static std::atomic<bool> g_running{true};
static void signal_handler(int) { ... }
```

`static` on a file-scope variable or function gives it **internal linkage**: it is invisible to other translation units. Two `.cpp` files can each have their own `static void helper()` with no linker collision. Without `static`, both would be *external* symbols, and the linker would complain about duplicates.

The benchmarks use `static void bench_add_order()` for the same reason: these are private to the file.

### 1.7 Translation units in TradeFeed

The `tradefeed` executable is built from three translation units:

```
main.cpp            (+ everything it includes: matching_engine.h → order_book.h → ..., socket_transport.h → ...)
order_book.cpp      (+ order_book.h → types.h, order_pool.h, price_level.h)
matching_engine.cpp (+ matching_engine.h → order_book.h, spsc_ring.h, sbe_messages.h)
```

Each is compiled independently to a `.o`, then linked. `OrderBook::add_order` is *defined* in `order_book.o` and *needed* by `matching_engine.o`; the linker connects them.

### Interview Questions

**Q: What does `#pragma once` do and why not use include guards?**
A: Prevents a header from being included twice in the same translation unit. Include guards are standard but require a unique macro per file (typo risk); `#pragma once` is universally supported, faster, and can't be mis-typed. Its only failure case is the same file reachable via different paths.

**Q: What's the difference between a declaration and a definition?**
A: A declaration introduces a name and its type; a definition provides the body or storage. You can declare many times, define once (ODR). Headers hold declarations; `.cpp` files hold definitions; `inline` definitions in headers are exempt from the ODR as long as all copies are identical.

**Q: What does `inline` actually mean?**
A: Primarily an ODR exemption — "this may be defined in multiple translation units." Inlining as an optimisation is a separate decision made by the compiler; the keyword is only a weak hint for that.

### Exercises

1. Remove `#pragma once` from `types.h`, then include it twice in a test file. Read the error. Put it back.
2. Move `PriceLevel::push` into a new `price_level.cpp`. Rebuild. Run `bench_orderbook` before and after. Explain any difference (hint: `-O3` and inlining).
3. Write two `.cpp` files that each define `int helper() { return 1; }`. Link them. Fix the error two different ways (`static`, `inline`).

---

## Chapter 2: Types

### 2.1 Fixed-width integers

```cpp
#include <cstdint>
uint8_t   a;   // exactly 8 bits,  unsigned,  0 .. 255
uint32_t  b;   // exactly 32 bits, unsigned,  0 .. 4,294,967,295
uint64_t  c;   // exactly 64 bits, unsigned,  0 .. 18,446,744,073,709,551,615
int32_t   d;   // exactly 32 bits, signed,   -2,147,483,648 .. 2,147,483,647
```

Plain `int`, `long`, `short` have *platform-dependent* sizes. `long` is 64 bits on Linux and 32 bits on Windows. For a struct that must be exactly 32 bytes on the wire, you cannot afford that ambiguity. `<cstdint>` gives you exact widths.

`size_t` is the unsigned type used for sizes and indices — it's whatever width the platform's pointer is (64 bits on every machine you'll deploy to). Use it for array indices and counts, because `std::vector::size()` returns it and mixing signed/unsigned comparisons produces warnings and subtle bugs.

### 2.2 Unsigned arithmetic wraps — and that's a feature

```cpp
uint32_t x = 0;
x = x - 1;      // x == 4,294,967,295 — no error, no exception
```

Unsigned overflow is **defined behaviour**: it wraps modulo 2ⁿ. Signed overflow is **undefined behaviour** (the compiler may assume it never happens and optimise accordingly, producing bizarre results).

TradeFeed's ring buffer *depends* on unsigned wrap. `head_` and `tail_` are `size_t` counters that increase forever. After 2⁶⁴ pushes they wrap to zero. But `head - tail` still gives the correct element count because subtraction also wraps:

```
head = 3, tail = 18,446,744,073,709,551,614 (i.e. -2 in wrap terms)
head - tail = 3 - (2^64 - 2) = 5 (mod 2^64)   ✓ correct: 5 elements
```

At 100M pushes per second, 2⁶⁴ pushes takes 5,800 years. The wrap will never happen in practice, but the arithmetic is correct even if it did.

### 2.3 Type aliases: `using`

```cpp
using Price     = uint32_t;
using Quantity  = uint32_t;
using OrderId   = uint64_t;
```

`using Price = uint32_t;` is the modern spelling of `typedef uint32_t Price;`. It creates a *new name for the same type* — not a new type. `Price` and `Quantity` are both `uint32_t` and can be assigned to each other freely.

So why bother?

1. **Documentation.** `Price best_bid()` tells you more than `uint32_t best_bid()`.
2. **Single point of change.** If you move to FX with 8 decimal places and need `uint64_t` prices, you change one line.
3. **Grep-ability.** Search for `Price` and you find every price-related field.

What it does *not* give you: type safety. `add_order(price, qty)` and `add_order(qty, price)` both compile. Strong typedefs (wrapping in a struct) would catch that at the cost of verbosity. TradeFeed keeps aliases for simplicity — an interview-worthy tradeoff to mention.

### 2.4 Why prices are integers

```cpp
constexpr Price MAX_PRICE = 1'000'000;   // $100.0000 with 4 implied decimals
```

A price of `$150.25` is stored as `1'502'500`. The decimal point is *implied* — everyone agrees it sits four digits from the right.

The alternative, `double price = 150.25;`, is wrong for money:

```cpp
double a = 0.1, b = 0.2;
std::cout << (a + b == 0.3);   // prints 0 (false!)
```

`0.1` has no exact binary representation (it's `0.1000000000000000055511151231257827...`). Every arithmetic operation on doubles rounds. After a million fills, your P&L is off by cents. Worse, `bid_price >= ask_price` — the core matching predicate — becomes unreliable.

Integers are exact. `1'502'500 >= 1'502'499` is always true. Comparison is one `CMP` instruction. And integer division/multiplication by known constants is faster than floating-point equivalents.

The `'` in `1'000'000` is a C++14 *digit separator*. It's ignored by the compiler; it's purely for human readability.

**Real exchanges do this.** NASDAQ ITCH uses 4 implied decimals in a 32-bit field. CME uses 64-bit mantissa + exponent. Nobody uses `double` for prices.

### 2.5 `constexpr` vs `const` vs `#define`

```cpp
#define CACHE_LINE 64                    // preprocessor: dumb text substitution
const size_t CACHE_LINE = 64;            // typed constant, might live in memory
constexpr size_t CACHE_LINE = 64;        // typed constant, guaranteed compile-time
```

- `#define` has no type, no scope, and is invisible to the debugger. Avoid.
- `const` means "I won't modify this". The compiler *may* evaluate it at compile time, but isn't required to.
- `constexpr` means "this *must* be computable at compile time". You can use it as an array size, a template argument, or in a `static_assert`.

```cpp
constexpr size_t PRICE_LEVELS = MAX_PRICE - MIN_PRICE + 1;   // computed at compile time: 1,000,000
std::unique_ptr<PriceLevel[]> bids_(new PriceLevel[PRICE_LEVELS]);   // uses it
```

`constexpr` can also mark functions, meaning "if called with constant arguments, evaluate at compile time":

```cpp
static constexpr size_t capacity() { return Capacity; }   // in SPSCRing
```

### 2.6 `enum class`

```cpp
enum class Side : uint8_t { Buy = 0, Sell = 1 };
```

Three things are happening here:

**`enum`** declares a set of named integer constants.

**`class`** makes it a *scoped* enumeration. You must write `Side::Buy`, not `Buy`. And it does not implicitly convert to `int`:
```cpp
enum Colour { Red, Green };          // old-style
enum class Side { Buy, Sell };       // scoped

int x = Red;           // OK (implicit conversion) — and a source of bugs
int y = Side::Buy;     // ERROR — must static_cast<int>(Side::Buy)
if (Red == Side::Buy)  // ERROR — different types, cannot compare
```
Old-style enums pollute the enclosing scope (`Red` becomes a global name) and silently convert to `int`, so you can accidentally pass a `Colour` where a `Side` is expected. Scoped enums prevent both.

**`: uint8_t`** fixes the *underlying type*. Without it, the compiler picks (usually `int`, 4 bytes). With it, `sizeof(Side) == 1`. This matters for struct layout: `Order` has `Side side; OrderType type;` adjacent — 2 bytes total, not 8.

Converting back to an integer, e.g. for the wire:
```cpp
wire.type = static_cast<uint8_t>(out.type);
```
And from the wire:
```cpp
msg.type = static_cast<MessageType>(wire.type);
```

**Caution**: `static_cast<MessageType>(99)` compiles and produces an enum with no named value. The gateway trusts the client here; a production system would validate at this trust boundary.

### 2.7 `static_cast` and friends

C++ has four named casts. TradeFeed uses two:

```cpp
static_cast<uint8_t>(out.type)          // "normal" conversion: enum↔int, int↔float, base↔derived
reinterpret_cast<sockaddr*>(&addr)      // "trust me": reinterpret the bits as another pointer type
```

`static_cast` is checked by the compiler for plausibility. `reinterpret_cast` is not — it's for interfacing with C APIs (the BSD socket API takes a generic `sockaddr*` but you fill in a `sockaddr_in`).

Avoid C-style casts `(int)x` — they silently try every cast in turn and can do `reinterpret_cast` when you meant `static_cast`.

### 2.8 `static_assert`

```cpp
static_assert(sizeof(WireMessage) == 32, "wire message must be exactly 32 bytes");
static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be power of 2");
```

A compile-time check. If the condition is false, compilation fails with your message. Zero runtime cost. Use it to encode invariants that the design relies on — if someone adds a field to `WireMessage`, the build breaks immediately rather than the wire protocol silently corrupting.

The power-of-2 trick: a power of 2 in binary is a single 1-bit followed by zeros (`1000`). Subtracting 1 gives all ones below it (`0111`). AND-ing them gives zero. Any non-power-of-2 has at least two 1-bits, and the AND is non-zero.

```
8   = 1000    8-1 = 0111    8 & 7 = 0000  ✓ power of 2
12  = 1100   12-1 = 1011   12 & 11 = 1000  ✗ not
```

### Interview Questions

**Q: Why `uint32_t` for prices and not `double`?**
A: Doubles can't represent most decimal fractions exactly; rounding accumulates and equality comparisons fail. Integers with implied decimals are exact, faster to compare, and match what real exchange protocols (ITCH, OUCH, SBE) do.

**Q: What's the difference between `enum` and `enum class`?**
A: Scoped enums don't leak names into the enclosing scope and don't implicitly convert to `int`. That prevents comparing or mixing unrelated enums by accident. You can also fix the underlying type to control `sizeof`.

**Q: Why `constexpr` over `const`?**
A: `constexpr` guarantees compile-time evaluation, so the value can be used as an array bound, template argument, or in `static_assert`. `const` only promises immutability.

**Q: Is unsigned overflow safe?**
A: Yes — defined to wrap modulo 2ⁿ. Signed overflow is UB. The SPSC ring relies on unsigned wrap for its monotonic counters.

### Exercises

1. Write a program that adds `0.1` one hundred times as a `double` and compares it to `10.0`. Then do the same with integer cents. Explain the output.
2. Change `Side` to a plain `enum` (not `class`). Find every line that now compiles differently or breaks.
3. Write a `static_assert` that fails if `sizeof(Order) != 64`. Add it to `types.h`. Then add a `uint64_t` field to `Order` and watch it fire.
4. Implement a `struct StrongPrice { uint32_t v; }` and try to pass it where a `Quantity` is expected. What does the compiler say? What would it take to make arithmetic work?

---

## Chapter 3: Structs, Classes, and Memory Layout

### 3.1 `struct` vs `class`

They are the same thing with one difference: members of a `struct` are `public` by default; members of a `class` are `private` by default.

```cpp
struct Fill { OrderId bid_id; ... };        // plain data; everything public
class OrderBook { PriceLevel* bids_; ... public: ... };   // has invariants; hides internals
```

Convention (followed in TradeFeed): use `struct` for plain aggregates of data with no invariants to protect. Use `class` when the object has internal state that must stay consistent (an `OrderBook` must never have `best_bid_` pointing at an empty level — so the fields are private and only the methods can touch them).

The trailing underscore (`bids_`, `head_`) is a naming convention for private members. It lets you write `Price best_bid() const { return best_bid_; }` without the name clash between the getter and the field.

### 3.2 Member functions and `const`

```cpp
Quantity remaining() const { return quantity - filled_qty; }
void fill(Quantity q)      { filled_qty += q; }
```

A member function declared `const` promises not to modify the object. The compiler enforces it: inside `remaining()`, `filled_qty = 0;` would be an error.

Why it matters:
- You can call `const` methods on a `const Order&`. `MatchingEngine::book()` returns `const OrderBook&` — the caller can read `best_bid()` but cannot `add_order()`. This is how the main thread's status printer safely reads the book (it's a data race, strictly — but a benign one on a single `uint32_t` for a status line; a production system would publish a snapshot through the ring).
- It documents intent: every `const` method is a "query"; every non-`const` is a "command".

### 3.3 Default member initialisers

```cpp
Order* prev = nullptr;
Order* next = nullptr;
```

These run whenever an `Order` is constructed without an explicit initialiser for that field. `std::vector<Order> arena_(capacity)` value-initialises each element, so every order starts with null links.

Fields *without* an initialiser (`price`, `id`, ...) are left uninitialised by a plain `Order o;` — they contain garbage. TradeFeed always assigns every field in `add_order` before use, so this is safe, and it avoids the cost of zeroing 1M × 64 bytes at startup for fields that will be overwritten anyway.

### 3.4 `sizeof` and alignment: the rules

Every fundamental type has a **size** and an **alignment**. On x86-64 and ARM64:

| Type | Size | Alignment |
|------|------|-----------|
| `uint8_t`, `bool`, `enum : uint8_t` | 1 | 1 |
| `uint16_t` | 2 | 2 |
| `uint32_t`, `float` | 4 | 4 |
| `uint64_t`, `double`, pointers | 8 | 8 |

**Alignment N** means the object's address must be a multiple of N. A `uint64_t` at address 0x1003 is *misaligned*. On x86 it works but is slower (may straddle two cache lines); on some ARM configurations it crashes with a bus error. Atomics on misaligned addresses are not atomic.

The compiler guarantees alignment by inserting **padding** between struct fields. The rules:

1. Each field is placed at the next offset that is a multiple of its alignment.
2. The struct's own alignment is the maximum alignment of any field.
3. The struct's total size is rounded up to a multiple of its alignment (so arrays of it stay aligned).

**Worked example:**

```cpp
struct Bad {
    uint8_t  a;    // offset 0, size 1
                   // pad 7 bytes so next field is 8-aligned
    uint64_t b;    // offset 8, size 8
    uint8_t  c;    // offset 16, size 1
                   // pad 7 bytes so total is a multiple of 8
};                 // sizeof(Bad) == 24

struct Good {
    uint64_t b;    // offset 0
    uint8_t  a;    // offset 8
    uint8_t  c;    // offset 9
                   // pad 6 bytes
};                 // sizeof(Good) == 16
```

Same fields, 33% smaller — just by ordering large-to-small. **Rule of thumb: sort fields by alignment, descending, unless you have a cache-line reason not to.**

### 3.5 The `Order` layout, computed by hand

```cpp
struct alignas(CACHE_LINE) Order {
    Price     price;        // uint32_t  offset 0,  size 4
    Quantity  quantity;     // uint32_t  offset 4,  size 4
    Quantity  filled_qty;   // uint32_t  offset 8,  size 4
    Side      side;         // uint8_t   offset 12, size 1
    OrderType type;         // uint8_t   offset 13, size 1
                            //           pad 2 → next is 8-aligned at 16
    Order*    prev;         // pointer   offset 16, size 8
    Order*    next;         // pointer   offset 24, size 8
    OrderId   id;           // uint64_t  offset 32, size 8
    ClientId  client_id;    // uint32_t  offset 40, size 4
                            //           pad 4 → next is 8-aligned at 48
    Timestamp timestamp;    // uint64_t  offset 48, size 8
                            //           data ends at 56
                            //           pad 8 → total is multiple of 64
};                          // sizeof(Order) == 64
```

Notice the order violates "largest first". That's deliberate, and it's the subject of Chapter 7: the fields checked on every matching step (`price`, `quantity`, `filled_qty`, `side`) are packed into the first 16 bytes so a single cache-line fetch delivers all of them, and the CPU's prefetcher gets the rest of the line for free.

### 3.6 `alignas`

```cpp
struct alignas(64) Order { ... };
alignas(64) size_t cached_tail_ = 0;
```

`alignas(N)` *raises* a type's or variable's alignment to N (it can never lower it). Consequences:

- `sizeof` is rounded up to a multiple of N.
- Every instance — on the stack, in an array, in a `vector` — sits at an address that's a multiple of N.

With N = 64 (the cache line size on every x86 and most ARM chips), each `Order` occupies exactly one cache line and never straddles two. An array of `Order` has one order per line. This is the foundation of the false-sharing discussion in Chapter 8.

`CACHE_LINE` is a `constexpr size_t`, so `alignas(CACHE_LINE)` is legal (it needs a compile-time constant).

### 3.7 `#pragma pack` — the opposite of alignment

```cpp
#pragma pack(push, 1)
struct WireMessage {
    uint8_t  type;       // offset 0
    uint8_t  side;       // offset 1
    uint8_t  order_type; // offset 2
    uint8_t  pad;        // offset 3
    uint32_t client_id;  // offset 4
    uint64_t order_id;   // offset 8
    uint32_t price;      // offset 16
    uint32_t quantity;   // offset 20
    uint64_t timestamp;  // offset 24
};                       // sizeof == 32, exactly
#pragma pack(pop)
```

`#pragma pack(push, 1)` saves the current packing setting and sets the maximum alignment to 1 — meaning *no padding at all*. `pop` restores it.

Why: this struct is sent over TCP. The receiver may be a different program in a different language. Both sides must agree on the byte-for-byte layout. Compiler padding is implementation-defined, so we eliminate it and define the layout by hand.

The cost: fields may be misaligned. `order_id` at offset 8 happens to be aligned here (we laid it out carefully), but in general a packed struct can put a `uint64_t` at offset 3. Reading that on x86 is slower; on strict-alignment ARM it can fault. That's why the gateway does:

```cpp
WireMessage wire;
std::memcpy(&wire, c.read_buf, WIRE_MSG_SIZE);
```

`memcpy` from the byte buffer into a properly-aligned local — it's the portable way to reinterpret bytes, and the compiler turns it into a few aligned loads.

**Endianness note**: this wire format sends integers in *host byte order*. x86 and Apple/Linux ARM64 are all little-endian, so it works. A production protocol would specify byte order explicitly (ITCH is big-endian; SBE is little-endian) and convert with `htonl`/`ntohl` or `std::byteswap`.

### 3.8 The `static_assert` guard

```cpp
static_assert(sizeof(WireMessage) == 32, "...");
```

The manual layout above is fragile — someone adds a field and the protocol silently changes. This line makes the build fail instead.

### 3.9 Constructors and member initialiser lists

```cpp
OrderBook::OrderBook(size_t pool_capacity)
    : bids_(new PriceLevel[PRICE_LEVELS]())
    , asks_(new PriceLevel[PRICE_LEVELS]())
    , order_table_(new Order*[MAX_ORDERS]())
    , pool_(pool_capacity) {
    fills_.reserve(64);
}
```

The part after the colon is the **member initialiser list**. Members are constructed *here*, before the body `{ }` runs, in the order they are *declared in the class* (not the order written in the list — a classic gotcha).

Why not assign in the body? Because members are constructed before the body runs regardless. `pool_ = OrderPool(cap);` in the body would first default-construct `pool_` (impossible — it has no default constructor) and then assign. The initialiser list constructs it once, correctly.

`new PriceLevel[PRICE_LEVELS]()` — the trailing `()` **value-initialises** every element (zeroes the pointers and counters). Without `()`, primitive members would be garbage.

`explicit OrderBook(size_t pool_capacity = MAX_ORDERS);` — `explicit` prevents the compiler from using this constructor for implicit conversions. Without it, `OrderBook b = 5;` would compile (converting the integer 5 to an `OrderBook` with a 5-element pool). `explicit` on any single-argument constructor is a standard defensive habit.

### 3.10 Destructors and RAII

```cpp
~Gateway() {
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients_[i].active) close(clients_[i].fd);
    if (listen_fd_ >= 0) close(listen_fd_);
    if (event_fd_ >= 0) close(event_fd_);
}
```

A destructor runs automatically when the object goes out of scope or is deleted. This is **RAII** (Resource Acquisition Is Initialisation): acquire a resource in the constructor, release it in the destructor, and you can never leak — even if an exception is thrown or an early `return` is hit.

File descriptors (sockets) are OS resources. If the `Gateway` is destroyed, its sockets are closed. No manual cleanup at every exit path.

`std::unique_ptr`, `std::vector`, and `std::thread` all use RAII internally. That's why `OrderBook` needs no destructor: its `unique_ptr` members free their arrays automatically.

### Interview Questions

**Q: Why is `sizeof(Order)` 64 when the fields add up to 56?**
A: `alignas(64)` rounds the size up to a multiple of 64 so each order occupies exactly one cache line. The 8 bytes are padding.

**Q: What does `#pragma pack(1)` do, and what's the risk?**
A: Removes all padding so the layout is byte-exact for wire transmission. Risk: misaligned field access, which is slow on x86 and can fault on strict-alignment architectures. Mitigate by copying into an aligned struct with `memcpy` before use.

**Q: Why order fields large-to-small?**
A: Minimises padding. Exception: put fields accessed together in the same cache line, even if it costs padding.

**Q: What's a member initialiser list and why use it?**
A: Constructs members directly instead of default-constructing then assigning. Required for `const` members, references, and types without default constructors. Members initialise in declaration order.

### Exercises

1. Write a program that prints `sizeof` and `offsetof` (from `<cstddef>`) for every field in `Order`. Verify the table in 3.5.
2. Reorder `Order`'s fields to minimise size without `alignas`. What's the smallest you can get? Now add `alignas(64)` back — does size change?
3. Remove `#pragma pack` from `WireMessage` and observe the `static_assert` fire. Compute by hand what the padded size would be.
4. Add a `char name[8]` field to `WireMessage` and re-layout it so the total is exactly 40 bytes with no padding.

---

## Chapter 4: Pointers, Memory, and the STL Containers We Use

### 4.1 Stack vs heap

Every thread has a **stack**: a contiguous region (8MB by default on Linux/macOS) where local variables live. Allocation is a single instruction (bump the stack pointer). Deallocation is free (pop). Extremely fast, but limited in size.

The **heap** is everything else. `new`, `malloc`, `std::vector` get memory from here. Allocation involves the allocator searching for a free block (tens of nanoseconds to microseconds). Unlimited in size (up to RAM + swap).

```cpp
void f() {
    int x = 5;                    // stack: 4 bytes, gone when f returns
    int* p = new int(5);          // heap: 4 bytes, lives until delete p
    std::vector<int> v(1000);     // v itself (24 bytes) on stack; its 4000 bytes of ints on heap
}
```

**The bug we hit while building TradeFeed**: `OrderBook` originally declared

```cpp
std::array<PriceLevel, PRICE_LEVELS> bids_;   // 1,000,000 × 24 bytes = 24MB
std::array<PriceLevel, PRICE_LEVELS> asks_;   // another 24MB
std::array<Order*, MAX_ORDERS> order_table_;  // 8MB
```

`std::array` stores its elements *inline* — inside the object itself. So `sizeof(OrderBook)` was 56MB, and `OrderBook book;` on the stack in the benchmark tried to allocate 56MB on an 8MB stack. Segfault at exit code 139 (128 + SIGSEGV 11). Running under AddressSanitizer gave the exact diagnosis: `stack-overflow`.

The fix: heap-allocate the big arrays and keep only pointers in the object:

```cpp
std::unique_ptr<PriceLevel[]> bids_;    // 8 bytes on stack; 24MB on heap
```

Same happened with the benchmark's `InboundRing in;` — a `SPSCRing<InboundMessage, 262144>` holds its 16MB buffer inline. Fix: `static InboundRing in;` (statics live in the data segment, not the stack). In `main.cpp` the rings are `static` for the same reason.

**Lesson**: big fixed-size objects go on the heap (via `unique_ptr`/`vector`) or in static storage. The stack is for small, short-lived things.

### 4.2 Raw pointers

```cpp
Order* head_ = nullptr;
```

A pointer is a 64-bit integer holding a memory address. `Order*` is "address of an Order". `nullptr` is the typed null (prefer it over `0` or `NULL`).

Operations: `*p` dereferences (gets the Order), `p->price` accesses a member through the pointer, `&o` takes an address.

TradeFeed uses raw pointers for **non-owning references**: `PriceLevel::head_` points at an `Order` that the `OrderPool` owns. The list does not allocate or free orders — it just links existing ones. This is the correct modern-C++ use of raw pointers: "I'm looking at this, I don't own it."

Ownership questions: who frees it? For every `new`, exactly one `delete` must run. Getting this wrong gives leaks (never freed), double-frees (crash), or use-after-free (silent corruption). The STL types below exist to make ownership automatic.

### 4.3 `std::unique_ptr`

```cpp
#include <memory>
std::unique_ptr<PriceLevel[]> bids_;
bids_(new PriceLevel[PRICE_LEVELS]())     // in initialiser list
bids_[idx(price)]                          // array subscript works
```

A `unique_ptr<T>` is a raw pointer with one addition: its destructor calls `delete`. `unique_ptr<T[]>` is the array form: its destructor calls `delete[]` and it supports `operator[]`.

Properties:
- **Zero overhead**: `sizeof(unique_ptr<T>) == sizeof(T*)`. Dereference compiles to the same instruction as a raw pointer.
- **Sole ownership**: cannot be copied (compile error), only *moved*. Exactly one owner at all times, so exactly one `delete`.
- **Exception safe**: if the constructor throws after allocating `bids_`, `bids_` is freed automatically.

The generic pattern:

```cpp
{
    std::unique_ptr<Widget> w = std::make_unique<Widget>(args);
    w->do_thing();
}   // w destroyed here; Widget deleted. No delete statement anywhere.
```

TradeFeed uses `new T[N]()` instead of `std::make_unique<T[]>(N)` for one reason: `make_unique<T[]>` value-initialises (same thing), but the explicit form makes it obvious that the trailing `()` zeroes the memory. Either is fine.

When you'd use `std::shared_ptr` instead: when multiple owners genuinely need the object to outlive any one of them (reference-counted). It costs an atomic increment/decrement per copy — never on a hot path. TradeFeed has no shared ownership, so no `shared_ptr`.

### 4.4 `std::vector` — how it actually works

```cpp
#include <vector>
std::vector<Order> arena_;
std::vector<Order*> free_list_;
std::vector<Fill> fills_;
```

A vector is three pointers:

```
struct vector<T> {
    T* begin;     // start of heap allocation
    T* end;       // one past last element (size = end - begin)
    T* cap_end;   // one past end of allocation (capacity = cap_end - begin)
};
```

`sizeof(std::vector<anything>) == 24` on 64-bit. The elements are on the heap, contiguous.

**`push_back`**: if `end < cap_end`, construct the element at `end` and increment — O(1), no allocation. If `end == cap_end` (full), allocate a new block of *double* the capacity (libstdc++ and libc++ use 2x; MSVC 1.5x), move all elements over, free the old block, then push. That's O(n) — but it happens only every 2x growth, so *amortised* O(1).

**`reserve(n)`**: pre-allocate capacity for `n` elements. Subsequent `push_back`s up to `n` never allocate. This is how TradeFeed makes vector operations allocation-free on the hot path:

```cpp
free_list_.reserve(capacity);   // OrderPool constructor: never reallocates after this
fills_.reserve(64);             // OrderBook: 64 fills per order is ample; if exceeded, one reallocation
```

**`pop_back`**: decrement `end`, destroy the element. O(1). Never shrinks the allocation.

**`back()`**: `*(end - 1)`. O(1).

**`vector(n)`** (the constructor with a count): allocates `n` elements and value-initialises them. `std::vector<Order> arena_(capacity)` gives 1M default-constructed orders in a contiguous 64MB block.

**Why not `std::list` or `std::deque` for the free list?** A vector used as a stack (`push_back`/`pop_back`/`back`) is the fastest possible LIFO: contiguous memory, no per-node allocation, no pointer chasing. `std::list` allocates per node; `std::deque` has a two-level indirection.

**Why not `std::stack`?** `std::stack` is an adapter over `std::deque` by default. `std::stack<T, std::vector<T>>` is equivalent to what we do, but the raw vector is more transparent and gives us `reserve`.

### 4.5 `std::array`

```cpp
#include <array>
std::array<ClientState, MAX_CLIENTS> clients_{};
std::array<double, VPIN_BUCKETS> vpin_history_{};
```

A fixed-size array with elements stored *inline* (not on the heap). `sizeof(std::array<T, N>) == N * sizeof(T)` — it *is* the elements. It's a thin wrapper over `T data[N]` that adds `.size()`, iterators, and bounds-checked `.at()`.

Use when: size is known at compile time and small enough to live inline. `clients_` is 64 × ~56 bytes ≈ 3.5KB — fine inside a `Gateway` on the stack. `bids_` at 24MB is not — hence the switch to `unique_ptr<T[]>` (Section 4.1).

The trailing `{}` value-initialises all elements (zeroes them). Without it, a `std::array` of PODs has garbage contents.

### 4.6 `std::atomic`

```cpp
#include <atomic>
std::atomic<size_t> val{0};
std::atomic<bool> g_running{true};
```

An atomic is a value that can be read and written by multiple threads without a data race. Every operation is indivisible — no thread ever sees a half-written value. Covered in depth in Chapter 17. For now: `std::atomic<size_t>` is 8 bytes, and `load`/`store` on x86 compile to ordinary `mov` instructions plus optional fences depending on the memory order.

### 4.7 `std::thread`

```cpp
#include <thread>
std::thread engine_thread([&] { engine.run(g_running); });
engine_thread.join();
```

Constructing a `std::thread` with a callable starts a new OS thread running that callable immediately. `join()` blocks until the thread finishes. A `std::thread` that is destroyed without `join()` or `detach()` calls `std::terminate` — the standard forces you to decide.

`[&] { ... }` is a **lambda** — an anonymous function. `[&]` means "capture every outer variable I mention *by reference*." So `engine` and `g_running` inside the lambda refer to the real objects in `main`. Safe here because `main` outlives both threads (it joins them before returning). Capturing by reference something that dies before the thread finishes is a use-after-free — a very common bug.

`[=]` would capture by value (copies). `[&engine, &g_running]` captures explicitly. Explicit is clearer for large scopes; `[&]` is fine in a 3-line lambda.

### 4.8 The algorithms and utilities used

```cpp
#include <algorithm>
std::min(a, b)                     // smaller of two; used in drain_level for fill quantity
std::sort(v.begin(), v.end())      // introsort, O(n log n); used to compute percentiles
std::shuffle(v.begin(), v.end(), rng)   // Fisher-Yates with a given RNG

#include <numeric>
std::iota(v.begin(), v.end(), 1)   // fill with 1, 2, 3, ...

#include <random>
std::mt19937 rng(42);                                  // Mersenne Twister, seeded 42 (reproducible)
std::uniform_int_distribution<Price> dist(lo, hi);     // maps rng output to [lo, hi] inclusive
dist(rng)                                              // draw one value

#include <chrono>
std::chrono::steady_clock::now()                       // monotonic wall clock (for TSC calibration)
std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()

#include <cstring>
std::memcpy(dst, src, n)           // copy n bytes; the compiler inlines it for small constant n

#include <cassert>
assert(cond)                       // in debug builds: abort if false. In -DNDEBUG builds: compiled out entirely.
```

`assert` is how TradeFeed checks invariants without paying for them in release. `OrderPool::allocate` asserts the free list is non-empty; `add_order` already checked `pool_.available() == 0` before calling, so in release the assert is dead code — zero cost.

`std::min` is a template; `std::min(aggressor->remaining(), resting->remaining())` deduces `Quantity` and compiles to a compare-and-conditional-move — no branch.

### Interview Questions

**Q: Why did `OrderBook` on the stack crash?**
A: `std::array` stores elements inline, so the object was 56MB — bigger than the 8MB default stack. Fixed by heap-allocating through `unique_ptr<T[]>`, leaving only 24 bytes of pointers in the object.

**Q: How does `std::vector::push_back` achieve O(1)?**
A: Amortised: it doubles capacity on overflow, so reallocations happen O(log n) times over n pushes. With `reserve`, it never reallocates.

**Q: When would you use `unique_ptr` vs `shared_ptr` vs raw pointer?**
A: `unique_ptr` for sole ownership (zero overhead). `shared_ptr` for genuinely shared lifetime (atomic refcount — not on hot paths). Raw pointer for non-owning observation, e.g. intrusive list links.

**Q: What does `[&]` in a lambda mean and what's the danger?**
A: Capture all referenced outer variables by reference. Danger: the lambda outlives the variables (dangling reference). Safe in `main.cpp` because the threads are joined before `main` returns.

### Exercises

1. Write a program that `push_back`s 1M ints into a vector and prints `capacity()` every time it changes. Confirm the growth factor.
2. Time the same loop with and without `reserve(1'000'000)` first.
3. Replace `std::unique_ptr<PriceLevel[]>` with a raw `PriceLevel*` and add the correct destructor. Then introduce an early `return` in the constructor and explain the leak.
4. Write a lambda that captures a local by reference, return it from a function, and call it. Run under AddressSanitizer (`-fsanitize=address`). Read the report.

---

## Chapter 5: Templates

### 5.1 What a template is

A template is a recipe for generating code. The compiler stamps out a concrete class or function each time you use it with new arguments.

```cpp
template <typename T>
T twice(T x) { return x + x; }

twice(3);      // compiler generates: int twice(int x) { return x + x; }
twice(2.5);    // compiler generates: double twice(double x) { return x + x; }
```

Each *instantiation* is a separate, fully-optimised function. There's no runtime dispatch — it's as fast as if you'd written the concrete version by hand. This is what makes `std::vector<Order>` exactly as fast as a hand-rolled `Order` array.

### 5.2 Non-type template parameters

```cpp
template <typename T, size_t Capacity>
class SPSCRing { ... };

SPSCRing<InboundMessage, 262144> ring;
```

`Capacity` is a *value* parameter, not a type. It's a compile-time constant inside the class. Consequences:

- `T buffer_[Capacity];` is a real fixed-size array — no heap allocation, no pointer indirection.
- `static constexpr size_t MASK = Capacity - 1;` is computed at compile time; `head & MASK` compiles to an `AND` with an immediate operand.
- `static_assert((Capacity & (Capacity - 1)) == 0, ...)` checks the power-of-2 requirement at compile time. Instantiating `SPSCRing<T, 1000>` is a *compile error*, not a runtime bug.

The alternative — `SPSCRing(size_t capacity)` as a constructor argument — would need a heap-allocated buffer and a runtime mask. Slightly slower, and the power-of-2 check would be a runtime assert. Since ring sizes never change at runtime, the template version is strictly better.

### 5.3 Why templates must live in headers

The compiler needs the full template definition to instantiate it. If `SPSCRing`'s methods were in a `.cpp` file, then `matching_engine.cpp` (which includes only the header) couldn't generate `SPSCRing<InboundMessage, 262144>::push` — link error: *undefined symbol*. So templates are header-only by necessity, which also gives us the inlining benefit from Chapter 1.

### 5.4 Type aliases for template instantiations

```cpp
using InboundRing  = SPSCRing<InboundMessage,  RING_SIZE>;
using OutboundRing = SPSCRing<OutboundMessage, RING_SIZE>;
```

The full type name is long and appears in several files. An alias makes it one word. Both `matching_engine.h` and `socket_transport.h` define these aliases identically — that's legal (an alias is a declaration, not a definition).

### Interview Questions

**Q: Why is `Capacity` a template parameter and not a constructor argument?**
A: Compile-time constant → inline array (no heap), mask as an immediate, power-of-2 checked by `static_assert`. Ring size is fixed for the process lifetime, so there's no reason to pay for runtime flexibility.

**Q: Why can't you put a template's implementation in a `.cpp`?**
A: The compiler must see the full definition to instantiate it for each set of arguments. Only the translation unit that sees the definition can generate the code.

### Exercises

1. Instantiate `SPSCRing<int, 1000>`. Read the compile error.
2. Write a `template <typename T, size_t N> class FixedStack` with `push/pop/top/empty` and a `static_assert(N > 0)`. Use it as the free list in a copy of `OrderPool`. Benchmark against the vector version.
3. Make `SPSCRing` take capacity as a runtime constructor argument. Benchmark push/pop against the template version. Look at the generated assembly (`-S`) for the mask operation in each.

---

# PART II — THE HARDWARE

Part I taught you what the code *says*. Part II teaches you what the machine *does*. Every performance decision in TradeFeed comes from this part.

---

## Chapter 6: The Memory Hierarchy

### 6.1 The fundamental problem

A modern CPU core executes roughly 4 instructions per clock cycle at ~3–5 GHz. That's about one instruction every 0.07 nanoseconds.

Main memory (DRAM) takes about 80 nanoseconds to respond.

If every instruction needed a value from DRAM, the CPU would idle for ~1,100 instruction slots per memory access. The entire architecture of modern computers is built to hide this gap.

### 6.2 The hierarchy

```
        ┌──────────────┐
        │  Registers   │   ~16-32 × 8 bytes      0 cycles    (part of the instruction)
        ├──────────────┤
        │   L1 Data    │   32-48 KB              ~4 cycles   ~1 ns
        ├──────────────┤
        │      L2      │   512 KB - 2 MB         ~14 cycles  ~4 ns
        ├──────────────┤
        │   L3 (LLC)   │   8-64 MB (shared)      ~40 cycles  ~12 ns
        ├──────────────┤
        │     DRAM     │   8-512 GB              ~240 cycles ~80 ns
        ├──────────────┤
        │  NVMe SSD    │   1-8 TB                            ~100,000 ns
        └──────────────┘
```

For the i9-10900K (the deployment target):
- L1d: 32 KB per core
- L2: 256 KB per core
- L3: 20 MB shared across 10 cores
- DRAM: 66 GB

Each level is roughly 4x slower and 10x bigger than the one above it.

**The mental model you need**: an L1 hit is essentially free. An L3 hit costs you ~40 instruction slots. A DRAM miss costs you ~240 instruction slots. A single cache miss on the hot path can dwarf all the arithmetic you were trying to optimise.

### 6.3 Cache lines

The cache does not store individual bytes. It stores **cache lines** — fixed-size blocks, 64 bytes on every x86-64 CPU and on Apple Silicon (some ARM server chips use 128).

When you read one byte that isn't cached, the CPU fetches the entire 64-byte line containing it.

```cpp
int arr[16];        // 64 bytes = exactly one cache line
int x = arr[0];     // MISS: fetches all 64 bytes (arr[0]..arr[15])
int y = arr[7];     // HIT: already in L1, free
```

This has an enormous consequence: **reading 1 byte costs the same as reading 64 bytes**, provided they're in the same line. Data structures that pack related data into the same line get 64 bytes of useful work per miss. Structures that scatter data across memory get 1 useful byte per miss.

### 6.4 Spatial locality: the array vs the linked list

The classic demonstration. Sum 10 million integers, two ways:

```cpp
// Version A: contiguous array
int* arr = new int[10'000'000];
long sum = 0;
for (int i = 0; i < 10'000'000; ++i)
    sum += arr[i];
```

```cpp
// Version B: linked list with the same values, nodes scattered by malloc
struct Node { int value; Node* next; };
long sum = 0;
for (Node* n = head; n; n = n->next)
    sum += n->value;
```

Both are O(n). Both do 10 million additions. Version A is typically **10–30x faster**.

Why:
- **Array**: 16 ints per 64-byte line. One cache miss serves 16 iterations. Plus the hardware prefetcher detects the sequential pattern and loads lines *before* you ask, so most misses are hidden entirely. Effective cost: near zero per element.
- **List**: each `Node` is 16 bytes, but `malloc` scatters them across the heap. Each `n->next` dereference is likely a fresh cache line — and possibly a fresh DRAM page. The prefetcher cannot help: it can't know the next address until the current node is loaded. This is **pointer chasing**, and the misses are *serialised* — you can't start the next fetch until the current one completes.

**This single fact drives most of TradeFeed's data structure choices.** Arrays beat trees. Arrays beat hash maps with chained buckets. Contiguous pools beat scattered allocations.

### 6.5 Temporal locality

Data you used recently is likely still cached. Data you use repeatedly stays in L1.

The matching engine's **working set** — the data it touches on a typical order — is:
- The price levels near the spread (a few dozen `PriceLevel` objects, ~1 KB)
- The head orders at those levels (a few dozen `Order` objects, ~2 KB)
- The order pool's free-list tail (~1 KB)
- The ring buffer head/tail region (~1 KB)

That's roughly 5 KB. It fits in 32 KB of L1 with room to spare. **This is the single most important performance property of the design**: the hot working set is small enough to live in L1 permanently, so the steady-state cost of matching an order is L1 hits only.

The book *as a whole* is 56 MB — far bigger than L3. But we never touch most of it. A price level at $10.00 when the market is at $150.00 is cold, and it staying cold is fine.

### 6.6 The prefetcher

Modern CPUs have hardware prefetchers that detect access patterns and speculatively load lines:
- **Sequential**: `arr[0], arr[1], arr[2]...` → loads ahead.
- **Strided**: `arr[0], arr[8], arr[16]...` → detects constant stride, loads ahead.
- **Pointer chasing**: no pattern → cannot help.

You can also prefetch manually:

```cpp
__builtin_prefetch(addr);        // hint: bring this line into cache, I'll need it soon
__builtin_prefetch(addr, 1);     // write intent
__builtin_prefetch(addr, 0, 3);  // read, high temporal locality
```

This is a *hint* — it issues a load that doesn't block and doesn't fault. Used correctly, it hides a miss by starting the fetch ~50 cycles before you need the data.

TradeFeed does not currently prefetch. The natural place would be in the engine loop: when you pop message N from the ring, prefetch the price level that message N+1 will touch. That's the "1-ahead pipeline" pattern. It's listed as an upgrade path in Chapter 29 — worth mentioning in an interview as "I know where it goes, I haven't measured a need for it yet."

### 6.7 Translation Lookaside Buffer (TLB)

Virtual addresses must be translated to physical addresses. The page table lives in memory; the TLB caches recent translations. A TLB miss costs a page-table walk — potentially several DRAM accesses.

Default page size is 4 KB. A 56 MB order book spans ~14,000 pages. If access were random, TLB misses would be constant. Because access is *clustered* near the spread, only a handful of pages are hot, and they stay in the TLB.

**Huge pages** (2 MB on x86) reduce a 56 MB structure from 14,000 pages to 28. On Linux:

```bash
# Enable transparent huge pages
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
```

Or explicitly with `madvise(ptr, size, MADV_HUGEPAGE)`. This is a standard production tuning step for large in-memory structures. Not applied in TradeFeed yet — another honest "I know the knob exists" answer.

### Interview Questions

**Q: Roughly how long does an L1 hit take vs a DRAM access?**
A: L1 ~1ns (4 cycles), DRAM ~80ns (240 cycles). About 80x, or ~1,100 wasted instruction slots.

**Q: Why is iterating an array faster than a linked list with the same elements?**
A: Spatial locality. An array packs ~16 ints per cache line, so one miss serves 16 iterations, and the prefetcher hides most misses. A linked list scatters nodes, gets one useful value per miss, and the misses are serialised because you can't compute the next address until the current node arrives.

**Q: What's the working set of your matching engine?**
A: ~5 KB — the price levels near the spread, their head orders, the free-list tail, and the ring's head/tail region. It fits in 32 KB L1, so steady-state matching is L1-resident.

### Exercises

1. Write the array-vs-linked-list sum benchmark from 6.4. Measure both. Then allocate the list nodes from a contiguous pool (like `OrderPool`) and measure again — how much of the gap closes?
2. Write a program that reads `arr[i]` for `i` stepping by 1, 16, 64, 256, 1024 over a 64 MB array. Plot time per access against stride. Identify the cache line size and the point where the prefetcher gives up.
3. Use `perf stat -e cache-misses,cache-references ./bench_orderbook` on Linux. What's the miss rate? Now change `PRICE_LEVELS` to 100 (a tiny book) and re-measure.

---

## Chapter 7: Designing Structs for the Cache

### 7.1 The 64-byte budget

Once you accept that memory arrives in 64-byte lines, struct design becomes a budgeting exercise: **which fields do I want in the same line?**

Two competing pressures:
- **Pack tightly** so more objects fit per line (good for scanning many objects).
- **Split hot from cold** so a line contains only fields you actually use (good for touching one field of many objects).

### 7.2 Hot/cold splitting — the generic pattern

Suppose you have a million particles and the physics loop only needs position:

```cpp
// Bad: 64 bytes per particle, but the loop only reads 12 of them
struct Particle {
    float x, y, z;            // 12 bytes — hot, read every frame
    float vx, vy, vz;         // 12 bytes — hot
    char  name[32];           // 32 bytes — cold, read never
    uint64_t id;              // 8 bytes  — cold
};

for (auto& p : particles) p.x += p.vx;   // loads 64 bytes, uses 8
```

One particle per cache line, and 87% of each line is wasted. The fix is either to reorder (put hot fields first, so multiple particles' hot data shares lines — doesn't work here since the struct is already 64B) or to split into parallel arrays:

```cpp
// Structure of Arrays (SoA)
struct Particles {
    std::vector<float> x, y, z, vx, vy, vz;   // hot arrays
    std::vector<std::string> names;            // cold array
};
for (size_t i = 0; i < n; ++i) x[i] += vx[i];  // 16 floats per line, 100% utilised
```

This is **AoS vs SoA** (Array of Structs vs Struct of Arrays). SoA wins when you sweep one field across many objects. AoS wins when you touch many fields of one object.

### 7.3 Why TradeFeed uses AoS

The matching engine's access pattern is: *take one order, look at several of its fields, then follow `next` to the adjacent order*. That's many fields of one object — AoS.

And within AoS, we apply hot/cold field ordering:

```cpp
struct alignas(CACHE_LINE) Order {
    // --- bytes 0-15: HOT. Read on every match attempt. ---
    Price     price;        // 0-3    compared against the aggressor's limit
    Quantity  quantity;     // 4-7    used by remaining()
    Quantity  filled_qty;   // 8-11   used by remaining(), written on fill
    Side      side;         // 12     branch on buy/sell
    OrderType type;         // 13     branch on limit/market
                            // 14-15  padding

    // --- bytes 16-31: WARM. Read when traversing or unlinking. ---
    Order*    prev;         // 16-23
    Order*    next;         // 24-31

    // --- bytes 32-55: COLD. Read only when emitting a fill. ---
    OrderId   id;           // 32-39
    ClientId  client_id;    // 40-43
                            // 44-47  padding
    Timestamp timestamp;    // 48-55
                            // 56-63  padding to 64
};
```

The whole struct is one cache line, so *any* access loads all of it — the ordering doesn't change the miss count for a single order. What it does buy:

1. **The compiler can load hot fields with one 16-byte SIMD load** (`movdqa`) rather than four scattered 4-byte loads.
2. **`remaining()` needs bytes 4–11**, contiguous, one load.
3. **Documentation**: the layout tells the next reader which fields are on the hot path.

If `Order` were larger than 64 bytes (say we added 32 bytes of metadata), the ordering would become critical — hot fields in the first line, cold in the second, and most accesses would touch only the first.

### 7.4 Why exactly one cache line

```cpp
struct alignas(CACHE_LINE) Order { ... };   // sizeof == 64
```

Three benefits:

**1. No straddling.** A 56-byte struct without `alignas` would sometimes span two lines (if it starts at offset 40 of a line, bytes 40–63 are in line N and 64–95 in line N+1). Accessing it costs two misses instead of one. With `alignas(64)` and size 64, every order starts at a line boundary. Access cost is exactly one miss, always — **deterministic latency**, which matters more than average latency for p99.9.

**2. No false sharing.** Two orders never share a line, so two threads writing to adjacent orders never invalidate each other's cache. (TradeFeed's book is single-threaded, so this is insurance rather than a live requirement — but it's free insurance.)

**3. Clean arithmetic.** `arena_[i]` is at `base + i*64` — a shift, not a multiply. The index-to-address computation is one instruction.

The cost is 8 bytes of padding per order — 8 MB across a million orders. Given we have 66 GB of RAM, that's a trivially good trade.

### 7.5 The same logic applied to messages

```cpp
struct alignas(CACHE_LINE) InboundMessage { ... };   // 64 bytes
struct alignas(CACHE_LINE) OutboundMessage { ... };  // 64 bytes
```

These live in the SPSC ring buffer. Each slot is one cache line. When the consumer pops message N, it loads exactly one line. When the producer writes message N+1, it touches a different line — so producer and consumer never contend over the same line even when they're only one slot apart.

Without alignment, a 40-byte message would let slot N and slot N+1 share a line, and the producer writing slot N+1 would invalidate the consumer's copy of slot N. That's false sharing in the ring's data array — subtle, and it shows up as mysterious latency spikes.

### 7.6 The cost of getting it wrong: a measurement

The generic experiment (worth running yourself):

```cpp
struct Unaligned { std::atomic<long> a; std::atomic<long> b; };   // both in one line
struct Aligned   { alignas(64) std::atomic<long> a;
                   alignas(64) std::atomic<long> b; };            // separate lines
```

Two threads: one increments `a` 10M times, the other increments `b` 10M times. They never touch the same variable.

Typical results on an x86 desktop:
- `Unaligned`: ~1.2 seconds
- `Aligned`: ~0.05 seconds

**24x slower for code that has no logical sharing at all.** That's false sharing, and it's the subject of the next chapter.

### Interview Questions

**Q: Why is `Order` exactly 64 bytes?**
A: One cache line. Guarantees a single miss per order access (no straddling), prevents false sharing between adjacent orders, and makes `arena_[i]` a shift rather than a multiply. Costs 8 bytes of padding, which is irrelevant at our scale.

**Q: AoS or SoA — which and why?**
A: AoS here, because the access pattern is "many fields of one order, then follow a pointer to the next order." SoA wins the opposite pattern — sweeping one field across many objects. If I were computing, say, total volume across all resting orders, SoA would be faster.

**Q: Does field ordering matter if the struct is already one cache line?**
A: Not for miss count, but it lets the compiler fuse adjacent hot fields into wider loads, and it documents the hot path. It becomes critical as soon as the struct exceeds 64 bytes.

### Exercises

1. Run the false-sharing measurement in 7.6. Record your numbers.
2. Remove `alignas(CACHE_LINE)` from `Order`. Rebuild, run `bench_orderbook`. Explain any change (or lack of one — and why the book being single-threaded matters).
3. Rewrite `PriceLevel::total_qty()` to compute the sum by walking the list instead of maintaining a counter. Benchmark. Now imagine `Order` were SoA — would the walk be faster?
4. Add `offsetof` prints for every `Order` field and verify Section 7.3's table exactly.

---

## Chapter 8: Cache Coherence and False Sharing

### 8.1 The problem multiple cores create

Each core has its own private L1 and L2. If core 0 and core 1 both cache the same memory, and core 0 writes to it, core 1's copy is now stale. Hardware must fix this transparently — that's **cache coherence**.

### 8.2 MESI

The standard protocol gives every cached line one of four states:

| State | Meaning |
|-------|---------|
| **M**odified | This core has the only copy, and it's been changed. DRAM is stale. |
| **E**xclusive | This core has the only copy, unchanged. |
| **S**hared | Multiple cores have read-only copies. |
| **I**nvalid | This line is not usable. |

The rules that matter:
- To **read** a line, a core needs it in M, E, or S.
- To **write** a line, a core needs it in M or E. If the line is S (other cores have copies), the writer broadcasts an invalidation; every other core drops its copy to I; the writer moves to M.
- If a core needs a line that another core holds in M, the holder must write it back or forward it directly.

**Every write to a shared line causes an invalidation broadcast and forces other cores to re-fetch.** On a modern chip that round trip is ~40–70ns.

### 8.3 False sharing defined

**False sharing** is when two cores write to *different variables* that happen to occupy the *same cache line*. There is no logical sharing — the program is correct either way — but the hardware sees one line being written by two cores and performs the full invalidation dance on every write.

```
Cache line (64 bytes):
┌────────────┬────────────┬─────────────────────────────┐
│ counter_a  │ counter_b  │         unused              │
│ (core 0)   │ (core 1)   │                             │
└────────────┴────────────┴─────────────────────────────┘
      ▲            ▲
      │            │
   core 0       core 1
   writes       writes

Every write by core 0 invalidates core 1's copy of the WHOLE line,
including counter_b, which core 0 never touched.
```

The fix is always the same: **push the variables onto separate cache lines.**

### 8.4 The fix in TradeFeed's ring buffer

```cpp
struct alignas(64) PaddedIndex {
    std::atomic<size_t> val{0};
};

PaddedIndex head_;    // producer writes this
PaddedIndex tail_;    // consumer writes this
```

`head_` is written only by the producer (the gateway thread). `tail_` is written only by the consumer (the engine thread). They are logically independent. Without `alignas(64)`, both 8-byte atomics would sit in the same 64-byte line, and every push would invalidate the consumer's line and vice versa — the 24x penalty from Section 7.6, on the hottest path in the system.

With the padding, each index owns a line. The producer writes its line; the consumer never has that line in S state during steady operation, so there's nothing to invalidate.

### 8.5 The cached-index optimisation

Padding fixes the write-write contention. There's a second problem: **read-write contention**.

To check whether the ring is full, the producer needs `tail_`:

```cpp
if (head - tail_.val.load(acquire) >= Capacity) return false;   // naive
```

Reading `tail_` pulls the consumer's line into the producer's cache in S state. Then the consumer's next `pop` — which writes `tail_` — must invalidate the producer's copy. So even though the producer only *reads*, it forces an invalidation on the consumer's next write. The line ping-pongs between cores.

The fix:

```cpp
alignas(64) size_t cached_tail_ = 0;   // producer-private, plain (non-atomic) variable
alignas(64) size_t cached_head_ = 0;   // consumer-private

bool push(const T& item) {
    const size_t head = head_.val.load(std::memory_order_relaxed);

    if (head - cached_tail_ >= Capacity) {          // check against stale copy first
        cached_tail_ = tail_.val.load(std::memory_order_acquire);  // only now touch shared state
        if (head - cached_tail_ >= Capacity)
            return false;                            // genuinely full
    }

    buffer_[head & MASK] = item;
    head_.val.store(head + 1, std::memory_order_release);
    return true;
}
```

The reasoning: `cached_tail_` is a *lower bound* on the true tail. The consumer only ever advances `tail_`, so if `head - cached_tail_ < Capacity`, then `head - true_tail <= head - cached_tail_ < Capacity` — there is definitely room. The check is conservative: it can only be wrong in the safe direction (claiming full when there's actually room), and when that happens we reload and get the truth.

**Effect**: in normal operation (the ring is rarely near full), the producer never reads `tail_` at all. It touches only its own lines — `head_`, `cached_tail_`, and the slot it's writing. Zero cross-core traffic per push. That's how the burst benchmark reaches 2.5 ns/op.

The consumer's `cached_head_` works symmetrically as an *upper bound check for emptiness*.

### 8.6 The data array needs the same treatment

```cpp
alignas(64) T buffer_[Capacity];
```

Combined with `alignas(CACHE_LINE)` on the message types themselves (Chapter 7.5), every slot is a whole number of cache lines and slot boundaries coincide with line boundaries. The producer writing slot N and the consumer reading slot N−1 touch different lines.

Without this, a 40-byte message would let slots share lines and reintroduce false sharing on the data path — the exact thing the index padding was meant to eliminate.

### 8.7 How to detect false sharing in the wild

On Linux:

```bash
perf c2c record ./your_program
perf c2c report
```

`perf c2c` (cache-to-cache) reports HITM events — "hit in modified state in another core's cache" — which is the signature of a contended line. It shows you the offending cache line address and the source lines that touched it.

Also useful:

```bash
perf stat -e cache-misses,cache-references,LLC-load-misses ./bench_ring
```

A high LLC-load-miss rate on a working set that should fit in L1 is a red flag for coherence traffic.

### Interview Questions

**Q: What is false sharing and how do you fix it?**
A: Two cores writing different variables that share a cache line. The coherence protocol invalidates the whole line on every write, so the line ping-pongs between cores at ~40–70ns per bounce, despite there being no logical sharing. Fix: pad/align the variables onto separate lines with `alignas(64)`.

**Q: Your ring has padded indices. Why also cache the counterpart index?**
A: Padding stops write-write contention, but the producer still *reads* the consumer's `tail_`, which pulls the line into shared state and forces an invalidation on the consumer's next write. Caching a stale lower-bound copy lets the producer skip that read entirely in the common case. It's conservative — it can only under-estimate available space, never over-estimate — so it's always safe.

**Q: How would you prove false sharing is happening?**
A: `perf c2c` on Linux shows HITM events and the contended line. Or ablate: add padding and measure. A 10x+ improvement from padding alone is diagnostic.

### Exercises

1. Build the two-thread counter benchmark from 7.6 with and without padding. Confirm the order-of-magnitude gap on your machine.
2. Remove `alignas(64)` from `PaddedIndex` in `spsc_ring.h`. Run `bench_ring`'s cross-core tests. Record the slowdown.
3. Remove `alignas(CACHE_LINE)` from `InboundMessage` (making it ~40 bytes) and re-run the cross-core throughput test. Explain the result in terms of slots per line.
4. On Linux, run `perf c2c record ./bench_ring && perf c2c report` on both the padded and unpadded versions. Find the contended line in the unpadded report.

---

## Chapter 9: Branches, Pipelines, and Predictability

### 9.1 The pipeline

A CPU doesn't execute one instruction at a time. It has a pipeline ~15–20 stages deep (fetch, decode, rename, schedule, execute, retire), and it keeps dozens of instructions in flight simultaneously. A Skylake-derived core like the i9-10900K can have over 200 instructions in flight.

To keep the pipeline full, the CPU must know what instruction comes next. At a conditional branch, it doesn't — the condition may depend on a value still being computed.

### 9.2 Branch prediction

Rather than stall, the CPU **guesses** and executes speculatively. Modern predictors use branch history tables and are extremely good — 95–99% accurate on typical code.

- **Correct prediction**: essentially free. The pipeline never stalls.
- **Misprediction**: the CPU must discard all speculative work and refill the pipeline. **~15–20 cycles, roughly 5 ns.**

A mispredict costs about as much as an L2 hit. In a 50ns hot path, a few mispredicts are a significant fraction.

### 9.3 Predictable vs unpredictable branches — the generic example

```cpp
std::vector<int> data(100'000);
// fill with random values 0..255

long sum = 0;
for (int i = 0; i < 100'000; ++i)
    if (data[i] >= 128)      // unpredictable: ~50/50 random
        sum += data[i];
```

Now sort `data` first and run the identical loop. The branch becomes: false, false, ..., false, true, true, ..., true — one mispredict total instead of ~50,000.

**Typical result: the sorted version runs 3–6x faster.** Same instructions, same data, same O(n). The only difference is predictability. (This is the famous "why is processing a sorted array faster" Stack Overflow question — worth knowing by name.)

### 9.4 Branchless alternatives

Sometimes you can eliminate the branch entirely:

```cpp
// Branchy
if (a > b) max = a; else max = b;

// Branchless — compiles to CMOV (conditional move), no prediction needed
max = (a > b) ? a : b;
```

`std::min` and `std::max` compile to `CMOV` on x86 at `-O2`+. TradeFeed relies on this:

```cpp
Quantity fill_q = std::min(aggressor->remaining(), resting->remaining());
```

This is on the hottest path in the system — executed once per fill. If it were a branch, it would be genuinely unpredictable (whether the aggressor or the resting order is larger is essentially random), so ~50% mispredict rate. As a `CMOV` it's one instruction with no speculation.

**When branchless loses**: `CMOV` always evaluates both sides and has a data dependency on the condition, so it can't be speculated past. If one branch is taken 99% of the time, the predictor is nearly free and beats `CMOV`. Rule: branchless wins on unpredictable conditions, loses on predictable ones.

### 9.5 The branches in TradeFeed's hot path

Walk `drain_level`:

```cpp
while (!level.empty() && !aggressor->is_filled()) {   // loop condition
    Order* resting  = level.front();
    Quantity fill_q = std::min(...);                   // branchless (CMOV)

    aggressor->fill(fill_q);
    resting->fill(fill_q);
    level.adjust_qty(-static_cast<int32_t>(fill_q));

    if (aggressor_side == Side::Buy)                   // highly predictable
        fills_.push_back({...});
    else
        fills_.push_back({...});
    ++match_count_;

    if (resting->is_filled()) {                        // predictable-ish
        level.remove(resting);
        unreg(resting);
        pool_.deallocate(resting);
    }
}
```

- **`aggressor_side == Side::Buy`**: constant for the entire loop. The predictor learns it after one iteration; after that it's free. (A compiler with profile data could hoist it out of the loop entirely — that's what `-fprofile-use` does.)
- **`resting->is_filled()`**: true when the aggressor is large enough to consume the resting order. In a sweeping market order this is true repeatedly (predictable). In a small aggressor it's false once and the loop exits. Reasonably predictable either way.
- **Loop condition**: predicted taken until the last iteration. One mispredict per `drain_level` call.

The structure is deliberately simple: no virtual calls, no function pointers, no indirect branches the predictor can't handle.

### 9.6 What we avoid: virtual dispatch

```cpp
// NOT in TradeFeed — shown as the anti-pattern
class OrderHandler {
    virtual void handle(Order&) = 0;
};
handler->handle(order);   // indirect call through vtable
```

A virtual call is an indirect branch: load the vtable pointer, load the function pointer, jump. The CPU has an indirect branch predictor, but it's weaker than the conditional predictor, and a mispredicted indirect call costs the full ~20 cycles. Worse, it's an optimisation barrier — the compiler can't inline through it, so no cross-function optimisation.

TradeFeed uses a `switch` on an enum instead:

```cpp
switch (msg.type) {
case MessageType::NewOrder:    ...
case MessageType::CancelOrder: ...
case MessageType::ModifyOrder: ...
}
```

The compiler turns a dense enum switch into a jump table (one indirect jump) or, for few cases, a chain of predictable compares. Either way it's cheaper than a vtable and fully inlinable.

**The general principle**: polymorphism costs latency. Use it in configuration and setup code; avoid it in the inner loop. This is why HFT codebases look "less object-oriented" than typical C++ — it's a deliberate trade.

### 9.7 `[[likely]]` and `[[unlikely]]`

C++20 lets you hint the predictor / code layout:

```cpp
if (pool_.available() == 0) [[unlikely]] return false;
```

This tells the compiler to lay out the unlikely branch out-of-line (off the hot instruction-cache path), keeping the common path dense. It doesn't change the hardware predictor, it changes code layout — which affects instruction cache pressure.

TradeFeed doesn't use these yet. They're a legitimate micro-optimisation once you've measured which branches matter; sprinkling them without profile data is cargo-culting.

### Interview Questions

**Q: What does a branch mispredict cost?**
A: ~15–20 cycles (~5 ns) to flush and refill the pipeline. Comparable to an L2 hit.

**Q: Why is `std::min` better than an if/else here?**
A: It compiles to `CMOV`. Whether the aggressor or the resting order is larger is unpredictable (~50/50), so a branch would mispredict half the time. `CMOV` has no speculation to get wrong. Branchless wins precisely because the condition is unpredictable — for a 99%-taken branch, the predictor would be cheaper.

**Q: Why no virtual functions on the hot path?**
A: Indirect calls use a weaker predictor, cost a full pipeline flush when mispredicted, and block inlining and cross-function optimisation. A `switch` on an enum gives the same dispatch with a jump table and stays inlinable.

### Exercises

1. Implement the sorted-vs-unsorted branch benchmark from 9.3. Measure both. Then rewrite the conditional sum branchlessly (`sum += data[i] * (data[i] >= 128)`) and measure again.
2. Replace `std::min` in `drain_level` with an explicit `if/else`. Check the assembly (`-S -masm=intel`) for `cmov` vs `jle`. Benchmark.
3. Add a `virtual` base class to `OrderBook` with `add_order` virtual, call through a base pointer in the benchmark, and measure the cost.
4. On Linux: `perf stat -e branches,branch-misses ./bench_orderbook`. What's the mispredict rate? Which loop do you think dominates?

---

## Chapter 10: Measuring Time Correctly

You cannot optimise what you cannot measure, and at the nanosecond scale most measurement tools are themselves the bottleneck.

### 10.1 Why not `std::chrono`

```cpp
auto t0 = std::chrono::steady_clock::now();
foo();
auto t1 = std::chrono::steady_clock::now();
```

`steady_clock::now()` is a function call that goes through the vDSO (a shared page the kernel maps into every process so clock reads avoid a full syscall). Cost: **~20–25 ns per call**, sometimes more.

If `foo()` takes 50 ns, you're adding 40–50 ns of measurement overhead to a 50 ns signal. The measurement dominates the thing being measured.

`std::chrono` is correct and portable and you should use it for anything above ~1 microsecond. Below that, you need the hardware counter.

### 10.2 `rdtsc`

```cpp
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
```

**x86**: `RDTSC` ("read time-stamp counter") returns a 64-bit cycle counter split across `EDX:EAX`. Cost: ~15–20 cycles (~5 ns), and crucially it's a single instruction with no call overhead.

**ARM64**: `CNTVCT_EL0` is the virtual counter. `MRS` moves a system register into a general register. On Apple Silicon this counter ticks at **24 MHz**, giving ~41.67 ns resolution — which is why the benchmark output shows `0ns` for anything faster than that. On the x86 target you'll see real numbers.

### 10.3 Reading the inline assembly syntax

```cpp
__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
```

- `__asm__` — GCC/Clang extended inline assembly.
- `volatile` — do not optimise this away or move it, even though it has no visible side effects. Essential: without it the compiler may hoist the timer read out of a loop.
- `"rdtsc"` — the instruction.
- `:` separates the template from the **output operands**.
- `"=a"(lo)` — constraint `a` means register EAX/RAX; `=` means write-only; bind it to the C variable `lo`.
- `"=d"(hi)` — register EDX/RDX.

For ARM: `"=r"(val)` means "any general-purpose register", and `%0` in the template refers to operand 0.

### 10.4 Calibration

`rdtsc` returns *ticks*, not nanoseconds. To convert, measure both clocks over a known interval:

```cpp
static double calibrate_tsc() {
    auto t0_wall = std::chrono::steady_clock::now();
    uint64_t t0_tsc = rdtsc();

    volatile int sink = 0;
    for (int i = 0; i < 100'000'000; ++i) sink += i;   // burn ~100ms

    uint64_t t1_tsc = rdtsc();
    auto t1_wall = std::chrono::steady_clock::now();

    double wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1_wall - t0_wall).count();
    return (t1_tsc - t0_tsc) / wall_ns;   // ticks per nanosecond
}
```

`volatile int sink` prevents the compiler from deleting the loop as dead code — `volatile` forces every write to actually happen.

The ~20ns of `chrono` overhead is amortised over ~100 ms, so it contributes ~0.00002% error. Irrelevant.

### 10.5 Invariant TSC

On old CPUs, `RDTSC` counted actual core clock cycles, so it changed rate with frequency scaling and stopped in deep sleep states. Useless as a clock.

All modern x86 CPUs (Nehalem 2008+) have **invariant TSC**: it ticks at a constant rate tied to the base frequency regardless of turbo, throttling, or C-states. Check it:

```bash
grep -o 'constant_tsc\|nonstop_tsc\|tsc_reliable' /proc/cpuinfo | sort -u
```

You want `constant_tsc` and `nonstop_tsc`. The i9-10900K has both.

**The remaining caveat**: TSC is synchronised across cores on a single socket, but a thread migrating between cores mid-measurement can in principle see a small skew. This is why `bench_ring`'s cross-core test shows a nonsense `max` value — a few samples where `rdtsc()` on the consumer core read slightly *lower* than the producer's timestamp, and the unsigned subtraction wrapped to ~1.8e19.

Two fixes: pin the threads (Chapter 25), and clamp:

```cpp
uint64_t delta = now - msg.tsc;
latencies[i] = (delta > (1ULL << 62)) ? 0 : delta;   // treat wrap as zero
```

Worth mentioning in an interview as a known artefact you can explain rather than a bug you didn't notice.

### 10.6 Out-of-order execution and serialisation

`RDTSC` is not a barrier. The CPU may reorder it relative to surrounding instructions:

```cpp
uint64_t t0 = rdtsc();
foo();                  // CPU might start foo() before rdtsc retires
uint64_t t1 = rdtsc();  // or retire this before foo() finishes
```

For precise single-operation timing, use `RDTSCP` (which waits for prior instructions to retire) or add a serialising instruction:

```cpp
__asm__ volatile("lfence" ::: "memory");   // load fence + compiler barrier
uint64_t t0 = rdtsc();
```

TradeFeed's benchmarks don't do this, deliberately: we time *many* operations and look at the distribution, where reordering noise averages out. For timing a single 5-instruction sequence you'd need the fences. The `::: "memory"` clobber also tells the compiler not to move memory operations across the barrier.

### 10.7 Why percentiles, not averages

```
  add_order   n=500000   p50=0ns  p90=42ns  p99=83ns  p99.9=625ns  max=12751ns
```

An average hides everything that matters. If 999 operations take 50 ns and one takes 50 µs, the average is ~100 ns — and you'd never know a 50 µs stall exists.

In trading, the tail *is* the product. A p99.9 of 625 ns means one order in a thousand takes 625 ns. At 100,000 orders/second that's 100 slow orders per second. If a slow order means a missed fill, that's real money.

Standard percentiles to report: **p50 (median), p90, p99, p99.9, p99.99, max**. TradeFeed's `LatencyStats` sorts the raw samples and indexes directly:

```cpp
std::sort(samples.begin(), samples.end());
auto ns = [&](size_t idx) { return samples[idx] / ticks_per_ns; };
ns(n * 50 / 100)     // p50
ns(n * 999 / 1000)   // p99.9
```

Storing every raw sample is exact but uses memory (500K samples × 8 bytes = 4 MB) and the sort is O(n log n) *after* the measurement, so it doesn't perturb the timing.

**HdrHistogram** is the production alternative: it bins values logarithmically with a configured precision, uses fixed pre-allocated memory, records in O(1), and reports any percentile. Use it when you need continuous recording in a live system rather than a fixed-size benchmark run.

### 10.8 Benchmark hygiene

Things that will give you wrong numbers:

**Dead-code elimination.** If the result is unused, the compiler deletes the work:
```cpp
volatile int sink = 0;      // forces the store
benchmark::DoNotOptimize(x); // Google Benchmark's version
__asm__ volatile("" :: "r"(x)); // manual: pretend to use x
```

**Cold caches on the first iterations.** Run a warmup pass before recording. TradeFeed's benchmarks pre-populate the book before the measured phase, which serves this purpose.

**Frequency scaling.** The CPU ramps up over the first few milliseconds. Long benchmarks amortise this; short ones should warm up first. On Linux, pin the governor:
```bash
sudo cpupower frequency-set -g performance
```

**Other processes.** On the benchmark machine, isolate cores (`isolcpus=2,3` kernel parameter) so the scheduler won't put anything else on them.

**Measuring the wrong thing.** `bench_add_order` deliberately constructs non-crossing orders so it measures insertion only, not matching. `bench_matching` deliberately constructs crossing orders. Mixing them gives a number that means nothing.

### Interview Questions

**Q: Why `rdtsc` instead of `std::chrono`?**
A: `chrono::now()` costs ~20–25 ns through the vDSO. `rdtsc` is one instruction, ~5 ns. When the thing you're timing is 50 ns, chrono's overhead is the measurement. Above ~1 µs, chrono is fine and more portable.

**Q: How do you convert TSC ticks to nanoseconds?**
A: Calibrate at startup — read both `rdtsc` and `steady_clock` across a ~100 ms busy loop and compute ticks per nanosecond. Valid because modern CPUs have invariant TSC (constant rate regardless of turbo/C-states).

**Q: Why report p99.9 rather than the mean?**
A: The mean hides the tail, and in trading the tail is what costs money. One order in a thousand taking 50 µs is invisible in an average but is 100 slow orders per second at realistic rates.

**Q: Your cross-core benchmark shows a max of 1.8e19 ns. Explain.**
A: Unsigned subtraction wrapping. A few samples had the consumer's `rdtsc` read marginally lower than the producer's timestamp — cross-core TSC skew on an unpinned thread. Fix: pin the threads and clamp the delta. The p50/p99 figures are unaffected.

### Exercises

1. Time an empty function 1M times with `chrono` and with `rdtsc`. Compute the per-call overhead of each.
2. Add the clamp from 10.5 to `bench_ring`'s cross-core latency test. Re-run and report the corrected max.
3. Remove `volatile` from `sink` in `calibrate_tsc`, compile with `-O3`, and print the calibration factor. Explain the result.
4. Add p99.99 and a full histogram dump to `LatencyStats`. Which operation has the worst tail, and can you explain why?
5. On Linux, run `bench_orderbook` with and without `taskset -c 3` and `cpupower frequency-set -g performance`. Compare the p99.9 figures.

---

# PART III — DATA STRUCTURES

Part II gave you the physics. Part III applies it. Every container choice in TradeFeed is a cache-line argument, and this part shows the working.

---

## Chapter 11: Memory Allocation and the Order Pool

### 11.1 What `new` actually does

```cpp
Order* o = new Order();
```

This looks like one operation. It is not. It expands to:

1. Call `operator new(64)` → `malloc(64)`.
2. `malloc` inspects its internal free lists (glibc uses size-class bins).
3. If a suitable free block exists, unlink it and return it.
4. If not, either split a larger block or request more memory from the kernel via `brk`/`mmap` (a **syscall**, ~1–2 µs).
5. In a multithreaded program, steps 2–4 need synchronisation. glibc uses per-thread arenas to reduce contention, but arena acquisition still involves an atomic operation, and cross-thread frees hit a lock.
6. Construct the `Order` at the returned address.

Typical cost when everything goes well: **40–100 ns**. Occasionally, when it must go to the kernel: **microseconds**.

`delete` is comparable — it must find the block's metadata, possibly coalesce with neighbours, and push it back on a free list.

### 11.2 Why that's fatal here

The matching engine's entire budget is ~50–200 ns per order. A single `new` would consume most of it. And an order's lifecycle involves at least one `new` and one `delete`.

Worse than the average is the **variance**. Most allocations are fast; occasionally one triggers an `mmap` and takes 2 µs. That shows up directly in your p99.9. You cannot build a predictable-latency system on an allocator with unpredictable latency.

There's a third problem: **fragmentation**. Over hours of running, interleaved allocations and frees of different sizes leave the heap scattered. Orders allocated at the same time end up far apart in memory, destroying the spatial locality from Chapter 6.

### 11.3 The arena / pool pattern — generic form

The fix is universal in low-latency and game engine code: **allocate everything up front, hand out slots, never call the system allocator again**.

```cpp
template <typename T, size_t N>
class Pool {
    T  storage_[N];
    T* free_[N];
    size_t free_count_ = N;
public:
    Pool() { for (size_t i = 0; i < N; ++i) free_[i] = &storage_[N - 1 - i]; }
    T*   allocate()      { return free_[--free_count_]; }
    void deallocate(T* p){ free_[free_count_++] = p; }
};
```

`allocate` is a decrement and an array read. `deallocate` is an array write and an increment. Both are ~1–2 ns, branch-free, and never touch the kernel.

### 11.4 TradeFeed's implementation

```cpp
class OrderPool {
    std::vector<Order>  arena_;      // the actual storage: N contiguous Orders
    std::vector<Order*> free_list_;  // stack of available slots

public:
    explicit OrderPool(size_t capacity = MAX_ORDERS) : arena_(capacity) {
        free_list_.reserve(capacity);
        for (size_t i = capacity; i > 0; --i)
            free_list_.push_back(&arena_[i - 1]);
    }

    Order* allocate() {
        assert(!free_list_.empty());
        Order* o = free_list_.back();
        free_list_.pop_back();
        return o;
    }

    void deallocate(Order* o) {
        o->prev = nullptr;
        o->next = nullptr;
        o->filled_qty = 0;
        free_list_.push_back(o);
    }

    size_t available() const { return free_list_.size(); }
    size_t capacity()  const { return arena_.size(); }
};
```

Line by line:

**`arena_(capacity)`** — one `std::vector<Order>` of 1,048,576 elements. That's a single 64 MB heap allocation at startup, then never again. All orders are contiguous: `&arena_[i+1] == &arena_[i] + 1`.

**`free_list_.reserve(capacity)`** — pre-allocates the pointer stack so `push_back` never reallocates. Without this, the first million `push_back`s would trigger ~20 reallocations, and later `deallocate` calls on the hot path could reallocate mid-trading. `reserve` makes `deallocate` genuinely O(1) with zero allocation, always.

**The reverse loop** — `for (size_t i = capacity; i > 0; --i) free_list_.push_back(&arena_[i-1]);` pushes `&arena_[N-1]` first, down to `&arena_[0]` last. Since `allocate` pops from the back, the first order handed out is `&arena_[0]`, then `&arena_[1]`, and so on. **Early allocations walk forward through contiguous memory**, so the prefetcher works and successive orders share cache lines' worth of locality. A forward-pushing loop would hand out addresses in descending order — still contiguous, but backwards, which some prefetchers handle less well.

**`allocate()`** — `back()` then `pop_back()`. Two instructions plus the assert (compiled out in release).

**`deallocate(o)`** — resets the three fields that must be clean for reuse (`prev`, `next`, `filled_qty`), then pushes. It deliberately does *not* clear `price`, `id`, etc., because `add_order` overwrites all of them unconditionally. Zeroing 64 bytes you're about to overwrite is wasted work.

### 11.5 LIFO is not an accident

The free list is a **stack**, so the most recently freed order is the next one allocated.

That order's cache line was touched microseconds ago. It is still in L1. Allocating it costs zero misses.

A FIFO free list (queue) would hand back the *oldest* freed order — one whose line was evicted long ago. Every allocation would be a cache miss.

This is a general principle: **when reusing memory, prefer the most recently used block**. Same reason stack allocation is fast.

### 11.6 The `assert`

```cpp
assert(!free_list_.empty());
```

`assert` from `<cassert>` evaluates its condition in debug builds and calls `abort()` if false. When `NDEBUG` is defined (any release build, including CMake's `Release` type), the macro expands to nothing — **zero runtime cost**.

The pool is not defensive at runtime because the caller already checked:

```cpp
bool OrderBook::add_order(...) {
    if (pool_.available() == 0) return false;   // the real check
    Order* o = pool_.allocate();                 // guaranteed safe
```

This is the correct division: **validate once at the boundary, assert internally**. Checking twice costs a branch on the hot path for a condition that cannot occur.

### 11.7 Capacity and the trade

`MAX_ORDERS = 1 << 20` = 1,048,576 orders × 64 bytes = **64 MB**, allocated at startup.

- A power of two so `id & ORDER_MASK` works for the lookup table (Chapter 14).
- 1M simultaneously-resting orders is far beyond any single-symbol book in reality (NASDAQ's busiest names rest tens of thousands).
- If the pool exhausts, `add_order` returns `false` and the engine emits an `OrderRejected`. Graceful, not a crash.

The cost is 64 MB of RAM held forever. On a 66 GB machine, irrelevant. On an embedded system you'd tune it down.

**Production refinement**: allocate the arena with huge pages (`madvise(MADV_HUGEPAGE)`) to cut TLB pressure from ~16,000 pages to 32, and pre-fault it at startup (touch every page) so no page faults occur during trading.

### Interview Questions

**Q: Why not just use `new`/`delete`?**
A: 40–100 ns typical, microseconds when it hits the kernel, with a lock in multithreaded builds — that's most of my per-order budget, and the variance lands directly in p99.9. It also fragments the heap over time, destroying locality. The pool is 1–2 ns, branch-free, syscall-free, and keeps all orders contiguous.

**Q: Why is the free list LIFO?**
A: The most recently freed order is still in L1. Reusing it costs zero cache misses. A FIFO queue would return the coldest block every time.

**Q: What happens when the pool runs out?**
A: `available()` returns 0, `add_order` returns false, the engine emits `OrderRejected`. No allocation, no crash. Sizing is a capacity-planning decision — 1M resting orders is well above any real single-symbol book.

**Q: Why `reserve` on the free list?**
A: Without it, `push_back` in `deallocate` could reallocate — a heap allocation on the hot path, exactly what the pool exists to avoid.

### Exercises

1. Benchmark `new Order()`/`delete` vs `pool.allocate()`/`deallocate()` over 10M cycles. Report both means and p99.9.
2. Change the free list from a stack to a queue (`std::deque`, pop from front). Re-run `bench_orderbook`. Explain the difference.
3. Remove `free_list_.reserve(capacity)`. Instrument `push_back` to detect reallocation. How many occur during `bench_mixed_workload`?
4. Make `deallocate` zero the entire `Order` with `memset`. Measure the cost. Is it ever worth it? (Hint: security — what if orders held client secrets?)
5. Implement a pool where the free list is an *intrusive* stack — reuse `Order::next` as the free-list link instead of a separate `vector<Order*>`. What do you save? What do you lose?

---

## Chapter 12: Intrusive Linked Lists

### 12.1 Non-intrusive: what `std::list` does

```cpp
std::list<Order*> level;
level.push_back(order);
```

`std::list` is a doubly-linked list of *nodes it allocates itself*:

```cpp
struct ListNode {
    ListNode* prev;      // 8
    ListNode* next;      // 8
    Order*    value;     // 8  ← your data (here, a pointer to it)
};                       // 24 bytes, separately heap-allocated
```

Every `push_back` calls `operator new` for a 24-byte node. Every `erase` calls `delete`. So the allocator cost from Chapter 11 is back — on every order insertion.

And the memory layout is bad:

```
Order (in arena) ──────▶ [ Order data, 64 bytes ]
        ▲
        │ value pointer
ListNode (heap) ───────▶ [ prev | next | value ]   ← somewhere else entirely
```

Traversing means: load node (miss), load `value` pointer, dereference to reach the Order (second miss). **Two cache misses per element** instead of one.

### 12.2 Intrusive: the links live in the data

```cpp
struct Order {
    // ... payload ...
    Order* prev;
    Order* next;
};
```

The list pointers are *fields of the object being stored*. There is no separate node, so:

- **Zero allocation.** `push` is pointer assignment. The `Order` already exists in the pool.
- **One cache miss per element.** The links and the payload are in the same 64-byte line. Loading `order->next` also loads `order->price` for free.
- **O(1) removal by pointer.** Given an `Order*`, you can unlink it immediately — no search. With `std::list` you'd need to have stored an iterator.

The cost: an object can only be in one intrusive list at a time (per link pair), and the container is coupled to the element type. Both are fine here — an order is in exactly one price level.

This is why the Linux kernel (`list_head`), Boost.Intrusive, and essentially every game engine and trading system use intrusive containers.

### 12.3 TradeFeed's `PriceLevel`

```cpp
class PriceLevel {
    Order*   head_      = nullptr;   // oldest order — fills first (time priority)
    Order*   tail_      = nullptr;   // newest order — most recently added
    Quantity total_qty_ = 0;         // sum of remaining() across the list
    uint32_t count_     = 0;         // number of orders

public:
    void push(Order* o) {
        o->prev = tail_;
        o->next = nullptr;
        if (tail_) tail_->next = o;
        else       head_ = o;
        tail_ = o;
        total_qty_ += o->remaining();
        ++count_;
    }

    void remove(Order* o) {
        if (o->prev) o->prev->next = o->next;
        else         head_ = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail_ = o->prev;
        total_qty_ -= o->remaining();
        --count_;
        o->prev = o->next = nullptr;
    }

    Order*   front()     const { return head_; }
    bool     empty()     const { return count_ == 0; }
    Quantity total_qty() const { return total_qty_; }
    uint32_t count()     const { return count_; }

    void adjust_qty(int32_t delta) { total_qty_ += delta; }
};
```

`sizeof(PriceLevel)` = 8 + 8 + 4 + 4 = **24 bytes**. Two pointers and two counters. Two and a half price levels fit in a cache line.

### 12.4 `push` — walking the pointer surgery

Appending order D to `[A ⇄ B ⇄ C]`:

```
Before:   head_ → A ⇄ B ⇄ C ← tail_

o->prev = tail_;         D.prev = C
o->next = nullptr;       D.next = null
if (tail_) tail_->next = o;   C.next = D        (list was non-empty)
else       head_ = o;                            (list was empty → D is also head)
tail_ = o;               tail_ = D

After:    head_ → A ⇄ B ⇄ C ⇄ D ← tail_
```

The `if (tail_)` handles the empty-list case: when there is no tail, the new order becomes both head and tail. Four pointer writes, one branch. **O(1).**

**Appending at the tail is what enforces time priority.** New orders join the back of the queue. `front()` returns the oldest. FIFO.

### 12.5 `remove` — the four-case unlink

Removing B from `[A ⇄ B ⇄ C]`:

```
if (o->prev) o->prev->next = o->next;   A.next = C     (B had a predecessor)
else         head_ = o->next;                          (B was head → C becomes head)

if (o->next) o->next->prev = o->prev;   C.prev = A     (B had a successor)
else         tail_ = o->prev;                          (B was tail → A becomes tail)
```

Two conditionals cover all four cases — middle, head, tail, or only element — without special-casing each explicitly. This is the standard doubly-linked unlink and you should be able to write it from memory.

Then `o->prev = o->next = nullptr;` — leaving stale pointers in a pooled object is how you get use-after-free bugs when it's reallocated.

**This is why the list is doubly-linked.** With only `next`, removing B requires walking from `head_` to find A — O(n). Cancellation is ~20% of real order flow, so O(n) cancels would dominate. The `prev` pointer costs 8 bytes per order and buys O(1) cancellation.

### 12.6 The `total_qty_` invariant

`total_qty_` must always equal the sum of `remaining()` over every order in the list. Three operations touch it:

```cpp
push:        total_qty_ += o->remaining();
remove:      total_qty_ -= o->remaining();
adjust_qty:  total_qty_ += delta;
```

`adjust_qty` exists because a resting order can be *partially filled* without leaving the list. The matching code calls it:

```cpp
aggressor->fill(fill_q);
resting->fill(fill_q);
level.adjust_qty(-static_cast<int32_t>(fill_q));   // level total drops by the fill
```

Verify the arithmetic balances over an order's life:

```
push A (qty 100):        total += 100        → 100
fill A for 40:           total += (-40)      →  60     A.remaining() == 60  ✓
fill A for 60:           total += (-60)      →   0     A.remaining() ==  0  ✓
remove A:                total -= 0          →   0     ✓
```

And a partial that stays resting:

```
push A (qty 100):        total += 100        → 100
fill A for 40:           total += (-40)      →  60
(A stays in the list)    total == 60 == A.remaining()  ✓
```

Because `remove` subtracts `remaining()` — which has already been reduced by the fills — every path balances. **This is the kind of invariant an interviewer will probe; be ready to trace it on a whiteboard.**

Why maintain it incrementally rather than summing on demand? Analytics (spread, imbalance, depth) query `total_qty()` on every book update. Walking a level with 500 resting orders would be 500 cache misses per query. The counter is one load.

### 12.7 `count_` and why `empty()` uses it

```cpp
bool empty() const { return count_ == 0; }
```

`head_ == nullptr` would also work and is one fewer field. But `count_` is useful for analytics ("how many orders at the BBO?") and for debugging. Given we're already paying 24 bytes and the struct has spare alignment slack, the counter is free.

### Interview Questions

**Q: Why an intrusive list instead of `std::list`?**
A: `std::list` heap-allocates a node per insert (40–100 ns each) and adds a level of indirection, so traversal costs two cache misses per element instead of one. Intrusive links live inside the `Order`, which already exists in the pool — zero allocation, one miss, and O(1) removal given just the pointer.

**Q: Why doubly-linked?**
A: O(1) cancellation. With only `next`, unlinking an arbitrary order requires an O(n) walk to find its predecessor. Cancels are ~20% of order flow, so that would dominate. The `prev` pointer costs 8 bytes.

**Q: How do you keep `total_qty_` correct when orders are partially filled?**
A: `push` adds `remaining()`, `remove` subtracts the current `remaining()`, and `adjust_qty` is called with the negative fill quantity on each partial. Since `remove` reads the already-reduced `remaining()`, every path balances to zero. I can trace it for you.

**Q: What's the downside of intrusive containers?**
A: An object can belong to only one list per link-pair, and the container is coupled to the element type. Neither matters here — an order rests at exactly one price level.

### Exercises

1. Implement `PriceLevel` with `std::list<Order*>` instead. Benchmark `bench_mixed_workload` against the intrusive version.
2. Write a `PriceLevel::validate()` that walks the list and asserts `count_` and `total_qty_` match the actual contents. Call it after every operation in a debug build. Run the mixed workload.
3. Remove the `prev` pointer and implement singly-linked removal. Measure how cancel latency scales with orders-per-level (try 1, 10, 100, 1000).
4. Add a second intrusive link pair (`client_prev`/`client_next`) so each order is simultaneously in its price level *and* a per-client list. Implement `cancel_all_for_client(ClientId)`.
5. Trace by hand: push A(50), B(30), C(20); fill A fully; fill B for 10; remove C. What is `total_qty_` and `count_` after each step?

---

## Chapter 13: The Price-Indexed Array

### 13.1 The problem

A book must map **price → the queue of orders at that price**, supporting:

- Insert an order at a price.
- Remove an order.
- Find the best (highest bid / lowest ask).
- Walk outward from the best price when sweeping.

### 13.2 Option A: `std::map<Price, PriceLevel>`

A red-black tree: self-balancing BST with nodes allocated individually.

| Operation | Complexity | Real cost |
|-----------|-----------|-----------|
| Insert | O(log n) | tree walk + node allocation + rebalancing rotations |
| Lookup | O(log n) | ~20 pointer-chasing hops for n=1M, each likely a cache miss |
| Best bid | O(1) | `rbegin()` — genuinely cheap |
| Best ask | O(1) | `begin()` |

The killer is the constant factor. Each node is a separate allocation with `parent`/`left`/`right` pointers plus colour. Traversal is pure pointer chasing — **~20 dependent cache misses at ~80 ns each ≈ 1.6 µs** for a cold lookup. Insertion adds an allocation and rotations that write to several scattered nodes.

`std::map` is the textbook answer and the wrong one here.

### 13.3 Option B: `std::unordered_map<Price, PriceLevel>`

O(1) average lookup, but:
- Hashing costs something.
- Bucket array lookup → chain pointer → node. Two or three cache misses.
- **No ordering.** Finding the best bid means scanning every bucket — O(capacity). Fatal, since best-price queries happen on every single order.

Rejected.

### 13.4 Option C: direct-indexed array — what TradeFeed does

Prices are integers in a known, bounded range. So use the price *as the index*:

```cpp
std::unique_ptr<PriceLevel[]> bids_;   // PRICE_LEVELS entries
std::unique_ptr<PriceLevel[]> asks_;

size_t idx(Price p) const { return p - MIN_PRICE; }

bids_[idx(price)].push(order);
```

| Operation | Complexity | Real cost |
|-----------|-----------|-----------|
| Insert | **O(1)** | one subtract, one shift-add, one cache line touched |
| Lookup | **O(1)** | same |
| Remove | **O(1)** | same |
| Best bid | O(1) amortised | cached in `best_bid_`; rescan on depletion (§13.7) |

`bids_[idx(p)]` compiles to roughly:

```asm
sub   eax, 1              ; p - MIN_PRICE
imul  rax, rax, 24        ; × sizeof(PriceLevel)
add   rax, [rdi+bids_]    ; + base
```

Three instructions, no branches, no pointer chasing. **One cache miss worst case, and near the spread it's an L1 hit every time.**

This is a **direct-address table** — the degenerate case of hashing where the key *is* the index and the hash function is the identity. It only works when keys are dense integers in a bounded range. Prices are exactly that.

### 13.5 The memory trade

```
PRICE_LEVELS = MAX_PRICE - MIN_PRICE + 1 = 1,000,000
sizeof(PriceLevel) = 24 bytes
bids_: 24 MB
asks_: 24 MB
```

48 MB allocated regardless of how many price levels are actually occupied. A book with 200 active levels still reserves space for a million.

**Why that's fine:**

1. **Virtual memory is lazy.** `new PriceLevel[1'000'000]()` value-initialises, which touches every page — so this version does commit all 48 MB. (A `calloc`-based version relying on the kernel's zero page would commit lazily. Worth knowing the distinction.)
2. **Only the hot region is *cached*.** Committed RAM ≠ cache pressure. The ~100 price levels near the spread occupy ~2.4 KB — permanently L1-resident. The other 48 MB sits untouched in DRAM and never competes for cache.
3. **48 MB on a 66 GB machine is 0.07%.**

**The trade stated plainly: we spend 48 MB of DRAM to turn an O(log n) pointer-chasing lookup into a single O(1) array index.** On modern hardware that is overwhelmingly the right trade, and being able to say it in exactly those terms is the interview answer.

### 13.6 When this breaks

Direct indexing requires a bounded, dense key space. It fails when:

- **The range is huge.** FX at 8 decimals over $0–$10,000 is 10¹² ticks — a petabyte. You'd need a hash map or a hybrid.
- **The range is unknown at startup.** Crypto pairs range from 10⁻⁸ to 10⁵.
- **Keys are sparse.** Most equities trade within ±20% of the previous close, so 95% of the array is permanently empty.

**The production refinement** is a two-tier scheme: a direct-indexed window of a few thousand ticks centred on the current price (covering ~99.99% of activity) with a hash map fallback for far-out-of-the-money prices, and re-centring the window when the price drifts. TradeFeed keeps the flat array for clarity; knowing the refinement is the senior answer.

### 13.7 Tracking the best price

```cpp
Price best_bid_ = INVALID_PRICE;    // 0 = no bids
Price best_ask_ = MAX_PRICE + 1;    // sentinel = no asks
```

**Sentinels chosen so comparisons work without special cases.** With no asks, `best_ask_ = 1'000'001`, so `best_ask_ <= aggressor->price` is false for any legal price — the match loop exits naturally, no null check. Same for `best_bid_ = 0` against `best_bid_ >= price`.

**On insert** — O(1):

```cpp
if (side == Side::Buy) {
    bids_[idx(price)].push(o);
    if (price > best_bid_) best_bid_ = price;    // new best
}
```

**On depletion** — scan:

```cpp
void OrderBook::scan_best_bid() {
    for (Price p = best_bid_; p >= MIN_PRICE; --p) {
        if (!bids_[idx(p)].empty()) { best_bid_ = p; return; }
        if (p == MIN_PRICE) break;        // guard: Price is unsigned, --p would wrap
    }
    best_bid_ = INVALID_PRICE;
}
```

The `if (p == MIN_PRICE) break;` is not redundant. `Price` is `uint32_t`; if `MIN_PRICE` were 0, `--p` at 0 wraps to 4 billion and the loop never ends. This is the unsigned-wrap hazard from Chapter 2 showing up in real code.

**Cost:** one iteration per empty price level between the old best and the new one. In a liquid market the next level is 1–5 ticks away — a handful of L1 hits, effectively free. In a thin book after a sweep, it could be thousands. That's the honest weakness of this design, and the mixed-workload p99.9 of 333 ns for cancels reflects exactly this.

### 13.8 The upgrade: hierarchical bitset

One bit per price level, "is this level non-empty", in three tiers:

```
L0: 1,000,000 bits = 15,625 uint64 words   (one bit per price)
L1:    15,625 bits =    245 uint64 words   (one bit per L0 word: "any bit set below?")
L2:       245 bits =      4 uint64 words   (one bit per L1 word)
```

Finding the highest set bit (best bid):

```cpp
int w2 = 63 - __builtin_clzll(l2[0]);        // which L1 word
int w1 = w2*64 + 63 - __builtin_clzll(l1[w2]); // which L0 word
int w0 = w1*64 + 63 - __builtin_clzll(l0[w1]); // which bit → the price
```

`__builtin_clzll` = count leading zeros, one `LZCNT`/`BSR` instruction. Three loads and three instructions — **truly O(1), independent of book shape**. Total memory: ~2 MB. Maintenance: one bit set on insert, one cleared when a level empties.

That removes the only non-O(1) operation in the book. It's the single highest-value improvement available and the right answer to "what would you optimise next?"

### Interview Questions

**Q: Why an array indexed by price instead of `std::map`?**
A: `std::map` is a red-black tree — O(log n) with ~20 dependent cache misses per lookup plus node allocation and rebalancing on insert. The array is one subtract, one shift-add, one load: O(1), and near the spread it's an L1 hit. It costs 48 MB of DRAM, which buys the elimination of all pointer chasing.

**Q: Isn't 48 MB wasteful for a book with 200 active levels?**
A: In absolute terms it's 0.07% of the machine's RAM. What matters for speed is the *cached* working set, and that's the ~100 levels near the spread — about 2.4 KB, permanently in L1. Untouched DRAM doesn't compete for cache.

**Q: When would this approach fail?**
A: Unbounded or sparse key spaces — FX at 8 decimals is 10¹² ticks. The fix is a direct-indexed window around the current price with a hash fallback for far strikes, re-centred as the price drifts.

**Q: What's the complexity of finding the best bid?**
A: O(1) on insert (compare and maybe update). On depletion it's a linear scan from the old best — typically 1–5 ticks in a liquid book, but unbounded in a thin one. The fix is a hierarchical bitset: three `__builtin_clzll` calls, genuinely O(1), ~2 MB. That's my next optimisation.

### Exercises

1. Replace `bids_`/`asks_` with `std::map<Price, PriceLevel>`. Run `bench_orderbook`. Report the slowdown for each operation.
2. Instrument `scan_best_bid` to count iterations. Run the mixed workload and histogram the counts. What's the p99?
3. Construct a pathological case: one bid at price 1, one at 999,999, then cancel the top one. Time the resulting scan.
4. Implement the hierarchical bitset from §13.8. Verify it against the linear scan on random books, then benchmark the pathological case from (3).
5. Compute the memory for a book supporting $0.00000001–$100,000.00000000 at 8 decimals with direct indexing. Design a windowed alternative and state its lookup cost.

---

## Chapter 14: Hash Tables and the Order Lookup

### 14.1 The requirement

`cancel_order(id)` and `modify_order(id, ...)` receive only an `OrderId`. The book must find that order's `Order*` — which price level it rests at, where in the list. This lookup happens on ~30% of all messages.

### 14.2 How hash tables work (and why they're slow here)

A hash table maps arbitrary keys to values by computing `hash(key) % capacity` to get a bucket index. Collisions — two keys landing in the same bucket — are resolved two ways:

**Chaining** (`std::unordered_map`): each bucket holds a pointer to a linked list of entries.

```
buckets[]:  [ ptr ][ null ][ ptr ][ null ] ...
               │              │
               ▼              ▼
            [k,v]→[k,v]     [k,v]
```

Lookup: hash → load bucket pointer (**miss 1**) → follow to node (**miss 2**) → compare key → maybe follow again. The standard mandates bucket-and-chain semantics (reference stability, `bucket_count()`), so every implementation pays this. **Two-plus dependent cache misses per lookup.**

**Open addressing** (`absl::flat_hash_map`, `robin_hood`): entries live *in* the bucket array; collisions probe the next slot.

```
buckets[]:  [k,v][k,v][ empty ][k,v] ...
```

Lookup: hash → load slot (**miss 1**) → compare → probe linearly if needed (same cache line, free). **One cache miss.** Typically 2–3x faster than `unordered_map` for this reason.

Either way, you pay a hash computation and at least one miss.

### 14.3 The observation that eliminates all of it

TradeFeed assigns order IDs itself:

```cpp
case MessageType::NewOrder: {
    OrderId id = next_id_++;    // 1, 2, 3, 4, ...
```

The keys are **sequential integers**. They are dense, ordered, and generated by us. A hash function's job is to scatter arbitrary keys uniformly — but our keys are *already* uniform. Hashing them is pure waste.

So use a direct-address table, exactly as in Chapter 13:

```cpp
std::unique_ptr<Order*[]> order_table_;    // MAX_ORDERS entries

Order* lookup(OrderId id) const {
    Order* o = order_table_[id & ORDER_MASK];
    return (o && o->id == id) ? o : nullptr;
}
void reg(Order* o)   { order_table_[o->id & ORDER_MASK] = o; }
void unreg(Order* o) { order_table_[o->id & ORDER_MASK] = nullptr; }
```

One AND, one load. No hash, no probe, no chain.

### 14.4 The mask

```cpp
constexpr size_t MAX_ORDERS = 1 << 20;        // 1,048,576
constexpr size_t ORDER_MASK = MAX_ORDERS - 1; // 0xFFFFF — 20 low bits
```

`id & ORDER_MASK` keeps the low 20 bits — equivalent to `id % 1'048'576` but as a single `AND` instruction instead of a division (~20–40 cycles).

**This only works because the capacity is a power of two.** For any power of 2ⁿ, `x & (2ⁿ − 1) == x % 2ⁿ`. It is the reason both this table and the SPSC ring require power-of-two sizes, enforced by `static_assert`.

```
id = 1,048,577  = 0x100001
mask            = 0x0FFFFF
id & mask       = 0x000001 = 1     ← wraps to slot 1
```

### 14.5 Why the `o->id == id` check exists

IDs are sequential and unbounded; the table has 2²⁰ slots. Order 1 and order 1,048,577 map to the same slot.

In practice this never collides: by the time ID 1,048,577 is issued, order 1 was filled or cancelled a million orders ago and `unreg` nulled its slot. But *in principle*, if a single order rested for more than 2²⁰ subsequent orders and a new order took its slot, a stale pointer would be returned — and cancelling the wrong order is a catastrophic, silent bug.

The verification:

```cpp
return (o && o->id == id) ? o : nullptr;
```

- `o` — is the slot occupied?
- `o->id == id` — is it *the order I asked for*, not a wrapped-around impostor?

Cost: one extra compare on a cache line already loaded (`id` is at offset 32 of the same 64-byte `Order`). **Effectively free**, and it converts a silent correctness disaster into a clean "order not found" rejection.

This is the general pattern for direct-address tables with wrapping keys: **store the key alongside the value and verify on read.**

### 14.6 Cost comparison

| Approach | Hash | Misses | Allocation | Notes |
|----------|------|--------|------------|-------|
| `std::unordered_map` | yes | 2+ | per insert | bucket → chain node |
| `absl::flat_hash_map` | yes | 1 | amortised on rehash | open addressing |
| **Direct-address table** | **no** | **1** | **none** | one AND + one load |

The table itself is 2²⁰ × 8 bytes = **8 MB**. Same argument as Chapter 13: a large allocation whose *active* portion — the slots for currently-resting orders — is small and stays cached.

### 14.7 Register and unregister, precisely

```cpp
reg(o);     // in add_order, right after the pool allocation
unreg(o);   // in cancel_order, and whenever an order is fully filled or discarded
```

Every path that returns an order to the pool must call `unreg` first. Trace them:

- `add_order`: fully filled during matching, or a market order with no remainder → `unreg` + `deallocate`.
- `cancel_order`: → `unreg` + `deallocate`.
- `drain_level`: a resting order fills completely → `remove` from level, `unreg`, `deallocate`.

Miss one and the table holds a pointer to a pooled order that has been handed to someone else — a use-after-free that the `o->id == id` check would *usually* catch (because the reused order has a different ID), which is a second reason that check earns its keep.

### Interview Questions

**Q: Why not `std::unordered_map<OrderId, Order*>`?**
A: It's chained hashing — a bucket array load plus a chain node load, so two-plus dependent cache misses per lookup, and a heap allocation per insert. Since I assign the IDs myself and they're sequential, I don't need hashing at all: a direct-address table indexed by `id & mask` is one AND and one load.

**Q: What if two IDs map to the same slot?**
A: I store the ID in the `Order` and verify `o->id == id` on lookup. A mismatch returns null, so a wrapped-around collision becomes a clean rejection rather than cancelling someone else's order. The check is free — `id` is in the same cache line I just loaded.

**Q: Why must the table size be a power of two?**
A: So `id & (size-1)` replaces `id % size`. Modulo is a 20–40 cycle division; AND is one cycle. The requirement is enforced at compile time.

**Q: How big is the table and is that a problem?**
A: 8 MB. Same trade as the price array — large allocation, small *active* working set. Only the slots for currently-resting orders get touched, and those stay in cache.

### Exercises

1. Replace `order_table_` with `std::unordered_map<OrderId, Order*>`. Benchmark cancel latency. Then try `reserve()`-ing it and measure again.
2. Force a collision: set `MAX_ORDERS` to 1024, submit 2000 orders keeping the first alive, then cancel order 1. Verify the `o->id == id` check saves you. Remove the check and observe the corruption.
3. Replace `id & ORDER_MASK` with `id % MAX_ORDERS` where `MAX_ORDERS` is 1000 (not a power of two). Inspect the assembly for the division. Benchmark.
4. Implement open-addressed hashing with linear probing for `OrderId → Order*` and benchmark it against the direct-address table. Explain the gap.
5. Deliberately remove one `unreg` call. Write a test that detects the resulting stale entry.

---

## Chapter 15: Choosing a Container — the Decision Table

### 15.1 What the STL costs

| Container | Lookup | Insert | Memory/elem | Cache behaviour | Allocates? |
|-----------|--------|--------|-------------|-----------------|------------|
| `std::vector` | O(1) index, O(n) search | O(1) amortised at end | sizeof(T) | **Excellent** — contiguous | on growth |
| `std::array` | O(1) index | n/a (fixed) | sizeof(T) | **Excellent** — inline | never |
| `std::deque` | O(1) index | O(1) both ends | sizeof(T) + chunk overhead | Good — chunked | per chunk |
| `std::list` | O(n) | O(1) given position | sizeof(T) + 16 | **Poor** — scattered nodes | **per element** |
| `std::map` | O(log n) | O(log n) | sizeof(T) + ~40 | **Poor** — tree walk | **per element** |
| `std::unordered_map` | O(1) avg | O(1) avg | sizeof(T) + ~16 | **Poor** — bucket + chain | **per element** |
| Direct-address array | **O(1)** | **O(1)** | sizeof(T) × range | **Excellent** | never (after init) |
| Intrusive list | O(n) walk, **O(1) by ptr** | **O(1)** | **0** (links in T) | Good — one miss/elem | **never** |

The right-hand columns matter more than the complexity column. `std::map` lookup being O(log n) is nearly irrelevant next to it being ~20 dependent cache misses.

### 15.2 The questions to ask

1. **Are the keys dense integers in a bounded range?** → direct-address array. (Prices: yes. Order IDs: yes.)
2. **Do I need ordering?** → array or sorted structure, not a hash map.
3. **Is the size known at compile time and small?** → `std::array`.
4. **Do I need O(1) removal of an arbitrary element I hold a pointer to?** → intrusive doubly-linked list.
5. **Do I only append and iterate?** → `std::vector` with `reserve`.
6. **Otherwise** → `std::vector` until profiling proves otherwise. It is the right default far more often than people expect.

### 15.3 Every container in TradeFeed, justified

| Where | Container | Why |
|-------|-----------|-----|
| `OrderPool::arena_` | `std::vector<Order>` | contiguous, allocated once, never resized |
| `OrderPool::free_list_` | `std::vector<Order*>` | LIFO stack; `reserve`d so never allocates |
| `OrderBook::bids_`/`asks_` | `unique_ptr<PriceLevel[]>` | direct-address by price; heap because 24 MB |
| `OrderBook::order_table_` | `unique_ptr<Order*[]>` | direct-address by `id & mask`; heap because 8 MB |
| `OrderBook::fills_` | `std::vector<Fill>` | append-then-iterate; `reserve(64)` |
| `PriceLevel` | intrusive doubly-linked | zero-alloc, O(1) push/remove, time priority |
| `SPSCRing::buffer_` | `T[Capacity]` inline | compile-time size, no indirection |
| `Gateway::clients_` | `std::array<ClientState, 64>` | fixed small size, inline is fine |
| `BookAnalytics::vpin_history_` | `std::array<double, 50>` | fixed small ring |
| `LatencyStats::samples` | `std::vector<uint64_t>` | append during run, sort after |

Not one `std::map`, `std::unordered_map`, or `std::list` in the whole system. That is not dogma — it's the outcome of applying §15.2 to each case.

### Interview Questions

**Q: You have no `std::map` or `unordered_map` anywhere. Isn't that over-engineering?**
A: It's the opposite — each case had a simpler structure available. Prices and order IDs are dense bounded integers, so direct addressing beats hashing with less code. If I had genuinely sparse or unbounded keys I'd reach for `absl::flat_hash_map` rather than write my own.

**Q: When *would* you use `std::map`?**
A: When I need ordered iteration over sparse keys and lookups aren't on the hot path — configuration, symbol metadata, an admin interface. The tree's cache behaviour is irrelevant if it's queried once per second.

**Q: What's your default container?**
A: `std::vector`, with `reserve` when the size is predictable. It's contiguous, prefetcher-friendly, and right far more often than people assume. I move off it only with a measurement or a structural reason.

### Exercises

1. For each container in §15.3, write the alternative you rejected and estimate its cost in cache misses per operation.
2. Benchmark `std::vector<int>` vs `std::list<int>` for: append 1M, iterate and sum, remove every other element. Explain each result.
3. Find the point where `std::vector` linear search beats `std::unordered_map` lookup (hint: it's larger than you'd guess — try 10, 50, 100, 500 elements).
4. Design the order book for an instrument whose price range is unknown at startup. Write the container decision and justify it.

---

# PART IV — CONCURRENCY

Two threads share the ring buffers. Everything in this part exists to make that sharing correct without locks.

---

## Chapter 16: Threads, Races, and Why Locks Are Too Slow

### 16.1 Threads

A **process** owns an address space. A **thread** is an execution context — its own registers and stack — inside that address space. All threads in a process share the heap, globals, and static storage.

That sharing is the whole point and the whole problem.

```cpp
#include <thread>
std::thread t([&]{ engine.run(g_running); });
t.join();
```

Constructing a `std::thread` starts an OS thread immediately. `join()` blocks until it finishes. Destroying a joinable thread calls `std::terminate` — the standard forces you to explicitly `join()` or `detach()`.

### 16.2 What a data race is

```cpp
int counter = 0;
// Thread A: counter++;
// Thread B: counter++;
```

`counter++` is three operations: load, add, store.

```
Thread A: load counter (0)
Thread B: load counter (0)
Thread A: add 1 → 1, store 1
Thread B: add 1 → 1, store 1
Result: 1, not 2
```

Formally, a **data race** is two threads accessing the same memory location, at least one writing, with no synchronisation between them. In C++ a data race is **undefined behaviour** — not "you get a stale value", but *anything at all*. The compiler is entitled to assume races don't happen, which licenses optimisations that make racy code behave bizarrely:

```cpp
bool done = false;          // plain bool, not atomic

// Thread A:
while (!done) { /* work */ }

// Thread B:
done = true;
```

The compiler sees that nothing inside the loop modifies `done`, hoists the load out, and generates `if (!done) while(true) {}`. An infinite loop, at `-O2`, from code that looks obviously correct. This is the single most common concurrency bug in C++, and the fix is `std::atomic<bool>`.

### 16.3 Mutexes, and what they cost

```cpp
#include <mutex>
std::mutex m;
{
    std::lock_guard<std::mutex> lock(m);   // RAII: locks here
    shared_data++;
}                                           // unlocks here
```

`std::lock_guard` is RAII for locks — it unlocks in its destructor, so early returns and exceptions can't leave the mutex held.

Costs:

| Case | Cost |
|------|------|
| Uncontended lock/unlock | ~20 ns (an atomic compare-exchange each way) |
| Contended | ~1–10 **µs** — the loser blocks, the kernel context-switches it out and later back in |
| Convoy effect | one slow holder stalls every waiter |
| Priority inversion | a low-priority holder blocks a high-priority waiter |

For a 50–200 ns hot path, even the uncontended 20 ns is significant. The contended microseconds are catastrophic, and worse, they're *unpredictable* — they land squarely in p99.9.

There's also a correctness hazard: a thread holding a lock can be descheduled by the OS, blocking every other thread for a full scheduling quantum (~1 ms). Lock-free algorithms cannot suffer this.

### 16.4 The design that avoids the problem

TradeFeed has exactly two threads and a strict ownership rule:

```
Gateway thread                    Engine thread
──────────────                    ─────────────
owns: sockets, client state       owns: OrderBook, OrderPool, all Orders
writes: inbound_ring head         writes: inbound_ring tail
        outbound_ring tail                outbound_ring head
```

**No object is written by both threads.** The order book is touched only by the engine. Socket buffers only by the gateway. The only shared state is the two ring buffers, and even there each thread writes only its own index.

This is the key architectural decision: **structure the system so there is almost nothing to synchronise**, then make that small remainder lock-free. Lock-free data structures are hard; needing only one of them, in one shape (single producer, single consumer), is what makes it tractable.

### 16.5 The status-line caveat

```cpp
const auto& book = engine.book();
std::printf("... bid: %u ...", book.best_bid());
```

`main` reads `best_bid_` while the engine writes it. Strictly, that is a data race.

It is benign in practice — an aligned 32-bit load/store is atomic on x86 and ARM64, so the worst outcome is a stale or slightly-inconsistent status line printed every 5 seconds. But "benign race" is not a concept the C++ standard recognises, and saying so in an interview is a trap.

**The honest answer**: it's a known race on a diagnostic path. The correct fix is to have the engine publish a snapshot struct through a third ring, or make the fields `std::atomic` with relaxed ordering. It's on the list, it's not on the hot path, and I'd fix it before shipping.

### Interview Questions

**Q: What's a data race and why is it worse than a stale read?**
A: Two threads accessing the same location, one writing, with no synchronisation. It's undefined behaviour, not just a stale value — the compiler assumes races don't occur, so it can hoist loads out of loops and turn a polling loop into an infinite one. The fix is `std::atomic`, which makes the access defined and stops the compiler assuming.

**Q: Why not just use a mutex around the queue?**
A: ~20 ns uncontended, 1–10 µs contended, plus the risk that the OS deschedules the lock holder and stalls everyone for a scheduling quantum. On a 50 ns hot path that's the whole budget, and the variance goes straight into p99.9.

**Q: How do you avoid locks entirely?**
A: By structuring ownership so there's nothing to lock. Each thread exclusively owns its data; the only shared state is two SPSC rings where each thread writes only its own index. Making one narrow lock-free primitive is tractable; making a whole system lock-free is not.

### Exercises

1. Write the `while (!done)` example from 16.2 with a plain `bool`, compile at `-O2`, and confirm it hangs. Change to `std::atomic<bool>` and confirm it terminates. Inspect both in `-S`.
2. Benchmark 10M increments: (a) plain `int`, one thread; (b) `std::atomic<int>`, one thread; (c) mutex-protected `int`, two threads; (d) `std::atomic<int>`, two threads. Explain the ordering of results.
3. Run `bench_ring`'s cross-core test under ThreadSanitizer (`-fsanitize=thread`). Does it report the ring as clean?
4. Add the status-line race fix: make `best_bid_`/`best_ask_` atomic with relaxed ordering. Measure the impact on `bench_orderbook`.

---

## Chapter 17: Atomics and the Memory Model

### 17.1 `std::atomic`

```cpp
#include <atomic>
std::atomic<size_t> x{0};
x.store(5, std::memory_order_release);
size_t v = x.load(std::memory_order_acquire);
x.fetch_add(1, std::memory_order_relaxed);
```

An atomic guarantees two things:

1. **Indivisibility** — no thread ever observes a partially-written value.
2. **A defined memory ordering** — constraints on how this operation is ordered relative to *other* memory operations in the same thread, as seen by other threads.

Property 1 is the obvious one and is nearly free: on x86 and ARM64, an aligned load or store of ≤8 bytes is *already* atomic in hardware. `std::atomic<size_t>::load(relaxed)` compiles to a plain `mov`.

Property 2 is where the subtlety and the cost live.

`is_lock_free()` tells you whether the type uses hardware atomics or a hidden mutex. For ≤8-byte types on modern platforms it's always true.

### 17.2 The reordering problem

You write:

```cpp
buffer[head] = item;     // (1) write the data
head_index = head + 1;   // (2) publish it
```

Three separate agents may reorder these:

**The compiler** — sees two independent stores and may emit them in either order.

**The CPU** — executes out of order and retires stores through a store buffer.

**The cache system** — stores may become visible to other cores in a different order than issued.

If (2) becomes visible before (1), a consumer sees `head_index` advance, reads `buffer[head]`, and gets garbage. The data structure is broken, intermittently, under load, in ways that don't reproduce in a debugger.

Memory ordering is how you forbid that specific reordering — and *only* that one, so you don't pay for barriers you don't need.

### 17.3 The orderings

**`memory_order_relaxed`** — atomicity only, no ordering guarantees.

```cpp
const size_t head = head_.val.load(std::memory_order_relaxed);
```

Use when the value is only read by the thread that writes it, or when you genuinely don't care about ordering (e.g. a statistics counter). Free — compiles to a plain load or store.

In `push`, the producer loads its own `head_`. No other thread writes it, so no ordering is needed. Relaxed is correct and costs nothing.

**`memory_order_release`** (stores) — "everything I wrote before this, in program order, is visible to anyone who *acquires* this variable."

```cpp
buffer_[head & MASK] = item;                          // (1)
head_.val.store(head + 1, std::memory_order_release); // (2) — (1) cannot move after this
```

It is a **one-way barrier**: prior operations cannot sink below it, but later operations may rise above it. That's exactly what publishing needs.

**`memory_order_acquire`** (loads) — "anything the releasing thread wrote before its release is visible to me after this load."

```cpp
cached_head_ = head_.val.load(std::memory_order_acquire);  // (1)
item = buffer_[tail & MASK];                                // (2) — cannot move before (1)
```

The opposite one-way barrier: later operations can't rise above it.

**Release/acquire pair** together form a *synchronises-with* relationship. It's the standard publish/subscribe primitive, and it's what makes the ring correct:

```
Producer                              Consumer
────────                              ────────
write buffer_[N]                      load head_  (acquire)  ──┐
store head_ = N+1  (release) ─────────────────────────────────┘
                                      read buffer_[N]   ← guaranteed to see the write
```

**`memory_order_seq_cst`** — the default. Everything above, plus a single global total order that all threads agree on. On x86 this makes stores emit `MFENCE` or `XCHG`; on ARM it emits `DMB ISH`. **~10–30 ns extra per operation.**

TradeFeed never uses it. `std::atomic` defaults to it precisely because it's the easiest to reason about — but the ring's correctness needs only release/acquire, and paying for `seq_cst` on every push would roughly double the cost.

### 17.4 Hardware reality

| Ordering | x86-64 | ARM64 |
|----------|--------|-------|
| relaxed load/store | `mov` | `ldr`/`str` |
| acquire load | `mov` (free — x86 is strongly ordered) | `ldar` |
| release store | `mov` (free) | `stlr` |
| seq_cst store | `xchg` or `mov`+`mfence` (**expensive**) | `stlr` + `dmb` |

x86 gives acquire/release essentially for free — its memory model already forbids the dangerous reorderings, so the ordering annotations only constrain the *compiler*. ARM64 has a weaker model and needs real instructions (`ldar`/`stlr`), but those are still far cheaper than a full barrier.

This is why code that "works fine" on x86 can break on ARM. The annotations aren't decoration — they're what makes the algorithm portable and correct. Write them as if the hardware were weak, because somewhere it is.

### 17.5 Compiler barriers

```cpp
__asm__ volatile("" ::: "memory");
```

An empty asm statement with a `"memory"` clobber. It generates no instructions but tells the compiler "this may read or write any memory" — so it can't move memory operations across it. Useful in benchmarks to stop the optimiser reordering the timed region. It constrains only the compiler, not the CPU.

### Interview Questions

**Q: What does `memory_order_release` actually guarantee?**
A: Every memory operation before it in program order is visible to any thread that performs an acquire load on the same variable and sees that value. It's a one-way barrier — prior operations can't sink below it. Paired with an acquire load, it's the publish/subscribe primitive.

**Q: Why not just use the default `seq_cst`?**
A: It adds a global total order, which costs an `MFENCE` on x86 and a `DMB` on ARM — roughly 10–30 ns per operation. The ring only needs release/acquire to guarantee the data write is visible before the index advance. Paying for `seq_cst` on every push would roughly double the cost for a guarantee I don't use.

**Q: Why is the producer's load of its own `head_` relaxed?**
A: No other thread writes `head_`, so there's nothing to order against. I need atomicity (which is free for an aligned 8-byte load) but no barrier.

**Q: If x86 gives acquire/release for free, why annotate?**
A: Two reasons: the annotations also constrain the *compiler*, which reorders regardless of hardware; and ARM64 genuinely needs `ldar`/`stlr`. Code that's only correct on x86 breaks silently on Apple Silicon or Graviton.

### Exercises

1. Compile a release store and a seq_cst store to assembly on x86 (`-S -masm=intel`) and on ARM (`-target aarch64`). Compare the instructions.
2. Write Dekker's algorithm with relaxed ordering. Run it on ARM (or under `herd7`/`cppmem`) and find the violation. Fix it with the minimum ordering that works.
3. Change every ordering in `spsc_ring.h` to `seq_cst`. Run `bench_ring`. Report the slowdown.
4. Change the release store in `push` to relaxed. Does `bench_ring` still pass on x86? On ARM? Explain why the answers differ.

---

## Chapter 18: The SPSC Ring Buffer, Line by Line

This is the most subtle code in the system. It deserves a full walkthrough.

### 18.1 The shape

A fixed-size circular buffer with one producer thread and one consumer thread.

```
buffer_:  [ _ ][ _ ][ X ][ X ][ X ][ _ ][ _ ][ _ ]
                      ▲              ▲
                     tail           head
                  (consumer        (producer
                   reads here)      writes here)

push: write at head, then advance head
pop:  read at tail, then advance tail
empty: head == tail
full:  head - tail == Capacity
```

**SPSC is the easy case** of a lock-free queue, and choosing an architecture that only needs SPSC is a large part of why this is tractable. MPMC queues need compare-exchange loops, ABA-problem handling, and hazard pointers or epoch reclamation. SPSC needs neither — because each index has exactly one writer, a plain load/store suffices. No CAS anywhere in this file.

### 18.2 The declaration

```cpp
template <typename T, size_t Capacity>
class SPSCRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
```

`Capacity` as a non-type template parameter (Chapter 5) makes `MASK` a compile-time constant, so `head & MASK` is an `AND` with an immediate. The `static_assert` rejects a non-power-of-two at compile time.

```cpp
    struct alignas(64) PaddedIndex {
        std::atomic<size_t> val{0};
    };

    PaddedIndex head_;    // written only by producer
    PaddedIndex tail_;    // written only by consumer

    alignas(64) size_t cached_tail_ = 0;   // producer-private
    alignas(64) size_t cached_head_ = 0;   // consumer-private

    alignas(64) T buffer_[Capacity];
```

Five separately-aligned regions, each on its own cache line (Chapter 8):

```
line 0:  head_          ← producer writes, consumer reads
line 1:  tail_          ← consumer writes, producer reads
line 2:  cached_tail_   ← producer only, never shared
line 3:  cached_head_   ← consumer only, never shared
line 4+: buffer_        ← slot N written by producer, read by consumer
```

Without the padding, `head_` and `tail_` share a line and every push invalidates the consumer's copy — the 24x penalty from §7.6, on the hottest path in the system.

Note `cached_tail_`/`cached_head_` are **plain `size_t`, not atomic**. Each is read and written by exactly one thread. No synchronisation needed, so no atomic cost.

### 18.3 `push`, annotated

```cpp
bool push(const T& item) {
    const size_t head = head_.val.load(std::memory_order_relaxed);
```
Load my own index. Relaxed: no other thread writes it, so nothing to order against.

```cpp
    if (head - cached_tail_ >= Capacity) {
```
Fullness check against the **cached** tail. `cached_tail_` is a *lower bound* on the true tail (the consumer only ever increases it). So `head - true_tail <= head - cached_tail_`. If the cached check says there's room, there is definitely room — the stale value can only make us conservative, never wrong. In steady state this branch is not taken and the producer never touches the consumer's cache line at all.

```cpp
        cached_tail_ = tail_.val.load(std::memory_order_acquire);
        if (head - cached_tail_ >= Capacity)
            return false;
    }
```
The cached value said full — refresh it and check again. Acquire ordering pairs with the consumer's release store to `tail_`, guaranteeing we see the slots it has freed. If it's genuinely full, return false; the caller decides whether to spin or drop.

```cpp
    buffer_[head & MASK] = item;
```
The actual write. `head & MASK` wraps the monotonic counter to a slot index (Chapter 14's power-of-two trick again).

```cpp
    head_.val.store(head + 1, std::memory_order_release);
    return true;
}
```
**Publish.** The release store guarantees the `buffer_` write above is visible to any consumer that acquire-loads this new `head_`. Reversing these two lines breaks everything — the consumer would see the advanced index and read a slot that hasn't been written.

`head_` is incremented *monotonically* — it is never wrapped. Only the indexing wraps, via the mask. This is what makes `head - tail` a correct element count with no ambiguity between full and empty (an ambiguity that plagues implementations which wrap the indices themselves).

### 18.4 `pop`, annotated

```cpp
bool pop(T& item) {
    const size_t tail = tail_.val.load(std::memory_order_relaxed);

    if (cached_head_ <= tail) {
        cached_head_ = head_.val.load(std::memory_order_acquire);
        if (cached_head_ <= tail)
            return false;
    }

    item = buffer_[tail & MASK];
    tail_.val.store(tail + 1, std::memory_order_release);
    return true;
}
```

The mirror image. `cached_head_` is an *upper bound* check for emptiness: the producer only increases `head_`, so if the cached value says non-empty, it really is non-empty. If it says empty, refresh and recheck.

The acquire load of `head_` pairs with the producer's release store, guaranteeing that when we see `head_ == N+1`, the write to `buffer_[N]` is visible.

The release store to `tail_` publishes "slot N is free" to the producer.

`item = buffer_[tail & MASK]` is a copy. For a 64-byte `InboundMessage` that's one cache line — the compiler emits two 32-byte AVX moves, or four 16-byte SSE moves. This is why the messages are exactly one cache line: the copy is a single line transfer.

### 18.5 Why `head - tail` is always correct

Both indices are `size_t` and increase without bound. After 2⁶⁴ operations they wrap. Does the arithmetic still hold?

Yes, because unsigned subtraction also wraps (Chapter 2.2):

```
head = 3, tail = 2^64 - 2
head - tail = 3 - (2^64 - 2) mod 2^64 = 5    ← correct count
```

At 100M pushes/second, 2⁶⁴ takes ~5,800 years. But the arithmetic is correct even then, which is a nicer property than "we'll be dead first."

The alternative — wrapping the indices to `[0, Capacity)` — creates the classic full/empty ambiguity (`head == tail` means both), forcing either a wasted slot or an extra flag. Monotonic counters sidestep it entirely.

### 18.6 Why there is no compare-exchange

In an MPMC queue, two producers could both read `head == 5` and both write slot 5. You need `compare_exchange_weak` in a retry loop, plus ABA protection.

With a single producer, `head_` has exactly one writer. Nobody can change it between the load and the store. A plain load and a plain store are sufficient and correct.

**This is the entire reason SPSC is fast**: ~2.5 ns/op versus ~20–50 ns for a well-implemented MPMC queue. It is why the architecture uses two dedicated threads with dedicated rings instead of a thread pool.

### 18.7 Sizing

```cpp
constexpr size_t RING_SIZE = 1 << 18;   // 262,144 slots
```

262,144 × 64 bytes = **16 MB per ring**, 32 MB for both.

Why so large? The ring absorbs *bursts*. If the gateway receives a 10,000-message burst while the engine is mid-sweep on a deep order, the ring buffers them instead of forcing backpressure onto TCP. At 67M orders/sec of drain rate, 262K slots is ~4 ms of buffer — comfortably more than any plausible stall.

The cost is 32 MB of RAM and the fact that these must be `static` or heap-allocated, never stack (Chapter 4.1 — this is exactly the bug that segfaulted the benchmark).

### 18.8 What happens when it's full

```cpp
inbound_.push(msg);   // gateway: return value currently ignored
```

If the ring is full, `push` returns false and **the message is silently dropped**. That is a real gap, and you should name it before an interviewer does.

The options:

| Policy | Behaviour | When appropriate |
|--------|-----------|------------------|
| Drop | discard the message | market data (next tick supersedes) |
| Spin | `while (!push(m)) {}` | orders — never lose one; burns CPU |
| Backpressure | stop reading the socket; let TCP window close | orders — pushes the problem to the client, correctly |
| Reject | send `OrderRejected` | orders — explicit and honest |

For an order-entry path, silently dropping is wrong. The right fix is backpressure: stop draining the socket so the TCP receive window closes and the client is throttled by the network stack. That's what real exchanges do.

### Interview Questions

**Q: Walk me through why your ring is correct without locks.**
A: Each index has exactly one writer, so no compare-exchange is needed. The producer release-stores `head_` after writing the slot; the consumer acquire-loads `head_` before reading it. That pair guarantees the data write is visible before the index advance. The indices are monotonic and unsigned, so `head - tail` is the element count even across wrap, and there's no full/empty ambiguity.

**Q: Why cache the other side's index?**
A: To avoid touching the other core's cache line. The cached value is a conservative bound — the producer's `cached_tail_` can only understate available space — so acting on it is always safe. In steady state the producer never reads `tail_`, so a push touches only producer-owned lines. That's what gets it to 2.5 ns.

**Q: What breaks if you swap the two lines in `push`?**
A: Everything. If `head_` is published before the slot is written, the consumer can observe the advanced index and read an unwritten slot. The release store must come second — that's the whole point of the ordering.

**Q: Why SPSC rather than a general MPMC queue?**
A: SPSC needs no CAS, no ABA handling, no memory reclamation — plain loads and stores suffice. That's ~2.5 ns/op versus 20–50 ns for MPMC. I designed the threading model around getting SPSC, rather than picking a general queue and paying for generality I don't need.

**Q: What happens if the ring fills?**
A: Currently `push` returns false and the gateway ignores it — the message is dropped. That's acceptable for market data but wrong for order entry. The correct fix is backpressure: stop draining the socket so the TCP window closes and the client is throttled. I'd do that before shipping.

### Exercises

1. Swap the two lines in `push` (store before write). Run the cross-core test until it corrupts. How many iterations did it take? Why is it so rare?
2. Remove `cached_tail_`/`cached_head_` and always load the real index. Benchmark. Explain the gap in terms of cache-line transfers.
3. Remove `alignas(64)` from `PaddedIndex`. Benchmark. Now also remove it from `buffer_`. Report both.
4. Implement backpressure in the gateway: when `push` fails, deregister the client fd from the event loop until the ring drains below half. Test with a client that floods faster than the engine drains.
5. Write an MPMC ring with `compare_exchange_weak`. Benchmark it against the SPSC version with one producer. Quantify the cost of generality.
6. Prove the cached-index optimisation is safe: state the invariant on `cached_tail_` relative to the true tail and show that acting on a stale value can never overflow the buffer.

---

# PART V — MARKET MICROSTRUCTURE

You cannot defend a matching engine without knowing what it is matching, or why the rules are the way they are. This part is the domain knowledge.

---

## Chapter 19: What an Exchange Actually Does

### 19.1 The one-sentence definition

An exchange is a **continuous double auction**: buyers and sellers submit orders, the exchange maintains a public queue of unmatched ones, and whenever a buy price meets a sell price it executes a trade — by a published, deterministic, non-discretionary rule.

Everything else — the protocols, the colocation, the microwave towers — is detail around that core.

### 19.2 The four responsibilities

1. **Order management** — accept, acknowledge, amend, cancel. Every order gets a unique ID and a definitive state.
2. **Matching** — apply the priority rules. Deterministic: the same order sequence must always produce the same trades.
3. **Market data** — publish the resulting book and trade stream to everyone simultaneously.
4. **Fairness** — identical treatment for all participants. This is a regulatory obligation, not a courtesy.

TradeFeed implements 1, 2, and a simplified 3.

### 19.3 Why determinism is non-negotiable

Given the same input sequence, the engine must produce the same output. Always.

- **Regulation.** Under MiFID II and Reg NMS, an exchange must be able to reconstruct and justify any trade.
- **Disputes.** "Why did my order not fill?" needs an answer derivable from the rules and the tape.
- **Recovery.** A failed engine is restored by replaying the input log into a fresh instance. That only works if replay is deterministic.
- **Testing.** Non-deterministic systems cannot be regression-tested.

This is why TradeFeed's engine is **single-threaded**. Parallelising the matching of a single book would make execution order depend on thread scheduling — and then the same inputs could produce different trades. Real exchanges shard *by symbol* (each symbol's book on one thread) and never within a book.

**That is the answer to "why don't you parallelise the matching engine?"** Not because it's hard — because it would be wrong. The engine at 67M orders/sec is nowhere near being the bottleneck anyway.

### 19.4 Price representation and ticks

A **tick** is the minimum price increment. US equities above $1.00 tick at $0.01; below $1.00 at $0.0001. Futures vary by contract.

Ticks exist to stop sub-penny undercutting — without them a participant could jump the queue by improving one hundred-thousandth of a cent, which is economically meaningless but grants full price priority.

TradeFeed represents every price as an integer count of ticks (Chapter 2.4), which is what makes the direct-indexed price array possible (Chapter 13).

### 19.5 The order lifecycle

```
                 ┌──────────┐
   client ──────▶│   NEW    │
                 └────┬─────┘
                      │ validate
          ┌───────────┴───────────┐
          ▼                       ▼
    ┌──────────┐            ┌──────────┐
    │ REJECTED │            │ ACCEPTED │
    └──────────┘            └────┬─────┘
                                 │ attempt match
                 ┌───────────────┼───────────────┐
                 ▼               ▼               ▼
          ┌────────────┐  ┌─────────────┐  ┌──────────┐
          │   FILLED   │  │   PARTIAL   │  │  RESTING │
          └────────────┘  └──────┬──────┘  └────┬─────┘
                                 │               │
                                 └───────┬───────┘
                                         ▼
                              ┌──────────────────────┐
                              │  CANCELLED / FILLED  │
                              └──────────────────────┘
```

Every transition emits a message to the owning client. That is what `MatchingEngine::emit_*` does.

### Interview Questions

**Q: What does an exchange do, in one sentence?**
A: It runs a continuous double auction — maintaining a queue of unmatched buy and sell orders and executing trades by a published, deterministic priority rule whenever the prices cross.

**Q: Why is your matching engine single-threaded?**
A: Determinism. Parallelising a single book would make execution order depend on thread scheduling, so identical inputs could produce different trades — which breaks regulatory reconstruction, replay-based recovery, and regression testing. Real exchanges shard by symbol, never within a book. At 67M orders/sec the engine isn't the bottleneck anyway.

### Exercises

1. Write out the message sequence a client sees for: a limit buy that partially fills, rests, then is cancelled.
2. Research the tick-size regime for US equities under Reg NMS Rule 612. What changes below $1.00 and why?
3. Explain how you'd recover a crashed matching engine from an input log. What must be true of the engine for that to work?

---

## Chapter 20: The Order Book

### 20.1 Anatomy

The book holds all resting (unmatched) orders, organised by side and price:

```
        ASKS (sellers)                         Price      Size    Orders
        ─────────────                       ─────────────────────────────
                                            $150.05      1,200      4
                                            $150.04        800      2
                                            $150.03      2,500      7
        ┌── best ask (lowest sell) ────────  $150.02        500      1
        │
        │   ← SPREAD = $0.01 →
        │
        └── best bid (highest buy) ────────  $150.01        900      3
                                            $150.00      3,100     11
                                            $149.99      1,500      5
        BIDS (buyers)
```

**Key terms** (you will be asked these):

- **Bid** — a buy order. Bidders want low prices.
- **Ask / Offer** — a sell order. Askers want high prices.
- **Best bid** — the *highest* buy price resting.
- **Best ask** — the *lowest* sell price resting.
- **BBO (Best Bid and Offer)** — the pair. Also called top of book or Level 1.
- **Spread** — best ask minus best bid. Always ≥ 0 in a valid book (see 20.2).
- **Mid** — (best bid + best ask) / 2. The usual "fair value" proxy.
- **Depth** — total quantity resting at a level, or cumulative across several.
- **Level 1 / Level 2** — BBO only / full depth by price.

In TradeFeed:

```cpp
Price best_bid_ = INVALID_PRICE;    // 0        — sentinel for "no bids"
Price best_ask_ = MAX_PRICE + 1;    // 1000001  — sentinel for "no asks"
```

### 20.2 The crossed-book invariant

**`best_bid_ < best_ask_` must hold at all times a message is not being processed.**

If a bid at $150.02 and an ask at $150.02 both rested, they should have traded. A crossed book means the matcher failed.

The engine maintains this by construction: an incoming order matches against the opposite side *before* it is allowed to rest. By the time `add_order` inserts the remainder, everything crossable has already been consumed.

This is the single best invariant to assert in a test:

```cpp
assert(book.best_bid() == INVALID_PRICE ||
       book.best_ask() >  MAX_PRICE     ||
       book.best_bid() <  book.best_ask());
```

### 20.3 Order types

**Limit order** — "buy up to $150.02, no worse." Executes at the limit or better; any unfilled remainder rests in the book. Limit orders are what *provide* liquidity.

```cpp
book.add_order(id, client, Side::Buy, OrderType::Limit, 1'500'200, 100, ts);
```

**Market order** — "buy 100 shares at whatever the market offers." No price limit; sweeps the book until filled. Never rests — any unfilled remainder is discarded (in TradeFeed; real venues may convert to limit or cancel). Market orders *consume* liquidity.

```cpp
book.add_order(id, client, Side::Buy, OrderType::Market, 0, 100, ts);
```

Market orders carry **slippage risk**: a large one sweeps multiple levels and fills at progressively worse prices.

Real venues also have IOC (immediate-or-cancel), FOK (fill-or-kill), post-only, iceberg, stop, and more. TradeFeed implements the two fundamental ones; the rest are policy layers over the same matching core, which is the right way to describe them in an interview.

### 20.4 Maker and taker

- **Maker** — the resting order. It *made* liquidity by posting and waiting.
- **Taker** — the incoming aggressive order. It *took* liquidity by crossing the spread.

Exchanges typically charge the taker and rebate the maker (the "maker-taker" model) to incentivise posted liquidity. This is why market makers care intensely about queue position: being early in the FIFO at a price level means filling more often and earning more rebate.

In TradeFeed's `drain_level`, the `aggressor` is the taker and every `resting` order is a maker.

### 20.5 The fill price rule

**A trade executes at the resting (maker) order's price, never the aggressor's.**

```cpp
fills_.push_back({aggressor->id, resting->id,
                  aggressor->client_id, resting->client_id,
                  resting->price,          // ← the maker's price
                  fill_q});
```

Example: best ask is $150.02. A buy limit arrives at $150.10. It executes at **$150.02**, not $150.10.

Why: the maker's price was public and committed first. The taker's willingness to pay more is private information and doesn't set the price. The taker receives **price improvement** — they got a better fill than their limit. If the rule were reversed, posting a limit order would be dangerous (you'd be filled at arbitrary prices), and nobody would provide liquidity.

This is a favourite interview question because getting it backwards reveals you haven't thought about incentives.

### Interview Questions

**Q: What's the spread and why is it never negative?**
A: Best ask minus best bid. It can't be negative in a settled book because the engine matches an incoming order against the opposite side before letting it rest — anything crossable has already traded. A crossed book is a matcher bug.

**Q: A buy limit at $150.10 hits a resting ask at $150.02. What's the fill price?**
A: $150.02 — the maker's price. The resting order's price was public and committed first; the taker gets price improvement. Reversing it would make posting limit orders unsafe and destroy liquidity provision.

**Q: Difference between a market and a limit order?**
A: A limit has a worst acceptable price and rests if unfilled — it provides liquidity. A market has no price limit, sweeps the book until filled, never rests — it consumes liquidity and carries slippage risk on size.

### Exercises

1. Draw a book with five levels per side. Apply: a buy limit inside the spread; a large market sell; a cancel of the best bid. Redraw after each.
2. Write a test asserting the crossed-book invariant after every operation in `bench_mixed_workload`.
3. Implement IOC: match what you can, discard the remainder instead of resting. How much of `add_order` changes?
4. Compute the slippage on a market buy for 5,000 shares against the book in §20.1. What's the average fill price versus the mid?

---

## Chapter 21: Price-Time Priority Matching

### 21.1 The rule

When multiple resting orders could fill an incoming order, which fills first?

**1. Price priority.** The best-priced order goes first — highest bid, lowest ask. Offering a better price always wins.

**2. Time priority.** Among orders at the *same* price, the one that arrived first fills first. FIFO.

That is "price-time priority", used by NASDAQ, NYSE, LSE, and most equity venues.

The alternative, **pro-rata** (used on some futures and options venues), splits fills proportionally to order size at a price level. It rewards size rather than speed and produces very different market-maker behaviour — worth knowing the name and the contrast.

Time priority is why queue position matters and, ultimately, why low-latency infrastructure exists: arriving one microsecond earlier at a price level can mean being ahead of thousands of shares in the queue.

### 21.2 How the data structures encode the rule

Price priority is the **outer loop**: start at `best_ask_` and walk outward.
Time priority is the **inner loop**: `level.front()` is the oldest order, because `push` appends at the tail.

**The entire matching rule falls out of the data structures.** There is no sorting step, no comparator, no priority queue. The intrusive FIFO gives time priority for free; the price-indexed array plus `best_ask_` gives price priority for free. That is the clean answer to "how do you implement price-time priority efficiently."

### 21.3 `drain_level` — the inner loop

```cpp
void OrderBook::drain_level(PriceLevel& level, Order* aggressor, Side aggressor_side) {
    while (!level.empty() && !aggressor->is_filled()) {
        Order* resting  = level.front();                                  // (1) time priority
        Quantity fill_q = std::min(aggressor->remaining(), resting->remaining());  // (2)

        aggressor->fill(fill_q);                                          // (3)
        resting->fill(fill_q);
        level.adjust_qty(-static_cast<int32_t>(fill_q));                  // (4)

        if (aggressor_side == Side::Buy)                                  // (5)
            fills_.push_back({aggressor->id, resting->id,
                              aggressor->client_id, resting->client_id,
                              resting->price, fill_q});
        else
            fills_.push_back({resting->id, aggressor->id,
                              resting->client_id, aggressor->client_id,
                              resting->price, fill_q});
        ++match_count_;

        if (resting->is_filled()) {                                       // (6)
            level.remove(resting);
            unreg(resting);
            pool_.deallocate(resting);
        }
    }
}
```

**(1)** `front()` is the head of the intrusive list — the oldest order at this price. Time priority.

**(2)** The fill is the smaller of the two remaining quantities. `std::min` compiles to `CMOV` — branchless, which matters because which side is larger is genuinely unpredictable (Chapter 9.4).

**(3)** Both orders' `filled_qty` increases. Either or both may now be complete.

**(4)** The level's aggregate drops by the fill. This is the `adjust_qty` path from Chapter 12.6 — the resting order is still in the list (if partially filled), so the counter must be corrected in place.

**(5)** The `Fill` record is normalised to always store `(bid_id, ask_id)`, so downstream code never has to ask which side was aggressive. The price is always `resting->price` — the maker's price (Chapter 20.5). The branch is loop-invariant and perfectly predicted.

**(6)** A fully-filled resting order leaves the book. Three steps, and **all three are mandatory**: unlink from the level, clear the lookup-table entry, return the slot to the pool. Omitting `unreg` leaves a dangling pointer that a later cancel could follow — the failure mode the `o->id == id` check in Chapter 14.5 exists to catch.

**Loop exit**: either the level is exhausted (walk to the next price) or the aggressor is fully filled (done).

### 21.4 `match_limit` — the outer loop

```cpp
void OrderBook::match_limit(Order* aggressor) {
    if (aggressor->side == Side::Buy) {
        while (!aggressor->is_filled() && best_ask_ <= aggressor->price && best_ask_ <= MAX_PRICE) {
            drain_level(asks_[idx(best_ask_)], aggressor, Side::Buy);
            if (asks_[idx(best_ask_)].empty()) scan_best_ask();
        }
    } else {
        while (!aggressor->is_filled() && best_bid_ >= aggressor->price && best_bid_ != INVALID_PRICE) {
            drain_level(bids_[idx(best_bid_)], aggressor, Side::Sell);
            if (bids_[idx(best_bid_)].empty()) scan_best_bid();
        }
    }
}
```

Three conditions on the buy loop:

- `!aggressor->is_filled()` — stop when satisfied.
- `best_ask_ <= aggressor->price` — **the limit price constraint**. Stop when the cheapest ask exceeds what we're willing to pay.
- `best_ask_ <= MAX_PRICE` — the empty-book sentinel. With no asks, `best_ask_` is `MAX_PRICE + 1`, so this is false and the loop exits without a null check.

After draining a level, if it's now empty, rescan for the next best price (Chapter 13.7). The loop then re-tests against the new `best_ask_` — which may now exceed the limit, ending the sweep.

The sell branch mirrors it: walk `best_bid_` downward while it's at or above the limit.

### 21.5 `match_market`

```cpp
void OrderBook::match_market(Order* aggressor) {
    if (aggressor->side == Side::Buy) {
        while (!aggressor->is_filled() && best_ask_ <= MAX_PRICE) {
            drain_level(asks_[idx(best_ask_)], aggressor, Side::Buy);
            if (asks_[idx(best_ask_)].empty()) scan_best_ask();
        }
    } else { /* mirror */ }
}
```

Identical minus the price constraint. A market order sweeps until filled or the book is empty. This is where slippage comes from — later fills occur at progressively worse prices, and each `Fill` records the actual level it hit.

### 21.6 `add_order` — the full path

```cpp
bool OrderBook::add_order(OrderId id, ClientId client, Side side, OrderType type,
                          Price price, Quantity qty, Timestamp ts) {
    if (pool_.available() == 0) return false;                                    // (1)
    if (type == OrderType::Limit && (price < MIN_PRICE || price > MAX_PRICE))
        return false;                                                            // (2)

    Order* o = pool_.allocate();                                                 // (3)
    o->id = id;  o->client_id = client;  o->price = price;
    o->quantity = qty;  o->filled_qty = 0;
    o->side = side;  o->type = type;  o->timestamp = ts;
    o->prev = nullptr;  o->next = nullptr;

    reg(o);                                                                      // (4)

    if (type == OrderType::Market) match_market(o);                              // (5)
    else                           match_limit(o);

    if (!o->is_filled() && type == OrderType::Limit) {                           // (6)
        if (side == Side::Buy) {
            bids_[idx(price)].push(o);
            if (price > best_bid_) best_bid_ = price;
        } else {
            asks_[idx(price)].push(o);
            if (price < best_ask_) best_ask_ = price;
        }
    } else {                                                                     // (7)
        unreg(o);
        pool_.deallocate(o);
    }
    return true;
}
```

**(1)(2)** Validation at the boundary. Capacity and price range. These are the only runtime checks — everything downstream can assume validity (Chapter 11.6).

**(3)** Pool allocation. Every field assigned explicitly; nothing inherited from the previous occupant.

**(4)** Register in the lookup table *before* matching, so a fill can be attributed and a subsequent cancel can find it.

**(5)** **Match before resting.** This is what maintains the crossed-book invariant (Chapter 20.2). The order only rests after everything crossable is consumed.

**(6)** Rest the remainder. `push` appends at the tail — joining the back of the FIFO, establishing time priority. Update the BBO if this order improves it: O(1).

**(7)** Fully filled, or a market order with unfilled remainder. Unregister and return to the pool. A market order never rests.

### 21.7 `cancel_order`

```cpp
bool OrderBook::cancel_order(OrderId id) {
    Order* o = lookup(id);
    if (!o) return false;                                        // unknown or already gone

    Price p = o->price;
    if (o->side == Side::Buy) {
        bids_[idx(p)].remove(o);
        if (bids_[idx(p)].empty() && p == best_bid_) scan_best_bid();
    } else {
        asks_[idx(p)].remove(o);
        if (asks_[idx(p)].empty() && p == best_ask_) scan_best_ask();
    }

    unreg(o);
    pool_.deallocate(o);
    return true;
}
```

The rescan is guarded by **two** conditions — the level must now be empty *and* it must have been the best price. Cancelling one of fifty orders at the BBO doesn't change the BBO; cancelling deep in the book doesn't either. Most cancels skip the scan entirely, which is why the measured cancel p50 is under 42 ns despite the scan's unbounded worst case.

### 21.8 `modify_order`

```cpp
bool OrderBook::modify_order(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
    Order* o = lookup(id);
    if (!o) return false;

    Side     side   = o->side;
    ClientId client = o->client_id;
    cancel_order(id);                                              // frees o
    return add_order(id, client, side, OrderType::Limit, new_price, new_qty, ts);
}
```

Cancel-and-replace. `side` and `client` are copied out *before* the cancel, because `cancel_order` returns the `Order` to the pool and the pointer is immediately reusable.

**The semantic consequence: modification loses queue position.** The replacement joins the back of the FIFO at its price level.

Is that right? It's what most exchanges do, and the justification is fairness: a price change is a new economic commitment and shouldn't keep priority earned at a different price. Some venues preserve priority for a pure quantity *decrease* (you're not asking for more, so you keep your place) — a refinement TradeFeed doesn't implement, but knowing it exists is the senior answer.

### 21.9 A complete worked example

Starting book:

```
ASKS:  $150.03 → [ D(200) ]
       $150.02 → [ B(100), C(50) ]      B arrived before C
BIDS:  $150.00 → [ A(300) ]
best_bid_ = 15000,  best_ask_ = 15002
```

Incoming: **buy limit, 250 shares, limit $150.03**.

```
match_limit, side = Buy
├─ iteration 1: best_ask_ = 15002 <= 15003 ✓
│  drain_level(asks_[15002]):
│    ├─ resting = B (front → time priority)
│    │  fill_q = min(250, 100) = 100
│    │  aggressor.filled = 100, B.filled = 100
│    │  level.adjust_qty(-100)  → level total 150 → 50
│    │  Fill{bid=agg, ask=B, price=15002, qty=100}   ← maker's price
│    │  B is filled → remove, unreg, deallocate
│    ├─ resting = C
│    │  fill_q = min(150, 50) = 50
│    │  aggressor.filled = 150, C.filled = 50
│    │  level.adjust_qty(-50)   → level total 0
│    │  Fill{bid=agg, ask=C, price=15002, qty=50}
│    │  C is filled → remove, unreg, deallocate
│    └─ level empty → exit inner loop
│  asks_[15002] empty → scan_best_ask() → best_ask_ = 15003
│
├─ iteration 2: best_ask_ = 15003 <= 15003 ✓
│  drain_level(asks_[15003]):
│    ├─ resting = D
│    │  fill_q = min(100, 200) = 100
│    │  aggressor.filled = 250 (complete), D.filled = 100
│    │  level.adjust_qty(-100)  → level total 100
│    │  Fill{bid=agg, ask=D, price=15003, qty=100}
│    │  D NOT filled (100 of 200 remain) → stays in book
│    └─ aggressor filled → exit inner loop
│  level not empty → no rescan
│
└─ aggressor->is_filled() → exit outer loop

add_order step (6): aggressor IS filled → unreg + deallocate (never rests)
```

Resulting book:

```
ASKS:  $150.03 → [ D(200, 100 filled → 100 remaining) ]
BIDS:  $150.00 → [ A(300) ]
best_bid_ = 15000,  best_ask_ = 15003
```

Three fills emitted, total 250 shares. Average fill price = (100×150.02 + 50×150.02 + 100×150.03) / 250 = **$150.024** — the slippage from sweeping two levels.

**Be able to reproduce this trace on a whiteboard.** It is the single most likely practical exercise in a matching-engine interview.

### Interview Questions

**Q: Implement price-time priority. What data structures?**
A: Price priority is the outer loop over an array of price levels indexed by price, starting from the cached best. Time priority is the inner loop over an intrusive FIFO at each level — `push` appends at the tail, `front()` returns the oldest. The rule falls out of the structures; there's no sort or comparator anywhere.

**Q: Walk me through matching a 250-share buy against a book with 150 at the touch and 200 one tick up.**
A: *(reproduce §21.9)*

**Q: Why does an order lose queue priority when modified?**
A: A modification is a new economic commitment, so it shouldn't retain priority earned at a different price. I implement it as cancel-and-replace. Some venues preserve priority for a pure quantity decrease, since that's not asking for more — I haven't implemented that refinement.

**Q: Why match before inserting?**
A: To maintain the no-crossed-book invariant. If I inserted first, a buy at $150.02 would rest alongside an ask at $150.02 — two orders that should have traded. Matching first guarantees only the non-crossable remainder ever rests.

### Exercises

1. Trace §21.9 by hand on paper. Then add a `printf` to `drain_level` and verify against the real output.
2. Construct the case where `match_limit` exits because the price constraint fails mid-sweep (not because the aggressor filled). Write it as a test.
3. Implement pro-rata allocation as an alternative to FIFO within a level. How does the fill distribution change for a 250-share order against B(100) and C(50)?
4. Add an `assert` for the crossed-book invariant at the end of `add_order` and `cancel_order`. Run the full benchmark suite.
5. Implement priority-preserving quantity *decrease* in `modify_order`. Which call sites change?
6. What happens if a market order arrives on an empty book? Trace it and confirm no memory is leaked.

---

## Chapter 22: Market Analytics

The book is not just a matching structure — it is a signal. This chapter covers the metrics in `book_analytics.h`.

### 22.1 Spread, mid, and why they matter

```cpp
static Price spread(const OrderBook& book) {
    if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE) return 0;
    return book.best_ask() - book.best_bid();
}

static double mid(const OrderBook& book) {
    if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE) return 0.0;
    return (book.best_bid() + book.best_ask()) / 2.0;
}
```

Both guard against a one-sided book using the sentinels from Chapter 20.1.

**Spread** is the round-trip cost of immediacy: buy at the ask, sell at the bid, and you've paid the spread. It's the market maker's gross compensation for providing liquidity, and it widens with volatility, thinness, and perceived toxicity (§22.3).

**Mid** is the standard fair-value proxy. Note `/ 2.0` — deliberate floating point, because a mid can legitimately fall between ticks. This is *analytics*, not the matching path, so floating point is fine here. The rule from Chapter 2.4 is about prices that must compare exactly, not derived statistics.

### 22.2 VWAP

```cpp
void on_fill(Price price, Quantity qty, Side aggressor_side) {
    total_volume_ += qty;
    vwap_numer_   += static_cast<double>(price) * qty;
    ...
}
double vwap() const { return total_volume_ > 0 ? vwap_numer_ / total_volume_ : 0.0; }
```

$$\text{VWAP} = \frac{\sum_i p_i q_i}{\sum_i q_i}$$

The average price weighted by volume. Computed **incrementally** — a running numerator and denominator, O(1) per fill, no trade history stored.

Why it matters: VWAP is the standard execution benchmark. An institution filling 500,000 shares over a day is measured against the day's VWAP; beating it means the execution algorithm worked. Entire trading desks exist to hit VWAP.

`static_cast<double>(price) * qty` — the cast prevents integer overflow. `1'500'200 × 10'000` exceeds `uint32_t` and would silently wrap.

### 22.3 VPIN — flow toxicity

The most sophisticated metric here, and the best one to raise in an interview because it shows you understand *why* market makers behave as they do.

**The problem it measures.** A market maker quotes both sides and profits from the spread — provided their counterparties are uninformed. If the counterparty knows something (an impending news release, a large parent order), the maker is systematically picked off: they buy just before the price falls and sell just before it rises. This is **adverse selection**, and the flow causing it is **toxic**.

**VPIN** (Volume-Synchronized Probability of Informed Trading, Easley–López de Prado–O'Hara, 2012) estimates it:

```cpp
static constexpr size_t   VPIN_BUCKETS  = 50;
static constexpr Quantity BUCKET_VOLUME = 10'000;

if (aggressor_side == Side::Buy) current_buy_vol_  += qty;
else                             current_sell_vol_ += qty;
current_bucket_vol_ += qty;

if (current_bucket_vol_ >= BUCKET_VOLUME) {
    Quantity total = current_buy_vol_ + current_sell_vol_;
    double imbalance = total > 0
        ? std::abs(static_cast<double>(current_buy_vol_) - current_sell_vol_) / total
        : 0.0;
    vpin_history_[vpin_idx_ % VPIN_BUCKETS] = imbalance;
    ++vpin_idx_;
    if (vpin_count_ < VPIN_BUCKETS) ++vpin_count_;
    current_buy_vol_ = current_sell_vol_ = current_bucket_vol_ = 0;
}
```

$$\text{VPIN} = \frac{1}{n}\sum_{b=1}^{n} \frac{|V_b^{buy} - V_b^{sell}|}{V_b^{buy} + V_b^{sell}}$$

**The key design choice is volume-time, not clock-time.** Buckets close after a fixed *volume* (10,000 shares), not a fixed duration. Information arrives with volume, not with the clock — a quiet minute and a frantic minute are not comparable samples, but 10,000 shares is always 10,000 shares. This also makes the metric self-normalising across instruments and sessions.

**Interpretation.** Balanced flow (buys ≈ sells) → imbalance near 0 → uninformed two-way trading. Persistently one-sided flow → imbalance near 1 → someone is accumulating, and they probably know why. VPIN above ~0.7 historically precedes volatility events; it was notably elevated before the May 2010 Flash Crash, which is what made the metric famous.

**What a maker does with it.** Rising VPIN → widen quotes, reduce size, or withdraw. This is exactly why liquidity evaporates during stress: it is not panic, it is every maker's toxicity model firing simultaneously.

The `vpin_history_` array is a fixed-size ring (`std::array<double, 50>`, indexed `% VPIN_BUCKETS`) — bounded memory, no allocation, rolling window.

### 22.4 Order book imbalance

```cpp
static double imbalance(const OrderBook& book) {
    if (book.best_bid() == INVALID_PRICE || book.best_ask() > MAX_PRICE) return 0.0;
    Quantity bq = book.bid_qty_at(book.best_bid());
    Quantity aq = book.ask_qty_at(book.best_ask());
    Quantity total = bq + aq;
    return total > 0 ? static_cast<double>(static_cast<int64_t>(bq) - aq) / total : 0.0;
}
```

$$I = \frac{Q_{bid} - Q_{ask}}{Q_{bid} + Q_{ask}} \in [-1, +1]$$

Where VPIN measures *executed* flow, imbalance measures *resting* intent. +1 means all resting size is on the bid (buying pressure); −1 means all on the ask.

It is one of the strongest short-horizon price predictors known — a heavily bid-skewed book usually ticks up next. Most high-frequency market-making models use it as a core feature.

**The `static_cast<int64_t>` is a genuine bug fix, not decoration.** `bq` and `aq` are `uint32_t`. If `aq > bq`, then `bq - aq` wraps to a huge positive number and the function returns nonsense instead of a negative imbalance. Promoting to `int64_t` makes the subtraction signed and correct. This is the Chapter 2.2 unsigned-wrap hazard appearing in real analytics code — and the compiler's static analyser flagged it during development, which is a good story to tell about tooling.

### 22.5 Depth

```cpp
static Quantity bid_depth(const OrderBook& book, int levels) {
    Quantity total = 0;
    Price p = book.best_bid();
    for (int i = 0; i < levels && p >= MIN_PRICE; ++i, --p) {
        total += book.bid_qty_at(p);
        if (p == MIN_PRICE) break;
    }
    return total;
}
```

Cumulative resting quantity across the top N levels. Answers "how much can I trade before moving the price by N ticks?" — the practical measure of liquidity, and the input to slippage estimates.

Note the `if (p == MIN_PRICE) break;` guard again — the same unsigned-decrement hazard as `scan_best_bid` (Chapter 13.7). It appears wherever you walk prices downward.

O(levels), and levels near the touch are L1-resident, so a 10-level depth query is ~10 L1 hits.

### Interview Questions

**Q: What is VPIN and why volume buckets rather than time buckets?**
A: It estimates the share of order flow that's informed, by measuring buy/sell volume imbalance within fixed-volume buckets and averaging over a rolling window. Volume-time because information arrives with volume, not with the clock — a quiet minute and a frantic minute aren't comparable samples, but 10,000 shares always is. High VPIN means adverse selection risk, so makers widen or withdraw; it was elevated before the 2010 Flash Crash.

**Q: Why is order book imbalance predictive?**
A: It's revealed resting intent. If resting bid size heavily exceeds ask size, buyers are more eager than sellers and the next tick is more likely up. It's one of the strongest short-horizon predictors and a standard feature in market-making models.

**Q: You said never use floating point for prices, but `mid()` returns a double.**
A: The rule is about prices that must compare and match exactly — the matching path is entirely integer. A mid can legitimately fall between ticks, and it's a derived statistic that never feeds back into matching. Using a double there is correct; using one for `best_bid_` would not be.

**Q: Why the `int64_t` cast in `imbalance`?**
A: `bq` and `aq` are unsigned. When asks exceed bids, `bq - aq` wraps to a huge positive value and the function returns garbage instead of a negative imbalance. Promoting to signed 64-bit fixes it. The static analyser caught it during development.

### Exercises

1. Wire `BookAnalytics::on_fill` into `MatchingEngine::emit_fills` and print VWAP and VPIN every 10,000 fills during `bench_mixed_workload`.
2. Write a test that drives the book with 100% one-sided aggressive flow and assert VPIN converges to 1.0. Then alternate sides and assert it converges to 0.
3. Remove the `int64_t` cast from `imbalance`. Construct a book where asks exceed bids and print the result. Explain the number.
4. Implement `slippage_estimate(book, side, qty)` using `bid_depth`/`ask_depth` — return the expected average fill price for a market order of that size.
5. Add a rolling realised-volatility estimator over the last N mid-price updates. What data structure, and why?

---

# PART VI — THE SYSTEM

Parts I–V covered the pieces. Part VI assembles them into a running exchange.

---

## Chapter 23: Sockets and the Network Stack

### 23.1 What a socket is

A **socket** is a file descriptor — a small integer the kernel maps to an open resource. The BSD socket API, unchanged since 1983, is the universal interface.

```cpp
int fd = socket(AF_INET, SOCK_STREAM, 0);
```

- `AF_INET` — IPv4 address family (`AF_INET6` for IPv6).
- `SOCK_STREAM` — reliable, ordered, connection-oriented → TCP. (`SOCK_DGRAM` → UDP.)
- `0` — default protocol for that type.

Returns a file descriptor, or −1 on error.

### 23.2 The server sequence

```cpp
socket()  →  setsockopt()  →  bind()  →  listen()  →  accept()  →  recv()/send()  →  close()
```

**`bind`** — attach the socket to a local address and port.

```cpp
sockaddr_in addr{};
addr.sin_family      = AF_INET;
addr.sin_port        = htons(port);        // host → network byte order
addr.sin_addr.s_addr = INADDR_ANY;         // all interfaces
bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
```

`htons` = "host to network short". Network byte order is big-endian; x86 and ARM are little-endian, so ports and addresses must be converted. Forgetting `htons` on the port is the classic beginner bug — you bind to a byte-swapped port and nothing connects.

`reinterpret_cast<sockaddr*>` — the API takes a generic `sockaddr*` but you fill in the IPv4-specific `sockaddr_in`. This is C-era polymorphism and one of the few legitimate uses of `reinterpret_cast` (Chapter 2.7).

**`listen`** — mark the socket passive and set the backlog.

```cpp
listen(listen_fd_, LISTEN_BACKLOG);   // 16
```

The backlog bounds the queue of completed-but-not-yet-accepted connections. Overflow means new connections are refused or dropped.

**`accept`** — pull one connection off that queue, returning a *new* fd for that client. The listening socket stays open for more.

### 23.3 `SO_REUSEADDR`

```cpp
int one = 1;
setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
```

After a socket closes, TCP keeps the port in `TIME_WAIT` for up to 2×MSL (~60 s) to absorb stray packets from the old connection. Without `SO_REUSEADDR`, restarting the server within that window fails with `EADDRINUSE` — the "why can't I restart my server immediately" problem every network programmer hits once.

### 23.4 `TCP_NODELAY` — disabling Nagle

```cpp
int one = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
```

**Nagle's algorithm** (1984) batches small writes: if there is unacknowledged data outstanding, buffer new small writes until the ACK arrives or a full segment accumulates. It was designed to stop telnet flooding the network with 41-byte packets carrying one keystroke.

For us it is a disaster. A 32-byte order acknowledgement would sit in the kernel buffer waiting for an ACK — **up to 40 ms** on a typical stack, and pathologically up to 200 ms when it interacts with delayed ACK.

40 ms is roughly 400,000 times our engine's per-order latency. Every real trading connection sets `TCP_NODELAY`. It is the single most important socket option in this file, and a guaranteed interview question.

### 23.5 `SO_NOSIGPIPE` and `MSG_NOSIGNAL`

Writing to a socket whose peer has closed raises `SIGPIPE`, which by default **terminates the process**. A client disconnecting must not kill the exchange.

Two platform-specific fixes:

```cpp
#ifdef __APPLE__
    setsockopt(listen_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));   // per-socket
#endif

int flags = 0;
#ifdef __linux__
    flags = MSG_NOSIGNAL;                                                   // per-call
#endif
send(clients_[i].fd, &wire, WIRE_MSG_SIZE, flags);
```

macOS uses the socket option; Linux uses the send flag. (Linux does not define `SO_NOSIGPIPE`; macOS does not define `MSG_NOSIGNAL`.) With either, the write returns `EPIPE` instead of signalling — an error you can handle.

This is why the code branches on platform rather than using one mechanism.

### 23.6 Non-blocking I/O

```cpp
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

By default `recv` blocks until data arrives. A blocked gateway thread cannot drain the outbound ring, so responses stall behind an idle client.

With `O_NONBLOCK`, `recv` returns immediately: −1 with `errno == EAGAIN` (or `EWOULDBLOCK`, the same value on Linux and macOS) when there is nothing to read. The thread stays free.

The read/error contract:

```cpp
ssize_t n = recv(fd, c.read_buf + c.read_pos, WIRE_MSG_SIZE - c.read_pos, 0);
if (n <= 0) {
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        disconnect(slot);
    return;
}
```

| Return | Meaning | Action |
|--------|---------|--------|
| `n > 0` | bytes received | advance buffer |
| `n == 0` | peer closed cleanly (FIN) | disconnect |
| `n < 0`, `EAGAIN` | no data available right now | return, try later |
| `n < 0`, other | real error | disconnect |

Conflating `n == 0` with `EAGAIN` is the classic bug: you either spin forever on a closed socket or drop live ones.

### 23.7 TCP is a byte stream, not a message stream

**This is the single most misunderstood thing about TCP.** It guarantees ordered, reliable *bytes*. It does not preserve message boundaries. A 32-byte `send` may arrive as 32 bytes, or 20 then 12, or be coalesced with the next message into 64.

So the receiver must reassemble. TradeFeed keeps a per-client accumulator:

```cpp
struct ClientState {
    int    fd       = -1;
    bool   active   = false;
    char   read_buf[WIRE_MSG_SIZE];
    size_t read_pos = 0;          // bytes accumulated so far
};

c.read_pos += n;
if (c.read_pos >= WIRE_MSG_SIZE) {
    WireMessage wire;
    std::memcpy(&wire, c.read_buf, WIRE_MSG_SIZE);
    c.read_pos = 0;
    // ... translate and push ...
}
```

A partial message leaves `read_pos` mid-buffer; the next readable event continues filling it. Only when a full 32 bytes are present is a message parsed.

The `memcpy` into an aligned local is the safe way to read a packed struct out of a byte buffer (Chapter 3.7) — casting the buffer pointer directly would be an alignment violation and, strictly, a strict-aliasing violation.

**Known limitation worth naming**: the `for (;;)` read loop parses at most one message per outer iteration and resets `read_pos = 0`, so a read that delivers 64 bytes (two messages) parses the first and discards the second. The fix is a loop that consumes every complete message in the buffer and memmoves the remainder. Fixed-size framing makes this straightforward; it is on the list in Chapter 31.

### Interview Questions

**Q: Why `TCP_NODELAY`?**
A: Nagle's algorithm buffers small writes until an ACK returns or a full segment accumulates — up to 40 ms, and up to 200 ms interacting with delayed ACK. Our acknowledgements are 32 bytes, so every one would be delayed. That's ~400,000× the engine's latency. Every trading connection disables it.

**Q: TCP guarantees ordered delivery — why do you need reassembly?**
A: TCP is a byte stream, not a message stream. It preserves byte order but not message boundaries, so a 32-byte send can arrive split or coalesced. I accumulate into a per-client buffer and only parse when a full fixed-size frame is present.

**Q: What does `recv` returning 0 mean, versus −1?**
A: 0 means the peer closed cleanly — a FIN. −1 with `EAGAIN` means no data right now on a non-blocking socket, which is normal. −1 with anything else is a real error. Conflating 0 with `EAGAIN` either spins forever on a dead socket or drops live ones.

**Q: Why `SO_REUSEADDR`?**
A: So the server can rebind immediately after restart instead of waiting out `TIME_WAIT`, which can hold the port for ~60 seconds.

### Exercises

1. Write a client that sends a 32-byte message in two 16-byte `send` calls with a delay between. Verify the gateway reassembles it.
2. Fix the multi-message-per-read bug from §23.7: consume all complete frames, `memmove` the remainder. Test by sending three messages in one `send`.
3. Remove `TCP_NODELAY` and measure round-trip latency with a simple echo client. Report the difference.
4. Remove the `SIGPIPE` protection, connect a client, kill it, and send. Confirm the server dies. Restore the fix.
5. Write a client that connects and immediately closes. Confirm the gateway reclaims the slot and doesn't leak the fd (`lsof -p <pid>`).

---

## Chapter 24: Event-Driven I/O

### 24.1 The problem

With 64 clients, how does one thread know which have data?

**Thread per connection**: 64 threads, each blocking in `recv`. Simple, but every context switch costs ~1–5 µs, each thread needs 8 MB of stack address space, and the scheduler becomes the bottleneck. This is the architecture that collapses at scale (the "C10K problem").

**Polling every socket**: `recv` on each in turn with `O_NONBLOCK`. Works, but at 64 sockets that's 64 syscalls per loop, most returning `EAGAIN`. Syscalls cost ~100 ns–1 µs each.

**I/O multiplexing**: ask the kernel once — "tell me which of these are ready" — and it returns only those. One syscall per batch.

### 24.2 `select` and `poll`, and why not to use them

`select` (1983): a bitmask of fds, limited to `FD_SETSIZE` (1024). The kernel scans every fd in the set on every call, and the caller rebuilds the set each time. **O(n) per call.**

`poll`: an array of `pollfd` instead of a bitmask — no 1024 limit, but still **O(n)**: the kernel walks the whole array every call, and the whole array crosses the user/kernel boundary every call.

Both re-transmit the entire interest set on every call. That is the fundamental flaw.

### 24.3 `epoll` (Linux)

`epoll` keeps the interest set **in the kernel**. You register once; each call returns only the ready fds. **O(1) registration, O(ready) per wait.**

```cpp
event_fd_ = epoll_create1(0);                     // create the epoll instance

epoll_event ev{};
ev.events  = EPOLLIN;                              // interested in readability
ev.data.fd = listen_fd_;                           // payload returned on events
epoll_ctl(event_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);

epoll_event events[64];
int n = epoll_wait(event_fd_, events, 64, 1);      // 1 ms timeout
for (int i = 0; i < n; ++i) { ... }
```

`epoll_ctl` with `ADD`/`MOD`/`DEL` manages the set. `epoll_wait` blocks until something is ready or the timeout expires, and fills `events` with only the ready ones.

The 1 ms timeout matters: it bounds how long the gateway can sit in `epoll_wait` while the outbound ring has data waiting. With no timeout (−1) the gateway would block indefinitely on an idle inbound path and never send responses.

### 24.4 Level-triggered vs edge-triggered

```cpp
ev.events = EPOLLIN | EPOLLET;      // edge-triggered (used for client sockets)
```

**Level-triggered** (default): report readiness as long as data remains. If you read 10 of 100 available bytes, the next `epoll_wait` reports the fd again. Forgiving.

**Edge-triggered** (`EPOLLET`): report only on the *transition* from not-ready to ready. If you don't drain the socket completely, you get no further notification until *new* data arrives — and the unread bytes sit there forever.

Edge-triggered is faster (fewer wakeups) but demands you **read until `EAGAIN`**. That is exactly what the `for (;;)` loop in `read_client` does:

```cpp
for (;;) {
    ssize_t n = recv(...);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) disconnect(slot);
        return;                      // EAGAIN → socket drained, safe to stop
    }
    ...
}
```

The loop exits only on `EAGAIN` (fully drained), clean close, or error. **Using `EPOLLET` without a drain loop is the classic epoll bug** — connections mysteriously hang under load.

The listening socket is registered level-triggered (no `EPOLLET`), so a backlog of pending connections keeps being reported even though `accept_client` takes only one per event.

### 24.5 `kqueue` (macOS/BSD)

Same concept, different API:

```cpp
event_fd_ = kqueue();

struct kevent ev;
EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
kevent(event_fd_, &ev, 1, nullptr, 0, nullptr);          // register

struct kevent events[64];
struct timespec ts = {0, 1000000};                        // 1 ms
int n = kevent(event_fd_, nullptr, 0, events, 64, &ts);   // wait
for (int i = 0; i < n; ++i) {
    int fd = static_cast<int>(events[i].ident);
    ...
}
```

`EV_SET` is a macro filling a `kevent`. The same `kevent()` call both registers and waits, distinguished by which arrays are non-null. `kqueue` is more general than `epoll` — it also handles timers, signals, file changes, and process events through the same interface.

TradeFeed `#ifdef`s between them. The abstraction is thin enough — about 15 lines each — that a wrapper layer would add more complexity than it removes. That is a deliberate call worth defending: premature abstraction over two well-understood APIs is not worth the indirection.

### 24.6 `io_uring` — the modern answer

Linux 5.1+ offers `io_uring`: two shared ring buffers between userspace and kernel (submission and completion). You write requests into the submission ring and read results from the completion ring — **with no syscall at all** in polled mode.

It handles the actual reads and writes, not just readiness notification, so it eliminates both the `epoll_wait` syscall and the subsequent `recv` syscall. For a gateway doing millions of small I/Os per second, that is a significant win.

TradeFeed uses `epoll` because it's portable, universally understood, and the gateway isn't the measured bottleneck. `io_uring` is the documented upgrade path (Chapter 31) — and naming it is how you show you know where the ceiling is.

### Interview Questions

**Q: Why `epoll` over `select`/`poll`?**
A: `select` and `poll` re-transmit the entire interest set on every call and the kernel scans all of it — O(n). `epoll` keeps the set in the kernel: register once, and each wait returns only the ready fds — O(1) registration, O(ready) per wait.

**Q: What's edge-triggered mode and what's the trap?**
A: `EPOLLET` notifies only on the not-ready→ready transition rather than while data remains. Fewer wakeups, but if you don't drain to `EAGAIN` you'll never be told about the leftover bytes and the connection appears to hang. My `read_client` loops until `EAGAIN` for exactly that reason.

**Q: Why a 1 ms timeout on `epoll_wait`?**
A: The gateway also has to drain the outbound ring. Blocking indefinitely on the inbound path would stall responses whenever no client was sending. The timeout bounds that; a cleaner design would use an eventfd the engine signals, removing the poll entirely.

**Q: What would you use instead today?**
A: `io_uring`. It handles the I/O itself rather than just readiness, so it removes both the wait syscall and the recv syscall. `epoll` was the portable choice here and the gateway isn't the bottleneck, but `io_uring` is where this goes next.

### Exercises

1. Remove `EPOLLET` and confirm it still works. Add it back but remove the drain loop — reproduce the hang under load.
2. Instrument: count `epoll_wait` calls and messages received. Compute messages per wait under light and heavy load.
3. Replace the 1 ms timeout with an `eventfd` the engine writes to when it pushes to the outbound ring. Measure the change in idle CPU.
4. Write a `poll`-based gateway and benchmark against `epoll` at 8, 32, and 64 clients. Plot the curves.
5. (Linux) Port `read_client` to `io_uring` with `liburing`. Measure syscalls with `strace -c`.

---

## Chapter 25: The Wire Protocol

### 25.1 Two message representations

TradeFeed deliberately separates them:

| | Internal (`InboundMessage`) | Wire (`WireMessage`) |
|---|---|---|
| Where | SPSC ring, in-process | TCP, between machines |
| Size | 64 bytes (one cache line) | 32 bytes (packed) |
| Padding | compiler-chosen | none |
| Optimised for | cache-line transfer | byte efficiency and interop |

The gateway translates between them. This costs a few nanoseconds per message and is worth it: the ring format is tuned for the cache, the wire format for the network, and neither compromises for the other.

### 25.2 The internal messages

```cpp
struct alignas(CACHE_LINE) InboundMessage {
    MessageType type;
    Side        side;
    OrderType   order_type;
    uint8_t     pad_;
    ClientId    client_id;
    OrderId     order_id;
    Price       price;
    Quantity    quantity;
    Timestamp   recv_tsc;
};
```

Exactly one cache line, so a ring slot is one line and `item = buffer_[tail & MASK]` is a single line transfer (Chapter 18.4).

`recv_tsc` is stamped by the gateway the moment the message is parsed. The engine stamps its own `rdtsc()` on output, so `engine_tsc − recv_tsc` gives the in-process latency of that order — end-to-end instrumentation built into the message flow rather than bolted on.

```cpp
struct alignas(CACHE_LINE) OutboundMessage {
    MessageType type;
    Side        aggressor_side;
    uint8_t     reject_reason;
    uint8_t     pad_;
    ClientId    client_id;
    OrderId     order_id;
    Price       price;
    Quantity    quantity;
    OrderId     match_number;
    Timestamp   engine_tsc;
};
```

One struct covers accept, reject, cancel, and execution. A `union` or variant would save a few bytes but complicate the ring (which needs a fixed element size anyway) for no gain — the struct already fits in one line.

`match_number` is a monotonic trade sequence number, the analogue of NASDAQ ITCH's match number. It lets both sides of a trade be correlated and gives the audit trail a primary key.

### 25.3 The wire message

```cpp
#pragma pack(push, 1)
struct WireMessage {
    uint8_t  type;        // offset 0
    uint8_t  side;        // offset 1
    uint8_t  order_type;  // offset 2
    uint8_t  pad;         // offset 3
    uint32_t client_id;   // offset 4
    uint64_t order_id;    // offset 8
    uint32_t price;       // offset 16
    uint32_t quantity;    // offset 20
    uint64_t timestamp;   // offset 24
};
static_assert(sizeof(WireMessage) == 32);
#pragma pack(pop)
```

The explicit `pad` field at offset 3 is deliberate: it makes `client_id` land at offset 4 (naturally aligned) and `order_id` at offset 8. So despite `#pragma pack(1)` removing *automatic* padding, the hand-chosen layout keeps every field naturally aligned. **Packed for interoperability, aligned for speed** — you get both.

### 25.4 Why fixed-size framing

Every message is exactly 32 bytes. No length prefix, no delimiter, no type-dependent size.

Consequences:
- **Trivial framing**: accumulate 32 bytes, parse. No length field to validate, no risk of a malicious length driving a huge allocation.
- **No parsing**: `memcpy` and read fields. Compare with JSON (tokenise, allocate strings, convert numbers — microseconds) or Protobuf (varint decode, field tags — hundreds of nanoseconds).
- **Predictable**: every message costs the same. No size-dependent latency variance.

The cost is wasted bytes — a cancel doesn't need `price` or `quantity`. At 32 bytes, who cares.

**This is what real exchange protocols do.** NASDAQ OUCH and ITCH are fixed-size binary. CME uses SBE (Simple Binary Encoding), fixed-size with a schema. FIX is a text protocol and is universally regarded as too slow for the latency-sensitive path — venues offer it for compatibility and a binary protocol for anyone who cares.

### 25.5 Why 32 bytes specifically

- **Half a cache line** — two messages per line, so batch reads are efficient.
- **AVX2 register width** — a message is one `vmovdqu`, and batch processing could go SIMD.
- **Power of two** — `n_messages * 32` is a shift.

### 25.6 Endianness — the honest gap

The wire format uses **host byte order**. Both deployment targets are little-endian (x86-64 and Apple/Linux ARM64), so it works today.

It is not portable. A big-endian peer (SPARC, s390x, some network gear) would read every multi-byte field byte-reversed. The fix is to define the protocol's byte order and convert:

```cpp
wire.price = htonl(out.price);              // host → network (big-endian)
msg.price  = ntohl(wire.price);             // network → host
// or C++23: std::byteswap
```

ITCH specifies big-endian; SBE specifies little-endian. Either is fine as long as it's *specified*. Currently TradeFeed's is merely implied — a real gap, and naming it before an interviewer does is much better than being caught by it.

### 25.7 Validation — the other honest gap

```cpp
msg.type       = static_cast<MessageType>(wire.type);
msg.side       = static_cast<Side>(wire.side);
msg.order_type = static_cast<OrderType>(wire.order_type);
```

`static_cast` on an enum does **no range checking**. A client sending `type = 99` produces a `MessageType` with no named value; `quantity = 0` or a garbage `side` passes straight through.

The engine's `switch` has a `default: break;` so unknown types are ignored, and `add_order` validates the price range — so nothing crashes. But this is the system's **trust boundary**, and the rule from Chapter 11.6 is *validate at the boundary, assert internally*. The boundary validation is thin.

What belongs here:

```cpp
if (wire.type > static_cast<uint8_t>(MessageType::ModifyOrder)) { disconnect(slot); return; }
if (wire.side > 1 || wire.order_type > 1)                       { disconnect(slot); return; }
if (wire.quantity == 0 || wire.quantity > MAX_ORDER_QTY)        { reject(...);      return; }
if (wire.client_id != slot_client_id)                            { disconnect(slot); return; }  // impersonation
```

That last one matters: **the client currently supplies its own `client_id`**, so any client can claim to be any other and cancel their orders. The gateway should assign the ID on accept and overwrite whatever the client sends. That is a genuine security hole, and calling it out is far better than hoping nobody asks.

### Interview Questions

**Q: Why a fixed-size binary protocol rather than JSON or Protobuf?**
A: Fixed size means framing is "accumulate 32 bytes" — no length field to validate or exploit — and parsing is a `memcpy` rather than tokenising or varint decoding. JSON is microseconds per message with allocation; Protobuf is hundreds of nanoseconds. Real venues use fixed binary — OUCH, ITCH, SBE — for exactly this reason.

**Q: Why separate wire and internal message formats?**
A: They're optimised for different things. The internal one is a full cache line so a ring slot transfer is one line. The wire one is packed to 32 bytes for network efficiency and cross-language interop. The translation costs a few nanoseconds and keeps both optimal.

**Q: Is your protocol portable?**
A: Not currently — it uses host byte order, which works because both targets are little-endian, but a big-endian peer would misread every field. A real protocol specifies its byte order; ITCH is big-endian, SBE little-endian. Adding `htonl`/`ntohl` is the fix.

**Q: What's the biggest security issue in your gateway?**
A: The client supplies its own `client_id` and the gateway trusts it, so any client can impersonate another and cancel their orders. The gateway should assign the ID at `accept` and overwrite the field. More broadly, the boundary does almost no validation — enum values are `static_cast` without range checks.

### Exercises

1. Add full boundary validation per §25.7. Write a client that sends malformed messages and confirm each is rejected.
2. Fix the impersonation hole: assign `client_id` on accept, store it in `ClientState`, and overwrite the field on every inbound message.
3. Add `htonl`/`htonll` conversion in both directions. Verify with a test that byte-swaps manually.
4. Extend `WireMessage` to 40 bytes with a `char symbol[8]`. Update the `static_assert` and the layout so every field stays naturally aligned.
5. Measure parse cost: `memcpy` + field reads versus `nlohmann::json` parsing an equivalent object. Report the ratio.

---

## Chapter 26: The Matching Engine

The engine is the thin layer that turns messages into book operations and book results into messages.

### 26.1 The run loop

```cpp
void MatchingEngine::run(std::atomic<bool>& running) {
    InboundMessage msg;
    while (running.load(std::memory_order_relaxed)) {
        if (inbound_.pop(msg))
            process(msg);
    }
}
```

A **busy-spin** loop. When the ring is empty it spins, burning 100% of a core.

That is deliberate. The alternative — condition variable, `futex`, or `sleep` — costs 1–5 µs to wake a sleeping thread. At that point the wakeup latency dominates the 50 ns of actual work. A dedicated core spinning is how every low-latency system does it, and it is why the thread gets pinned (Chapter 27).

The `running` flag is `relaxed` — it's checked once per iteration and a few microseconds of delay noticing shutdown is irrelevant. No ordering is needed against anything.

**Refinement worth mentioning**: on a hyperthreaded core, insert `_mm_pause()` (x86 `PAUSE`) in the empty-ring path. It hints the CPU that this is a spin-wait, reducing power draw and freeing pipeline resources for the sibling thread.

### 26.2 `process`

```cpp
void MatchingEngine::process(const InboundMessage& msg) {
    Timestamp tsc = rdtsc();

    switch (msg.type) {
    case MessageType::NewOrder: {
        OrderId id = next_id_++;
        book_.clear_fills();
        if (book_.add_order(id, msg.client_id, msg.side, msg.order_type,
                            msg.price, msg.quantity, msg.recv_tsc)) {
            emit_accept(msg.client_id, id, tsc);
            emit_fills(tsc);
        } else {
            emit_reject(msg.client_id, id, 1, tsc);
        }
        break;
    }
    case MessageType::CancelOrder:
        if (book_.cancel_order(msg.order_id)) emit_cancel(msg.client_id, msg.order_id, tsc);
        else                                  emit_reject(msg.client_id, msg.order_id, 2, tsc);
        break;

    case MessageType::ModifyOrder:
        book_.clear_fills();
        if (book_.modify_order(msg.order_id, msg.price, msg.quantity, msg.recv_tsc)) {
            emit_accept(msg.client_id, msg.order_id, tsc);
            emit_fills(tsc);
        } else {
            emit_reject(msg.client_id, msg.order_id, 3, tsc);
        }
        break;

    default:
        break;
    }
}
```

**`OrderId id = next_id_++;`** — the engine, not the client, assigns IDs. Sequential and monotonic, which is what makes the direct-address lookup table work (Chapter 14.3). It also means IDs are globally unique and ordered by arrival — a free audit trail.

**`book_.clear_fills()` before**, `emit_fills()` after — the book accumulates fills into a member vector during matching, and the engine drains it. This avoids a callback or a return-by-value vector; the vector is `reserve(64)`'d once and reused forever, so no allocation on the hot path.

**`default: break;`** — unknown message types are silently ignored. Robust against garbage, but it should really be a reject or disconnect (Chapter 25.7).

The `switch` on a dense enum compiles to a jump table — one indirect jump, no vtable (Chapter 9.6).

### 26.3 Emitting

```cpp
void MatchingEngine::emit_fills(Timestamp tsc) {
    for (const auto& f : book_.fills()) {
        OutboundMessage m{};
        m.type         = MessageType::OrderExecuted;
        m.price        = f.price;
        m.quantity     = f.quantity;
        m.match_number = match_number_++;
        m.engine_tsc   = tsc;

        m.order_id  = f.bid_id;
        m.client_id = f.bid_client;
        outbound_.push(m);

        m.order_id  = f.ask_id;
        m.client_id = f.ask_client;
        outbound_.push(m);
    }
}
```

**Two messages per fill** — both sides of a trade must be notified. They share a `match_number`, so a client can correlate their fill with the trade print, and a regulator can reconstruct both sides from the tape.

The struct is reused between the two pushes, mutating only the two differing fields. `push` copies by value into the ring, so mutation after the first push is safe.

`OutboundMessage m{}` — the `{}` value-initialises, zeroing every field. Without it, `reject_reason` and `aggressor_side` would carry garbage from the stack. Cheap insurance: 64 bytes of zeroing that the compiler usually turns into two AVX stores.

**Gap**: `outbound_.push` return values are ignored. A full outbound ring silently drops fill notifications — a client would never learn their order executed, which is far worse than dropping an inbound order. Same fix as Chapter 18.8, and arguably more urgent.

### 26.4 Reject reasons

```cpp
emit_reject(msg.client_id, id, 1, tsc);   // 1 = pool exhausted or bad price
emit_reject(msg.client_id, msg.order_id, 2, tsc);   // 2 = cancel: order not found
emit_reject(msg.client_id, msg.order_id, 3, tsc);   // 3 = modify: order not found
```

Bare integers. They should be an `enum class RejectReason : uint8_t { PoolExhausted, InvalidPrice, UnknownOrder, ... }` — self-documenting, and the compiler catches a typo. This is exactly the `enum class` argument from Chapter 2.6, and the code currently doesn't take its own advice. A small, honest cleanup to name.

### Interview Questions

**Q: Why does the engine busy-spin instead of blocking?**
A: Waking a blocked thread costs 1–5 µs, which dwarfs the ~50 ns of work per message. A dedicated pinned core spinning is standard for low-latency systems. I'd add `_mm_pause()` in the empty path to be friendlier to a hyperthread sibling.

**Q: Why does the engine assign order IDs rather than the client?**
A: Sequential monotonic IDs are what make the direct-address lookup table work — no hashing needed. They're also globally unique and ordered by arrival, which gives a free audit trail. Client-supplied IDs would need a hash map and a uniqueness check.

**Q: What's the worst bug in this file?**
A: `outbound_.push` return values are ignored. If the outbound ring fills, execution reports are silently dropped and a client never learns their order filled. That's worse than dropping an inbound order — the client's view of their position diverges from the exchange's.

### Exercises

1. Replace the integer reject reasons with an `enum class`. Update all call sites.
2. Handle `push` failure in `emit_fills`: spin until it succeeds, and count the spins. Run the mixed workload and report the maximum.
3. Add `_mm_pause()` to the empty-ring path. Measure CPU power draw or sibling-thread throughput, if you have the tooling.
4. Add latency instrumentation: histogram `engine_tsc − recv_tsc` across a benchmark run and report percentiles.
5. Make `default:` disconnect the client instead of ignoring the message. What has to change in the engine→gateway interface to make that possible?

---

## Chapter 27: Threads, Cores, and Scheduling

### 27.1 The model

```
┌──────────────────────────────────────────────────────────────┐
│  main thread (any core)                                      │
│    prints status every 5 s, waits for SIGINT                 │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────┐        ┌──────────────────────────┐
│  Gateway (core 2)        │        │  Engine (core 1)         │
│  ─────────────────       │        │  ────────────────        │
│  epoll_wait / kevent     │        │  spin on inbound_.pop()  │
│  recv → parse            │  ────▶ │  process(msg)            │
│  inbound_.push()         │ inbound│    → OrderBook           │
│                          │  ring  │  outbound_.push()        │
│  outbound_.pop()         │ ◀──────│                          │
│  → send()                │outbound│                          │
└──────────────────────────┘  ring  └──────────────────────────┘
```

Two worker threads, two rings, strict ownership. The engine owns the book and pool; the gateway owns the sockets. Nothing is shared except the rings (Chapter 16.4).

### 27.2 Starting the threads

```cpp
std::thread engine_thread([&] {
#ifdef __linux__
    pin_thread(1);
#endif
    engine.run(g_running);
});
```

The lambda captures by reference (`[&]`) — safe because `main` joins both threads before returning (Chapter 4.7). Pinning happens *inside* the thread because affinity applies to the calling thread.

### 27.3 Core pinning

```cpp
static void pin_thread(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    ...
}
```

`cpu_set_t` is a bitmask of allowed cores. `CPU_ZERO` clears it, `CPU_SET` allows one core, `pthread_setaffinity_np` applies it. (`_np` = "non-portable" — it's a GNU extension, hence the `#ifdef __linux__`.)

**Why it matters.** Without pinning, the scheduler may migrate a thread between cores. The new core's L1 and L2 are cold, so the entire working set — the hot price levels, the pool's free-list tail, the ring indices — must be re-fetched. That is thousands of cache misses: **5–50 µs of degraded performance** after every migration.

Migrations happen for reasons you don't control: another process wakes, an interrupt is delivered, the scheduler rebalances. Pinning removes the variable entirely. The engine's ~5 KB working set (Chapter 6.5) stays resident in one core's L1 permanently.

**Choosing cores.** Core 0 typically handles interrupts and kernel work — avoid it. On a hyperthreaded CPU, cores 0–9 and 10–19 may be siblings sharing L1/L2 on the i9-10900K; pinning two busy threads to siblings means they fight for the same cache. The robust approach is to check the topology:

```bash
lscpu -e           # shows CPU, CORE, SOCKET, L1/L2/L3 mapping
```

and pin to distinct *physical* cores.

**Isolating cores.** The complete solution removes cores from the scheduler entirely:

```bash
# kernel cmdline
isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2
```

- `isolcpus` — the scheduler won't place any other task there.
- `nohz_full` — no periodic timer tick on those cores (the tick is a ~1 µs interruption, up to 1000×/second).
- `rcu_nocbs` — RCU callbacks are offloaded elsewhere.

With this, the pinned thread runs genuinely uninterrupted. This is standard practice on production trading hosts and is the right answer to "how would you reduce tail latency further?"

### 27.4 `SCHED_FIFO`

```cpp
sched_param sp{};
sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
sched_setscheduler(0, SCHED_FIFO, &sp);
```

Linux offers several scheduling policies:

| Policy | Behaviour |
|--------|-----------|
| `SCHED_OTHER` | default CFS — fair time-slicing among all tasks |
| `SCHED_FIFO` | real-time; runs until it blocks or yields; only preempted by higher RT priority |
| `SCHED_RR` | like FIFO but round-robins between equal-priority RT tasks |

`SCHED_FIFO` at max priority means the engine is never preempted by a normal process. A background `cron` job cannot context-switch it mid-match.

**The danger**: a `SCHED_FIFO` thread that never blocks will monopolise its core completely. If it's pinned to the same core as something essential, you can lock up the machine. Linux's RT throttling (`/proc/sys/kernel/sched_rt_runtime_us`, default 950000 of 1000000 µs) reserves 5% for non-RT tasks as a safety valve — which itself introduces a periodic 50 ms stall, so production systems often disable it *and* isolate cores properly.

Requires `CAP_SYS_NICE` or root. Without it `sched_setscheduler` fails, the return value is ignored, and the thread runs at normal priority — degraded but functional. That graceful degradation is intentional; the program must not require root to run.

### 27.5 The macOS situation

macOS has no `pthread_setaffinity_np`. There is `thread_policy_set` with `THREAD_AFFINITY_POLICY`, but it's only a hint and Apple Silicon ignores it — the OS decides P-core versus E-core placement itself.

So on macOS the threads are unpinned. This is a development platform; the benchmarks there measure relative improvements, and absolute latency numbers come from the Linux target. It also explains the cross-core TSC skew artefact in Chapter 10.5 — unpinned threads on Apple Silicon can migrate between P and E cores mid-measurement.

### 27.6 Shutdown

```cpp
static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }
std::signal(SIGINT,  signal_handler);
std::signal(SIGTERM, signal_handler);
```

Ctrl-C sets the flag; both loops observe it within one iteration and return; `main` joins them and prints final statistics.

**Signal-handler safety** is a real constraint: a handler may only call async-signal-safe functions. `printf`, `malloc`, and most of the standard library are *not* safe — they can deadlock if the signal arrives while the main program holds an internal lock. Storing to an `std::atomic<bool>` is safe, which is precisely why the handler does nothing else. (`std::atomic<bool>` is guaranteed lock-free on every real platform; `std::sig_atomic_t` is the strictly-standard alternative.)

### Interview Questions

**Q: Why pin threads to cores?**
A: To keep the working set in one core's L1/L2. A migration leaves the thread on a cold core and costs thousands of cache misses — 5–50 µs of degradation — and migrations happen for reasons outside your control. Pinning removes the variable. The engine's working set is ~5 KB, so once resident it stays resident.

**Q: What's `SCHED_FIFO` and what's the risk?**
A: A real-time policy where the thread runs until it blocks or yields, never preempted by normal tasks — it removes scheduler jitter from the hot path. The risk is monopolising a core and locking the machine. Linux throttles RT tasks to 95% by default as a safety valve, though that throttle itself causes a periodic stall, so production setups disable it and isolate cores instead.

**Q: How would you cut tail latency further at the OS level?**
A: `isolcpus` to remove the cores from the scheduler, `nohz_full` to stop the timer tick, `rcu_nocbs` to offload RCU callbacks, the performance frequency governor, and huge pages for the book and pool. Beyond that, IRQ affinity so NIC interrupts land on a different core than the engine.

**Q: Why does the signal handler only set a flag?**
A: Only async-signal-safe functions may be called from a handler. `printf` and `malloc` aren't — if the signal arrives while the program holds an allocator lock, calling them deadlocks. An atomic store is safe, so the handler sets a flag and the loops notice it.

### Exercises

1. Run `bench_orderbook` with and without `taskset -c 3`. Compare p99.9.
2. Write a program that pins to a core, records `rdtsc` deltas in a tight loop, and histograms them. Identify the timer tick. Re-run with `nohz_full` and compare.
3. Check your CPU topology with `lscpu -e` and identify the hyperthread sibling pairs. Pin two busy threads to siblings, then to distinct physical cores. Measure both.
4. Add `SCHED_FIFO` and run without root. Confirm it degrades gracefully. Then run with `sudo` and compare tail latency.
5. Add `_mm_pause()` to the engine's spin loop and measure the effect on a sibling hyperthread's throughput.

---

## Chapter 28: Building and Benchmarking

### 28.1 The CMake file

```cmake
cmake_minimum_required(VERSION 3.20)
project(TradeFeed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

`CMAKE_EXPORT_COMPILE_COMMANDS` writes `compile_commands.json`, which `clangd` uses for IDE completion and diagnostics. Always on.

```cmake
add_executable(tradefeed
    src/main.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(tradefeed PRIVATE src)
target_compile_options(tradefeed PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
target_link_libraries(tradefeed PRIVATE pthread)
```

Three translation units (Chapter 1.7); everything else is header-only.

### 28.2 The flags that matter

**`-O3`** — full optimisation: aggressive inlining, loop unrolling, vectorisation. The difference from `-O0` on this codebase is roughly 10×, because the whole design assumes small functions get inlined. Benchmarking a debug build is meaningless.

**`-march=native`** — emit instructions for *this exact CPU*, enabling AVX2/AVX-512, BMI2, `LZCNT`, and letting the scheduler tune for the specific microarchitecture. Typically another 10–30% on numeric code.

The catch: the binary may crash with `SIGILL` on an older CPU. For distribution you'd use `-march=x86-64-v3` (a portable baseline covering AVX2) or runtime dispatch. For a system built for one known machine, `native` is right.

**`-Wall -Wextra -Wpedantic`** — the warning set. `-Wall` is misleadingly named (it is not all warnings); `-Wextra` adds the rest of the useful ones; `-Wpedantic` flags non-standard extensions. The codebase compiles clean under all three, which is the baseline expectation.

**Does `-O3` auto-vectorise?** Yes, for simple countable loops over contiguous data. But it cannot vectorise pointer chasing — the intrusive list walk in `drain_level` is inherently serial, since you cannot know the next address until you've loaded the current node. Auto-vectorisation helps the benchmark's `std::sort` and the analytics loops, not the matching hot path. Knowing *where* it does and doesn't apply is the interview-grade answer.

### 28.3 The debug target

```cmake
target_compile_options(tradefeed_debug PRIVATE
    -O1 -g3 -fno-omit-frame-pointer -march=native
    -fsanitize=address,undefined)
target_link_options(tradefeed_debug PRIVATE -fsanitize=address,undefined)
```

**AddressSanitizer** instruments every memory access to catch use-after-free, buffer overflow, leaks, and stack overflow. ~2× slower, ~3× more memory. It is what diagnosed the 56 MB stack overflow in Chapter 4.1 — an instant, exact answer where a bare segfault gave nothing.

**UndefinedBehaviorSanitizer** catches signed overflow, misaligned access, invalid casts, out-of-range enum values.

`-O1` because sanitizers need *some* optimisation to be usable but full `-O3` makes stack traces unreadable. `-fno-omit-frame-pointer` keeps the frame pointer so stack traces are accurate.

**ThreadSanitizer** (`-fsanitize=thread`) detects data races and is the right tool for validating the ring buffer. It cannot be combined with ASan — it needs its own target.

### 28.4 The benchmark suite

Five order-book benchmarks and four ring benchmarks, each isolating one thing:

| Benchmark | What it isolates |
|-----------|------------------|
| `bench_add_order` | pure insertion — orders constructed *not* to cross |
| `bench_cancel_order` | pure cancellation — pre-populated book, random cancel order |
| `bench_matching` | pure matching — every order crosses the spread |
| `bench_mixed_workload` | realistic 70/20/10 add/cancel/match mix |
| `bench_engine_throughput` | full message path including ring and emit |
| ring: single-thread | push/pop round-trip with no contention |
| ring: burst | fill and drain in bulk |
| ring: cross-core latency | producer→consumer wakeup time |
| ring: cross-core throughput | sustained rate between cores |

**Isolation is the point.** `bench_add_order` deliberately places bids below and asks above so nothing matches — otherwise you'd be measuring insertion *and* matching and the number would mean nothing. `bench_matching` does the opposite. Mixing them without intent is the most common benchmarking mistake.

The shuffle in `bench_cancel_order`:

```cpp
std::iota(ids.begin(), ids.end(), 1);
std::shuffle(ids.begin(), ids.end(), rng);
```

Cancelling in insertion order would always hit the head of each list and always be cache-warm. Random order is what real flow looks like.

Seeded RNGs (`std::mt19937 rng(42)`) make every run reproducible — essential for comparing before and after a change.

### 28.5 Reading the output

```
  add_order   n=500000   p50=0ns  p90=42ns  p99=83ns  p99.9=625ns  min=0ns  max=12751ns
```

- **p50 = 0ns** — faster than the ARM timer's 41.67 ns resolution (Chapter 10.2). On x86 you'd see a real figure around 20–40 ns.
- **p99 = 83ns** — two timer ticks. Occasional cache misses.
- **p99.9 = 625ns** — the tail. Most likely a `scan_best_bid` walking several levels, or a `fills_` vector growth.
- **max = 12751ns** — almost certainly the OS descheduling the thread. On an isolated, pinned core this largely disappears.

```
  Processed 200000 orders in 2963957 ns
  Throughput: 67477353 orders/sec
```

67M orders/sec, ~15 ns per order. For scale, NASDAQ peaks around 1–5M messages/sec **across all symbols**. A single-symbol engine at 67M/sec has enormous headroom — the honest conclusion is that the engine is not the bottleneck; the network is.

### 28.6 What the benchmarks don't measure

Being straight about this is more impressive than the numbers:

- **No network.** Everything is in-process. Real end-to-end latency is dominated by NIC, kernel, and TCP — microseconds, not nanoseconds.
- **Single-threaded.** `bench_engine_throughput` pre-fills the ring and drains it on one thread, so there's no producer/consumer contention.
- **Synthetic flow.** Uniform random prices in a tight band. Real flow is bursty, clustered at round numbers, and has fat-tailed sizes.
- **Warm cache, no other load.** A production box runs monitoring, logging, and other processes.
- **macOS timer resolution.** ~41.67 ns granularity destroys p50 fidelity. The x86 numbers will be far more informative.

**What would make them honest**: a load-generating client over loopback and then over a real NIC, measuring true round-trip; replaying a real ITCH session for realistic flow; and running on the isolated, pinned Linux box.

### Interview Questions

**Q: Why `-march=native` and what's the downside?**
A: It enables the full instruction set of the build machine — AVX2, BMI2, `LZCNT` — and tunes scheduling for the microarchitecture, typically 10–30% on numeric code. The downside is the binary may `SIGILL` on an older CPU, so for distribution you'd target a baseline like `x86-64-v3` or do runtime dispatch.

**Q: Does `-O3` vectorise your matching loop?**
A: No. Auto-vectorisation needs countable loops over contiguous data. Walking an intrusive linked list is inherently serial — you can't compute the next address until the current node loads. It does vectorise the sort and analytics loops in the benchmarks. That's why I optimise the matching path for cache behaviour rather than SIMD.

**Q: Your benchmarks show 67M orders/sec. What are they not telling me?**
A: They're in-process with no network, single-threaded with no ring contention, driven by synthetic uniform flow on a warm cache with nothing else running, and measured on a platform with 41 ns timer granularity. Real end-to-end latency is dominated by NIC and kernel — microseconds. The engine number tells me the engine isn't the bottleneck; that's all it tells me.

**Q: How did you find the stack overflow?**
A: A bare segfault, exit 139, with no output. Rebuilt with AddressSanitizer and it named the function and diagnosed `stack-overflow` immediately. The cause was `std::array` storing elements inline, making `OrderBook` 56 MB on an 8 MB stack. Fixed by heap-allocating through `unique_ptr<T[]>`.

### Exercises

1. Build at `-O0`, `-O1`, `-O2`, `-O3` and plot `bench_orderbook` results. Explain the jump between `-O0` and `-O1`.
2. Remove `-march=native`. Measure. Then check which ISA extensions it was enabling: `gcc -march=native -Q --help=target | grep enabled`.
3. Build `bench_ring` with ThreadSanitizer and run the cross-core test. Report whether the ring is clean.
4. Add a warmup phase to each benchmark and measure how much the first-iteration outliers shrink.
5. Write a load-generating client that connects over TCP and measures true round-trip latency. Compare to the in-process numbers.
6. Run the full suite on the Linux target. Produce a comparison table against the macOS numbers and explain every difference.

---

# PART VII — MASTERY

---

## Chapter 29: One Message, End to End

Everything in this book, applied to a single order. Follow it with the source open.

### The scenario

Book state:

```
ASKS:  $150.02 → [ B(100) ]
BIDS:  $150.00 → [ A(300) ]
best_bid_ = 15000,  best_ask_ = 15002
```

A client sends: **buy 150 shares, limit $150.02**.

### Step 1 — Client sends 32 bytes

```cpp
WireMessage w{};
w.type = 1;  w.side = 0;  w.order_type = 0;
w.client_id = 7;  w.price = 15002;  w.quantity = 150;
send(fd, &w, 32, 0);
```

*Chapter 25: fixed-size packed frame, host byte order.*

### Step 2 — Kernel delivers; epoll reports readiness

NIC → driver → kernel TCP stack → socket receive buffer. The kernel marks the fd readable; `epoll_wait` returns it.

```cpp
int n = epoll_wait(event_fd_, events, 64, 1);
read_client(events[i].data.fd);
```

*Chapter 24: O(ready) notification, edge-triggered.*

### Step 3 — Gateway reads and reassembles

```cpp
ssize_t n = recv(fd, c.read_buf + c.read_pos, WIRE_MSG_SIZE - c.read_pos, 0);
c.read_pos += n;                    // 32
if (c.read_pos >= WIRE_MSG_SIZE) {  // complete frame
    WireMessage wire;
    std::memcpy(&wire, c.read_buf, WIRE_MSG_SIZE);
    c.read_pos = 0;
```

*Chapter 23.7: TCP is a byte stream; accumulate until a full frame. Chapter 3.7: `memcpy` into an aligned local.*

### Step 4 — Translate to the internal format

```cpp
InboundMessage msg{};
msg.type       = static_cast<MessageType>(wire.type);   // NewOrder
msg.side       = static_cast<Side>(wire.side);          // Buy
msg.order_type = static_cast<OrderType>(wire.order_type); // Limit
msg.client_id  = wire.client_id;                        // 7
msg.price      = wire.price;                            // 15002
msg.quantity   = wire.quantity;                         // 150
msg.recv_tsc   = rdtsc();                               // ← latency clock starts
```

*Chapter 25.1: wire→internal. Chapter 10.2: `rdtsc` timestamp.*

### Step 5 — Push to the inbound ring

```cpp
inbound_.push(msg);
```

Inside `push`:
```cpp
const size_t head = head_.val.load(relaxed);       // own index, no ordering needed
if (head - cached_tail_ >= Capacity) { ... }       // cached bound — no cross-core read
buffer_[head & MASK] = item;                       // one cache-line store
head_.val.store(head + 1, release);                // publish: data visible before index
```

*Chapter 18.3. Chapter 17.3: release ordering. Chapter 8.5: cached index.*

**The gateway's work is done. It returns to `epoll_wait`.**

### Step 6 — Engine pops

```cpp
while (running.load(relaxed)) {
    if (inbound_.pop(msg))
        process(msg);
}
```

Inside `pop`:
```cpp
const size_t tail = tail_.val.load(relaxed);
if (cached_head_ <= tail) { cached_head_ = head_.val.load(acquire); ... }  // acquire pairs with release
item = buffer_[tail & MASK];                       // guaranteed to see the write
tail_.val.store(tail + 1, release);                // publish: slot free
```

*Chapter 18.4. The acquire/release pair is what makes this correct.*

### Step 7 — Engine dispatches

```cpp
Timestamp tsc = rdtsc();
switch (msg.type) {
case MessageType::NewOrder: {
    OrderId id = next_id_++;        // say, 4001
    book_.clear_fills();
```

*Chapter 26.2: jump table, not a vtable. Engine-assigned sequential ID.*

### Step 8 — `add_order` validates and allocates

```cpp
if (pool_.available() == 0) return false;                       // OK
if (type == Limit && (price < MIN_PRICE || price > MAX_PRICE))  // 15002 valid
    return false;

Order* o = pool_.allocate();     // pop from free-list stack — LIFO, cache-warm
o->id = 4001;  o->client_id = 7;  o->price = 15002;
o->quantity = 150;  o->filled_qty = 0;
o->side = Buy;  o->type = Limit;  o->timestamp = msg.recv_tsc;
o->prev = o->next = nullptr;

reg(o);      // order_table_[4001 & 0xFFFFF] = o
```

*Chapter 11.4: ~2 ns pool allocation. Chapter 14.3: direct-address registration.*

### Step 9 — Match before resting

```cpp
match_limit(o);
```

```cpp
while (!o->is_filled() && best_ask_ <= o->price && best_ask_ <= MAX_PRICE) {
    // 15002 <= 15002 ✓ — the order crosses
    drain_level(asks_[idx(15002)], o, Side::Buy);
```

Inside `drain_level`:
```cpp
Order* resting = level.front();                    // B — oldest at this price
Quantity fill_q = std::min(150, 100) = 100;        // CMOV, branchless

o->fill(100);         // aggressor: 100/150 filled
resting->fill(100);   // B: 100/100 — complete
level.adjust_qty(-100);

fills_.push_back({4001, B.id, 7, B.client, 15002, 100});   // maker's price
++match_count_;

if (resting->is_filled()) {       // B is done
    level.remove(resting);        // O(1) intrusive unlink
    unreg(resting);               // clear lookup slot
    pool_.deallocate(resting);    // return to free list
}
// level now empty → exit inner loop
```

Back in `match_limit`:
```cpp
    if (asks_[idx(15002)].empty()) scan_best_ask();   // → best_ask_ = 1000001 (no asks)
}
// loop re-tests: best_ask_ (1000001) <= MAX_PRICE is false → exit
```

*Chapter 21.3–21.4. Chapter 20.5: fill at the maker's price. Chapter 13.7: sentinel ends the loop with no null check.*

### Step 10 — Rest the remainder

```cpp
if (!o->is_filled() && type == Limit) {     // 50 shares remain
    bids_[idx(15002)].push(o);              // append at tail — time priority
    if (15002 > best_bid_) best_bid_ = 15002;   // BBO improves: 15000 → 15002
}
```

*Chapter 12.4: O(1) tail append. Chapter 13.7: O(1) BBO update on insert.*

New book state:

```
ASKS:  (empty)
BIDS:  $150.02 → [ o(150, 100 filled → 50 remaining) ]
       $150.00 → [ A(300) ]
best_bid_ = 15002,  best_ask_ = 1000001
```

### Step 11 — Emit

```cpp
emit_accept(7, 4001, tsc);       // OrderAccepted → outbound ring
emit_fills(tsc);                 // one Fill → TWO messages
```

```cpp
m.match_number = match_number_++;
m.order_id = 4001;   m.client_id = 7;          outbound_.push(m);   // buyer
m.order_id = B.id;   m.client_id = B.client;   outbound_.push(m);   // seller
```

*Chapter 26.3: both sides of every trade are notified, sharing a match number.*

### Step 12 — Gateway drains and sends

```cpp
while (outbound_.pop(out)) {
    WireMessage wire{};
    wire.type = static_cast<uint8_t>(out.type);
    wire.order_id = out.order_id;
    wire.price = out.price;
    wire.quantity = out.quantity;
    wire.timestamp = out.engine_tsc;

    send(clients_[i].fd, &wire, 32, flags);   // TCP_NODELAY → on the wire immediately
}
```

*Chapter 23.4: no Nagle delay.*

### The scorecard

| Step | Technique | Chapter | Cost |
|------|-----------|---------|------|
| 5 | Lock-free ring push | 18 | ~2.5 ns |
| 6 | Lock-free ring pop | 18 | ~2.3 ns |
| 7 | Jump-table dispatch | 9.6, 26.2 | ~1 ns |
| 8 | Pool allocation | 11 | ~2 ns |
| 8 | Direct-address registration | 14 | ~1 ns |
| 9 | Price-indexed level lookup | 13 | ~1 ns (L1) |
| 9 | Intrusive `front()` | 12 | ~0 ns (same line) |
| 9 | Branchless `min` | 9.4 | ~1 ns |
| 9 | Intrusive unlink | 12.5 | ~2 ns |
| 10 | Intrusive tail append | 12.4 | ~2 ns |
| 10 | O(1) BBO update | 13.7 | ~1 ns |

**Total in-process: roughly 15–20 ns.** Which matches the measured 15 ns/order engine throughput.

Every one of those numbers is small because of a specific decision made in Parts II–IV. That is the story to tell.

---

## Chapter 30: The Interview

### 30.1 The 60-second pitch

> TradeFeed is a limit order book and matching engine in C++20, built to NASDAQ-style price-time priority. The architecture is two pinned threads — a gateway doing epoll and a single-threaded matching engine — connected by lock-free SPSC ring buffers. The engine is single-threaded deliberately: matching must be deterministic for replay and regulatory reconstruction, so real venues shard by symbol rather than parallelising a book.
>
> The performance work is all about eliminating memory stalls. Orders come from a pre-allocated pool, so there's no `malloc` on the hot path. Price levels are a direct-indexed array rather than a tree, so a lookup is one array access instead of twenty pointer-chasing cache misses. Each level is an intrusive doubly-linked list, so there's no per-node allocation and cancels are O(1). Order lookup is a direct-address table keyed on `id & mask`, because I assign the IDs sequentially and don't need hashing. The `Order` struct is exactly one cache line with the match-critical fields in the first sixteen bytes.
>
> It benchmarks at 67 million orders per second in-process, with sub-100-nanosecond p99 on book operations and a 2.5-nanosecond ring push. The honest caveat is those are in-process numbers — real end-to-end latency is dominated by the NIC and kernel, and the next thing I'd build is an AF_XDP data path to attack that.

### 30.2 Questions on architecture

**Q: Why is the matching engine single-threaded? Isn't that leaving performance on the table?**
Determinism. If matching a single book were parallel, execution order would depend on thread scheduling, so the same input sequence could produce different trades. That breaks regulatory reconstruction, replay-based recovery, and regression testing — all non-negotiable for an exchange. Real venues shard by symbol, one book per thread, and never parallelise within a book. Empirically it's also not the constraint: 67M orders/sec is 15–60× NASDAQ's all-symbol peak.

**Q: Walk me through your threading model.**
Two worker threads with strict data ownership. The gateway owns the sockets and client state; the engine owns the book, the pool, and every `Order`. Nothing is written by both. They communicate through two SPSC rings — inbound and outbound — where each thread writes only its own index. That's why I need no locks: I structured ownership so there's almost nothing to synchronise, then made that small remainder lock-free.

**Q: How would you scale to a thousand symbols?**
Shard by symbol. Each engine thread owns a disjoint set of symbols with its own book, pool, and ring pair. The gateway routes by symbol hash. Books never interact, so there's no cross-thread synchronisation and each shard keeps its determinism. That's exactly how production venues do it, and it scales linearly with cores.

**Q: What's your biggest architectural regret?**
The gateway broadcasts every outbound message to every client rather than routing by `client_id`. It's O(clients) per message and it leaks other participants' fills. The fix is a `client_id → fd` map plus per-client outbound buffering. It's first on the list.

### 30.3 Questions on data structures

**Q: Why not `std::map` for price levels?**
It's a red-black tree — O(log n) with roughly twenty dependent cache misses per lookup, plus a node allocation and rotations on insert. Pointer chasing is the worst possible access pattern because the misses serialise. Prices are dense bounded integers, so I index an array directly: one subtract, one shift-add, one load. It costs 48 MB of DRAM, and what matters for speed is the *cached* working set, which is the hundred or so levels near the spread — about 2.4 KB, permanently in L1.

**Q: That's a lot of memory for a mostly-empty array.**
0.07% of the machine's RAM. And untouched DRAM doesn't compete for cache — only the pages I actually read get cached. I'd reconsider if the price range were unbounded, like FX at eight decimals, where I'd use a direct-indexed window around the current price with a hash fallback for far strikes.

**Q: Why not `unordered_map` for order lookup?**
Chained hashing: a bucket array load plus a chain node load, so two-plus dependent misses, and a heap allocation per insert. But the real point is that I assign the order IDs myself, sequentially — they're already dense and uniform, so hashing them is pure waste. A direct-address table indexed `id & mask` is one AND and one load. I store the ID in the `Order` and verify it on lookup so a wrap-around collision becomes a clean rejection rather than cancelling the wrong order.

**Q: Why an intrusive list rather than `std::list`?**
`std::list` allocates a node per insert and adds an indirection, so traversal costs two cache misses per element. Intrusive links live inside the `Order`, which already exists in the pool — zero allocation, one miss, and O(1) removal given just the pointer. Doubly-linked specifically so cancels are O(1); with only `next` I'd need an O(n) walk to find the predecessor, and cancels are twenty percent of flow.

**Q: What's the complexity of finding the best bid?**
O(1) on insert. On depletion it's a linear scan from the old best — typically one to five ticks in a liquid book, but unbounded in a thin one, and that shows up in my cancel p99.9. The fix is a hierarchical bitset: one bit per level in three tiers, three `__builtin_clzll` calls, genuinely O(1), about 2 MB. That's my highest-value remaining optimisation.

### 30.4 Questions on concurrency

**Q: Explain why your ring buffer is correct.**
Each index has exactly one writer, so no compare-exchange is needed — a plain load and store suffice. The producer writes the slot then release-stores `head`; the consumer acquire-loads `head` then reads the slot. That pair guarantees the data write is visible before the index advance. Indices are monotonic unsigned counters, so `head − tail` is the element count even across wrap, and there's no full/empty ambiguity. Only the indexing wraps, via a mask.

**Q: What breaks if you use `relaxed` instead of `release` on the store?**
On x86, probably nothing visible — the hardware memory model already forbids that store-store reorder, so the annotation only constrains the compiler. On ARM64 it breaks: the index advance can become visible before the data write, and the consumer reads an unwritten slot. That's the class of bug that passes every test on your laptop and corrupts in production.

**Q: Why cache the counterpart index?**
Padding stops write-write contention on the indices, but the producer still *reads* the consumer's `tail`, which pulls that line into shared state and forces an invalidation on the consumer's next write. The line ping-pongs. Caching a stale copy fixes it: the cached tail is a lower bound on the true tail, so if it says there's room there genuinely is room — stale data can only make me conservative, never wrong. In steady state the producer never touches a consumer-owned line, which is what gets push to 2.5 ns.

**Q: What is false sharing?**
Two cores writing different variables that happen to share a cache line. There's no logical sharing, but coherence works at line granularity, so every write invalidates the other core's copy and the line bounces at 40–70 ns per round trip. I've measured 24× on a two-counter microbenchmark. Fix is `alignas(64)` to give each variable its own line — which is why `head_`, `tail_`, and both cached indices are separately aligned.

### 30.5 Questions on the domain

**Q: A buy limit at $150.10 hits a resting ask at $150.02. What price does it trade at?**
$150.02 — the maker's price. The resting order's price was public and committed first; the taker's willingness to pay more is private information and doesn't set the price. The taker gets price improvement. If it worked the other way, posting a limit order would expose you to being filled at arbitrary prices and nobody would provide liquidity.

**Q: Implement price-time priority. What structures?**
Price priority is the outer loop over an array of levels indexed by price, starting from the cached best and walking outward. Time priority is the inner loop over an intrusive FIFO at each level — push appends at the tail, `front()` returns the oldest. There's no sort and no comparator anywhere; the rule falls out of the structures.

**Q: Why does modifying an order lose queue priority?**
A modification is a new economic commitment, so it shouldn't retain priority earned at a different price. I implement it as cancel-and-replace. Some venues preserve priority for a pure quantity decrease, since you're not asking for more — a refinement I haven't implemented.

**Q: What's VPIN and why would a market maker care?**
It estimates what fraction of order flow is informed, by measuring buy/sell volume imbalance within fixed-*volume* buckets and averaging over a rolling window. Volume-time rather than clock-time because information arrives with volume — a quiet minute and a frantic minute aren't comparable samples, but 10,000 shares always is. A maker cares because informed flow means adverse selection: they're systematically buying just before the price falls. Rising VPIN means widen quotes or withdraw, which is why liquidity evaporates in stress — every maker's toxicity model fires at once.

### 30.6 The hard questions

**Q: What's wrong with this code?**
Five things, in order of severity. One: clients supply their own `client_id` and the gateway trusts it, so anyone can impersonate anyone and cancel their orders. Two: `push` return values are ignored in both directions, so a full ring silently drops messages — for outbound that means a client never learns their order filled. Three: the gateway broadcasts every message to every client instead of routing. Four: the read loop parses one message per readable event and resets the buffer, so two messages arriving in one `recv` loses the second. Five: the wire format is host byte order, which is unspecified and non-portable.

**Q: How would you test this properly?**
Unit tests per structure with invariant checks — a `validate()` on `PriceLevel` that walks the list and confirms `count_` and `total_qty_`, and a crossed-book assertion after every book operation. Property-based testing with random operation sequences, checking conservation invariants: total filled quantity is equal on both sides, and pool allocations minus deallocations equals resting orders. Differential testing against a deliberately naive reference implementation using `std::map` and `std::list` — same input, assert identical fills. ThreadSanitizer on the ring. And replay of a captured ITCH session for realistic flow.

**Q: Your p99.9 is 625 ns but p50 is under 42 ns. Explain the gap.**
Three contributors. The `scan_best_bid`/`scan_best_ask` linear walk when a best level empties — usually one to five ticks, occasionally far more, and that's the dominant one. The `fills_` vector growing past its reserved 64 on a deep sweep, which is one reallocation. And OS scheduling — the max of 12 µs is almost certainly a descheduling event, which is why the target is a pinned, isolated core. The bitset fixes the first, a larger reserve or a fixed-size buffer fixes the second, `isolcpus` and `nohz_full` fix the third.

**Q: If you had one week, what would you do?**
Fix the correctness gaps first — gateway-assigned client IDs, backpressure instead of silent drops, per-client routing, and the multi-message read. Those are bugs, not optimisations. Then the hierarchical bitset, because it removes the only non-O(1) operation in the book and directly attacks the p99.9. Then a proper load-generating client so I'm measuring end-to-end rather than in-process. AF_XDP comes after that, because there's no point shaving kernel microseconds while messages are being silently dropped.

**Q: What did you learn building this?**
That the algorithmic complexity is almost never the interesting part. Every real improvement came from memory behaviour — replacing a tree with an array, eliminating allocation, packing a struct into one cache line, padding two atomics apart. And that the tooling finds what reading doesn't: AddressSanitizer diagnosed a stack overflow instantly that a bare segfault told me nothing about, and the static analyser caught an unsigned-underflow bug in the imbalance calculation that would have silently returned garbage.

### 30.7 Questions to ask them

- How do you shard books across cores, and how do you handle symbols with wildly different volumes?
- Where does your latency actually go end to end — what fraction is NIC, kernel, engine?
- Do you run kernel bypass? DPDK, AF_XDP, or a Solarflare/Onload stack?
- How do you test determinism — do you replay production sessions against new builds?
- What's your approach to the risk layer? Pre-trade checks are on the critical path; how do you keep them cheap?

---

## Chapter 31: Known Gaps and Upgrade Paths

Being able to enumerate your own system's weaknesses is the strongest signal of seniority. Here is the honest list.

### 31.1 Correctness gaps (fix these first)

| # | Gap | Impact | Fix |
|---|-----|--------|-----|
| 1 | Client supplies its own `client_id` | Any client can impersonate another and cancel their orders | Assign on `accept`, store in `ClientState`, overwrite the field |
| 2 | `push` return ignored (both rings) | Messages silently dropped when full; clients never learn of fills | Backpressure inbound (deregister fd); spin or buffer outbound |
| 3 | Gateway broadcasts to all clients | O(clients) per message; leaks other participants' fills | `client_id → fd` map, per-client outbound queue |
| 4 | One message parsed per readable event | A `recv` delivering two frames loses the second | Loop consuming all complete frames, `memmove` remainder |
| 5 | No boundary validation | Out-of-range enums, zero quantities pass through | Range-check every field before the cast |
| 6 | Host byte order on the wire | Non-portable across endianness | Specify big-endian, use `htonl`/`ntohl` |
| 7 | Status-line data race | Benign but UB | Publish a snapshot through a third ring, or make fields atomic |
| 8 | Integer reject reasons | Unclear, typo-prone | `enum class RejectReason : uint8_t` |

**Fixed during development** (kept here because the failure modes are instructive):

| Was | Symptom | Fix |
|-----|---------|-----|
| `Gateway` ctor `perror`'d on bind failure but returned normally | Process started, printed its banner, and busy-spun a core on a socket it never owned — looked healthy, accepted nothing | `ready_` flag; `main` checks it and exits 1 |
| `main` slept in 5-second chunks | Ctrl-C took up to 5s to take effect (`sleep_for` loops on `EINTR`) | Poll at 100 ms, print every 50th tick — shutdown now ~0.13s |
| `errno`/`EAGAIN` used without `<cerrno>`; `std::chrono` without `<chrono>` | Compiled on macOS via transitive includes; latent break on another libc | Include them explicitly |
| `pin_thread` ignored both return values | Silent failure — you couldn't tell whether pinning or `SCHED_FIFO` actually applied | Check and warn per thread; validate core index against `_SC_NPROCESSORS_ONLN` |
| `target_link_libraries(... pthread)` | Bare library name, fragile across Linux distros | `find_package(Threads)` + `Threads::Threads` |

### 31.2 Performance upgrades, ranked by value

**1. Hierarchical bitset for best-price tracking** *(Chapter 13.8)*
The only non-O(1) operation in the book. One bit per level in three tiers, ~2 MB, three `__builtin_clzll` calls. Directly attacks the cancel p99.9. Highest value, moderate effort.

**2. AF_XDP data path**
The deployment box has a Realtek Killer E3000 (r8169), which has supported XDP since kernel 5.12. An `XDP_REDIRECT` program steers order-entry packets into an AF_XDP socket, bypassing the kernel TCP stack entirely — typically 1–2 µs saved per message, which is two orders of magnitude more than anything left in the engine. Requires implementing framing above raw packets. Highest absolute latency win.

**3. `io_uring` for the gateway** *(Chapter 24.6)*
Removes both the `epoll_wait` and `recv` syscalls. Simpler than AF_XDP, smaller win.

**4. Huge pages for the book and pool** *(Chapter 6.7)*
`madvise(MADV_HUGEPAGE)` on the 48 MB price arrays and 64 MB pool cuts TLB entries from ~28,000 to ~56. Then pre-fault every page at startup so no page fault occurs during trading. Cheap to implement.

**5. Prefetch pipeline in the engine** *(Chapter 6.6)*
Peek at message N+1 while processing N and `__builtin_prefetch` the price level it will touch. Hides an L2/L3 miss behind useful work. Requires a ring `peek()`.

**6. Core isolation** *(Chapter 27.3)*
`isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2` plus the performance governor and NIC IRQ affinity away from the engine core. Pure configuration, attacks the 12 µs max directly.

**7. Batch ring operations**
`push_n`/`pop_n` amortise the atomic index update across several messages. Meaningful under burst load.

### 31.3 Missing features

- **Market data dissemination.** No ITCH-style UDP multicast feed. Subscribers currently learn nothing; only order owners get fills.
- **Symbol sharding.** Single book, single symbol. The path to multi-symbol is one engine thread per shard (§30.2).
- **Pre-trade risk.** No position limits, fat-finger checks, or credit limits. These are mandatory in reality and sit on the critical path, so they must be cheap — typically a per-client array indexed by `client_id`.
- **Persistence and recovery.** No input journal, so a crash loses all state. Determinism means a sequenced input log is sufficient to rebuild — that's the design to implement.
- **Order types.** Only limit and market. IOC, FOK, post-only, iceberg, and stop orders are policy layers over the same core.
- **Auctions.** No open/close crossing auction, which is where a large fraction of real volume trades.
- **Self-trade prevention.** A client can currently trade with themselves.

### 31.4 What is genuinely good

Balance matters too — do not only recite faults.

- The core data structure choices are correct and defensible from first principles, not copied.
- The hot path has no allocation, no locks, no exceptions, no virtual dispatch, and no system calls.
- The concurrency is minimal by design and provably correct, not incidentally race-free.
- The struct layouts are deliberate and measured, not accidental.
- The benchmarks isolate individual operations and report percentiles rather than means.
- Every simplification is documented with its ceiling and upgrade path.

---

## Chapter 32: Capstone — Build It Yourself

The real test. Rebuild TradeFeed from an empty directory. Each stage is independently runnable and testable — do not proceed until the current one works.

### Stage 1 — Types and layout

Write `types.h`: the aliases, the scoped enums, the constants, `rdtsc`, and the `Order` struct.

**Done when**: a program prints `sizeof(Order) == 64` and the `offsetof` of every field matches Chapter 3.5.

### Stage 2 — Order pool

Write `order_pool.h`.

**Done when**: allocating N orders returns N distinct pointers; deallocating and reallocating returns the most recent one first (prove LIFO); `available()` is correct throughout. Benchmark against `new`/`delete` and record the ratio.

### Stage 3 — Price level

Write `price_level.h`.

**Done when**: a `validate()` walking the list confirms `count_` and `total_qty_` after every operation in a randomised sequence of pushes, removes, and `adjust_qty` calls. Trace Chapter 12.6's arithmetic by hand first.

### Stage 4 — Order book, insert and cancel only

Write `order_book.h`/`.cpp` with `add_order` that *never matches* (assume no crossing) and `cancel_order`. Include the BBO tracking and scans.

**Done when**: inserting and cancelling in random order leaves `best_bid_`/`best_ask_` correct at every step, verified against a brute-force scan of the whole array.

### Stage 5 — Matching

Add `drain_level`, `match_limit`, `match_market`, and the match-then-rest logic in `add_order`.

**Done when**: Chapter 21.9's worked example reproduces exactly — three fills, correct prices, correct residual book. Then assert the crossed-book invariant after every operation in a randomised workload.

### Stage 6 — Ring buffer

Write `spsc_ring.h`.

**Done when**: single-threaded push/pop round-trips correctly; two threads transfer 10M messages with no loss, duplication, or reordering; ThreadSanitizer reports clean. Then deliberately swap the store and the write in `push` and find the corruption.

### Stage 7 — Messages and engine

Write `sbe_messages.h` and `matching_engine.h`/`.cpp`.

**Done when**: pre-filling the inbound ring and draining it produces the expected outbound sequence — one accept per order, two executions per fill, correct match numbers.

### Stage 8 — Gateway

Write `socket_transport.h`. Handle partial reads, `EAGAIN`, disconnection, and the edge-triggered drain loop.

**Done when**: a client connects, sends an order split across two `send` calls, and receives the acknowledgement. Then send three messages in one `send` and confirm all three are processed — which requires fixing gap #4 from Chapter 31.

### Stage 9 — Wire it together

Write `main.cpp` with threads, pinning, and signal handling. Write the `CMakeLists.txt`.

**Done when**: the exchange starts, accepts clients, matches orders end to end, prints status, and shuts down cleanly on Ctrl-C with no leaks under ASan.

### Stage 10 — Benchmark

Write the benchmark suite with TSC calibration and percentile reporting.

**Done when**: you can explain every number, including the tail, and your explanation survives someone asking "why?" three times.

### Stage 11 — Beat it

Pick one item from Chapter 31.2. Implement it. Measure before and after. Write up what changed and why.

**That write-up is your portfolio piece.** A system someone built is interesting; a measured improvement someone reasoned their way to is hireable.

---

## Appendix A: Glossary

**Adverse selection** — systematically trading against better-informed counterparties.
**Aggressor** — the incoming order that crosses the spread. Synonym: taker.
**Arena** — a large pre-allocated memory block subdivided by a custom allocator.
**BBO** — Best Bid and Offer; the top of book.
**Cache line** — the unit of memory transfer, 64 bytes on x86-64 and Apple Silicon.
**CAS** — compare-and-swap; the atomic read-modify-write primitive underlying lock-free algorithms.
**Crossed book** — an invalid state where the best bid ≥ the best ask.
**Depth** — resting quantity at a level, or cumulative across several.
**False sharing** — two cores writing distinct variables in the same cache line, causing coherence traffic.
**FIFO** — first in, first out. The queue discipline that implements time priority.
**Intrusive container** — one whose link pointers live inside the stored element.
**Invariant TSC** — a timestamp counter that ticks at a constant rate regardless of CPU frequency.
**Level 1 / Level 2** — BBO only / full depth by price.
**Maker** — the resting order that provided liquidity.
**MESI** — the Modified/Exclusive/Shared/Invalid cache coherence protocol.
**Mid** — (best bid + best ask) / 2.
**Nagle's algorithm** — TCP small-write batching; disabled with `TCP_NODELAY`.
**ODR** — One Definition Rule: exactly one definition per entity across the program.
**Pointer chasing** — following a chain of pointers, where each load's address depends on the previous load's result. Serialises cache misses.
**Pro-rata** — an allocation rule splitting fills proportionally to size rather than by arrival time.
**RAII** — Resource Acquisition Is Initialisation: tie resource lifetime to object lifetime.
**Slippage** — the difference between expected and realised execution price.
**Spread** — best ask minus best bid.
**SPSC** — single producer, single consumer.
**Taker** — the aggressive order that consumed liquidity.
**Tick** — the minimum price increment.
**TLB** — Translation Lookaside Buffer; caches virtual-to-physical address translations.
**VPIN** — Volume-Synchronized Probability of Informed Trading; a flow toxicity metric.
**VWAP** — Volume-Weighted Average Price.

---

## Appendix B: Latency Reference

Memorise the orders of magnitude.

| Operation | Time |
|-----------|------|
| CPU cycle (3 GHz) | 0.33 ns |
| L1 cache hit | ~1 ns |
| Branch mispredict | ~5 ns |
| L2 cache hit | ~4 ns |
| L3 cache hit | ~12 ns |
| Atomic increment (uncontended) | ~7 ns |
| DRAM access | ~80 ns |
| Cache line bounce between cores | ~40–70 ns |
| Mutex lock/unlock (uncontended) | ~20 ns |
| `malloc`/`free` | ~40–100 ns |
| System call | ~100 ns – 1 µs |
| Context switch | ~1–5 µs |
| Thread migration penalty | ~5–50 µs |
| Kernel TCP stack (loopback round trip) | ~10–30 µs |
| Nagle delay | up to 40 ms |

And TradeFeed's measured figures:

| Operation | Value |
|-----------|-------|
| Ring push (burst) | 2.5 ns |
| Ring pop (burst) | 2.3 ns |
| Ring round trip (single thread) | 5.4 ns |
| Engine per order (in-process) | ~15 ns |
| Book op p99 | 42–125 ns |
| Book op p99.9 | 208–625 ns |
| Engine throughput | 67M orders/sec |
| Ring throughput (cross-core) | 161M msgs/sec |

---

## Appendix C: Further Reading

**Books**
- Hennessy & Patterson, *Computer Architecture: A Quantitative Approach* — the memory hierarchy, properly.
- Anthony Williams, *C++ Concurrency in Action* — the definitive treatment of the C++ memory model.
- Harris & Fraser et al., *The Art of Multiprocessor Programming* — lock-free algorithms.
- Larry Harris, *Trading and Exchanges* — market microstructure, the standard text.
- Fabozzi & Focardi, *High-Frequency Trading* — the industry context.

**Papers**
- Easley, López de Prado & O'Hara, "Flow Toxicity and Liquidity in a High-Frequency World" (2012) — VPIN.
- Drepper, "What Every Programmer Should Know About Memory" (2007) — still the best single reference on caches.
- Michael & Scott, "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" (1996).

**Specifications**
- NASDAQ TotalView-ITCH 5.0 — market data protocol.
- NASDAQ OUCH 5.0 — order entry protocol.
- FIX Simple Binary Encoding (SBE) — CME's wire format.

**Tools**
- `perf` — Linux profiling; `perf c2c` for false sharing specifically.
- Intel VTune — microarchitectural analysis.
- Compiler Explorer (godbolt.org) — see the assembly for any snippet.
- Google Benchmark — production-grade microbenchmarking.
- HdrHistogram — latency recording with bounded memory.

---

*You now have everything needed to rebuild this system, defend every decision in it, and explain exactly where it falls short. That last part is what separates an engineer from someone who followed a tutorial.*






