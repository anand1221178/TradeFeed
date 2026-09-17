#pragma once

#include "../core/types.h"

#include <cstring>
#include <cstdlib>
#include <string_view>
#include <optional>

// Alpaca streams JSON arrays of fixed-shape messages. A general JSON parser
// would allocate and walk a DOM; these messages have known keys, so we scan
// for the key and parse the value in place. No allocation, no DOM.
//
// Quote: {"T":"q","S":"AMD","bx":"K","bp":91.95,"bs":2,"ax":"Q","ap":91.98,"as":1,...}
// Trade: {"T":"t","S":"AAPL","i":628,"x":"K","p":162.92,"s":3,...}

enum class TickType : uint8_t { Quote = 'q', Trade = 't' };

// Fixed 32-byte capture record, same size as WireMessage (Chapter 25).
// Quote: p1/s1 = bid, p2/s2 = ask.  Trade: p1/s1 = price/size, p2/s2 = 0.
#pragma pack(push, 1)
struct CaptureRecord {
    uint8_t  type;
    char     symbol[7];
    uint32_t p1;
    uint32_t s1;
    uint32_t p2;
    uint32_t s2;
    uint64_t venue_ns;
};
static_assert(sizeof(CaptureRecord) == 32, "capture record must be 32 bytes");
#pragma pack(pop)

namespace feed {

// Dollars -> integer cents, rounded to nearest. 91.95 * 100 is 9194.999... in
// binary floating point, so truncation alone would give 9194.
inline Price to_cents(double dollars) {
    if (dollars <= 0.0) return INVALID_PRICE;
    double cents = dollars * 100.0 + 0.5;
    if (cents > static_cast<double>(MAX_PRICE)) return 0;   // out of range
    return static_cast<Price>(cents);
}

// Locate "key": and return a view starting at the value.
inline std::optional<std::string_view> field(std::string_view obj, std::string_view key) {
    char pat[24];
    const size_t n = key.size();
    if (n + 3 >= sizeof(pat)) return std::nullopt;
    pat[0] = '"';
    std::memcpy(pat + 1, key.data(), n);
    pat[n + 1] = '"';
    pat[n + 2] = ':';
    const std::string_view needle(pat, n + 3);

    const size_t at = obj.find(needle);
    if (at == std::string_view::npos) return std::nullopt;
    return obj.substr(at + needle.size());
}

inline std::optional<double> field_num(std::string_view obj, std::string_view key) {
    auto v = field(obj, key);
    if (!v) return std::nullopt;
    return std::strtod(v->data(), nullptr);
}

inline std::optional<std::string_view> field_str(std::string_view obj, std::string_view key) {
    auto v = field(obj, key);
    if (!v || v->empty() || v->front() != '"') return std::nullopt;
    auto rest = v->substr(1);
    const size_t end = rest.find('"');
    if (end == std::string_view::npos) return std::nullopt;
    return rest.substr(0, end);
}

// Days since 1970-01-01 from a civil date. Howard Hinnant's algorithm —
// exact, branch-light, and valid across the whole proleptic Gregorian range.
inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "2023-04-06T11:54:26.838232225Z" -> nanoseconds since the Unix epoch.
// Fixed-shape, so parsed positionally rather than with strptime (which cannot
// do sub-second precision and would cost a locale lookup per call).
inline uint64_t parse_rfc3339_ns(std::string_view s) {
    if (s.size() < 20) return 0;
    auto num = [&](size_t off, size_t len) -> int64_t {
        int64_t v = 0;
        for (size_t i = 0; i < len; ++i) {
            const char c = s[off + i];
            if (c < '0' || c > '9') return v;
            v = v * 10 + (c - '0');
        }
        return v;
    };

    const int64_t days = days_from_civil(static_cast<int>(num(0, 4)),
                                         static_cast<unsigned>(num(5, 2)),
                                         static_cast<unsigned>(num(8, 2)));
    const int64_t secs = days * 86400 + num(11, 2) * 3600 + num(14, 2) * 60 + num(17, 2);

    int64_t nanos = 0;
    if (s.size() > 20 && s[19] == '.') {
        size_t i = 20, digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9) {
            nanos = nanos * 10 + (s[i] - '0');
            ++i; ++digits;
        }
        while (digits++ < 9) nanos *= 10;      // pad to nanosecond scale
    }
    return static_cast<uint64_t>(secs) * 1'000'000'000ULL + static_cast<uint64_t>(nanos);
}

// Parse one Alpaca message object into a capture record.
// Returns false for message types we do not consume (success, subscription,
// error, bars) — those are control traffic, not market data.
inline bool normalize(std::string_view obj, CaptureRecord& out) {
    auto t = field_str(obj, "T");
    if (!t || t->empty()) return false;

    const char kind = (*t)[0];
    if (kind != 'q' && kind != 't') return false;

    auto sym = field_str(obj, "S");
    if (!sym) return false;

    out = CaptureRecord{};
    out.type = static_cast<uint8_t>(kind);
    const size_t n = std::min(sym->size(), sizeof(out.symbol));
    std::memcpy(out.symbol, sym->data(), n);

    if (auto ts = field_str(obj, "t")) out.venue_ns = parse_rfc3339_ns(*ts);

    if (kind == 'q') {
        auto bp = field_num(obj, "bp"), ap = field_num(obj, "ap");
        auto bs = field_num(obj, "bs"), as = field_num(obj, "as");
        if (!bp || !ap) return false;
        out.p1 = to_cents(*bp);
        out.s1 = bs ? static_cast<uint32_t>(*bs) : 0;
        out.p2 = to_cents(*ap);
        out.s2 = as ? static_cast<uint32_t>(*as) : 0;
        if (out.p1 == 0 || out.p2 == 0) return false;      // out of range
    } else {
        auto p = field_num(obj, "p"), sz = field_num(obj, "s");
        if (!p) return false;
        out.p1 = to_cents(*p);
        out.s1 = sz ? static_cast<uint32_t>(*sz) : 0;
        if (out.p1 == 0) return false;
    }
    return true;
}

// Alpaca frames are JSON arrays: [{...},{...}]. Split on top-level object
// boundaries, tracking string state so a '}' inside a symbol cannot fool us.
template <typename Fn>
void for_each_object(std::string_view frame, Fn&& fn) {
    int depth = 0;
    bool in_str = false, esc = false;
    size_t start = 0;

    for (size_t i = 0; i < frame.size(); ++i) {
        const char c = frame[i];
        if (esc)            { esc = false; continue; }
        if (c == '\\')      { esc = true;  continue; }
        if (c == '"')       { in_str = !in_str; continue; }
        if (in_str) continue;

        if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') {
            if (--depth == 0) fn(frame.substr(start, i - start + 1));
        }
    }
}

} // namespace feed
