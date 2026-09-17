// Self-check for the Alpaca normalisation path. Uses the exact example
// messages from Alpaca's documentation as fixtures.
//   build:  make feed_test && ./feed_test
#include "feed_normalizer.h"

#include <cassert>
#include <cstdio>
#include <string_view>
#include <vector>

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

int main() {
    // --- price conversion -------------------------------------------------
    // 91.95 * 100 is 9194.99999... in binary floating point; naive truncation
    // would yield 9194. This is the reason to_cents rounds.
    CHECK(feed::to_cents(91.95)  == 9195);
    CHECK(feed::to_cents(91.98)  == 9198);
    CHECK(feed::to_cents(162.92) == 16292);
    CHECK(feed::to_cents(0.01)   == 1);
    CHECK(feed::to_cents(9999.99) == 999999);
    CHECK(feed::to_cents(0.0)    == INVALID_PRICE);
    CHECK(feed::to_cents(20000.0) == 0);          // above MAX_PRICE -> rejected

    // --- RFC-3339 with nanosecond precision -------------------------------
    const uint64_t ns = feed::parse_rfc3339_ns("2023-04-06T11:54:26.838232225Z");
    CHECK(ns / 1'000'000'000ULL == 1680782066ULL);        // epoch seconds
    CHECK(ns % 1'000'000'000ULL == 838232225ULL);         // nanos preserved
    CHECK(feed::parse_rfc3339_ns("1970-01-01T00:00:00Z") == 0);
    // fractional part must be scaled, not taken literally
    CHECK(feed::parse_rfc3339_ns("2021-02-22T19:15:00.5Z") % 1'000'000'000ULL
          == 500'000'000ULL);

    // --- quote (verbatim from Alpaca docs) --------------------------------
    {
        constexpr std::string_view msg =
            R"({"T":"q","S":"AMD","bx":"K","bp":91.95,"bs":2,"ax":"Q","ap":91.98,"as":1,)"
            R"("c":["R"],"z":"C","t":"2023-04-06T11:54:21.670905508Z"})";
        CaptureRecord r{};
        CHECK(feed::normalize(msg, r));
        CHECK(r.type == static_cast<uint8_t>(TickType::Quote));
        CHECK(std::string_view(r.symbol, 3) == "AMD");
        CHECK(r.p1 == 9195);    // bid
        CHECK(r.s1 == 2);
        CHECK(r.p2 == 9198);    // ask
        CHECK(r.s2 == 1);
        CHECK(r.p1 < r.p2);     // bid below ask
        CHECK(r.venue_ns > 0);
    }

    // --- trade (verbatim from Alpaca docs) --------------------------------
    {
        constexpr std::string_view msg =
            R"({"T":"t","S":"AAPL","i":628,"x":"K","p":162.92,"s":3,)"
            R"("c":["@","F","T","I"],"z":"C","t":"2023-04-06T11:54:26.838232225Z"})";
        CaptureRecord r{};
        CHECK(feed::normalize(msg, r));
        CHECK(r.type == static_cast<uint8_t>(TickType::Trade));
        CHECK(std::string_view(r.symbol, 4) == "AAPL");
        CHECK(r.p1 == 16292);
        CHECK(r.s1 == 3);
        CHECK(r.p2 == 0 && r.s2 == 0);   // unused for trades
    }

    // --- control messages must be skipped, not misparsed -------------------
    {
        CaptureRecord r{};
        CHECK(!feed::normalize(R"({"T":"success","msg":"authenticated"})", r));
        CHECK(!feed::normalize(R"({"T":"subscription","trades":["AAPL"]})", r));
        CHECK(!feed::normalize(R"({"T":"error","code":400,"msg":"invalid syntax"})", r));
        // bars are market data but not something the book consumes
        CHECK(!feed::normalize(R"({"T":"b","S":"SPY","o":388.9,"c":389.1,"v":49378})", r));
    }

    // --- array splitting ---------------------------------------------------
    {
        constexpr std::string_view frame =
            R"([{"T":"q","S":"AMD","bp":91.95,"bs":2,"ap":91.98,"as":1},)"
            R"({"T":"t","S":"AAPL","p":162.92,"s":3},)"
            R"({"T":"success","msg":"authenticated"}])";
        std::vector<CaptureRecord> got;
        feed::for_each_object(frame, [&](std::string_view o) {
            CaptureRecord r{};
            if (feed::normalize(o, r)) got.push_back(r);
        });
        CHECK(got.size() == 2);                 // the control message is dropped
        CHECK(got[0].type == static_cast<uint8_t>(TickType::Quote));
        CHECK(got[1].type == static_cast<uint8_t>(TickType::Trade));
    }

    // --- a brace inside a string must not split an object ------------------
    {
        constexpr std::string_view frame =
            R"([{"T":"q","S":"A}B","bp":10.00,"bs":1,"ap":10.01,"as":1}])";
        int n = 0;
        feed::for_each_object(frame, [&](std::string_view) { ++n; });
        CHECK(n == 1);
    }

    // --- malformed input must be rejected, not crash -----------------------
    {
        CaptureRecord r{};
        CHECK(!feed::normalize("", r));
        CHECK(!feed::normalize("{}", r));
        CHECK(!feed::normalize(R"({"T":"q"})", r));               // no symbol
        CHECK(!feed::normalize(R"({"T":"q","S":"X"})", r));       // no prices
    }

    std::printf("feed_test: %d checks passed\n", checks);
    return 0;
}
