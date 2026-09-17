#pragma once

#include "types.h"
#include <memory>
#include <limits>

// Three-tier occupancy bitset over the price range: one bit per price level,
// set when that level holds orders. Each tier summarises the one below, so
// finding the highest or lowest occupied price is a fixed number of
// count-leading/trailing-zero instructions rather than a linear scan.
//
//   L0: one bit per price          15,625 words  (125 KB)
//   L1: one bit per L0 word           245 words  (1.9 KB)
//   L2: one bit per L1 word             4 words  (32 B)
//
// Invariant: an L1 bit is set iff its L0 word is non-zero; likewise L2 over L1.
class PriceBitset {
    static constexpr size_t L0_WORDS = (PRICE_LEVELS + 63) / 64;
    static constexpr size_t L1_WORDS = (L0_WORDS + 63) / 64;
    static constexpr size_t L2_WORDS = (L1_WORDS + 63) / 64;

    std::unique_ptr<uint64_t[]> l0_;
    std::unique_ptr<uint64_t[]> l1_;
    uint64_t l2_[L2_WORDS] = {};

public:
    static constexpr size_t NONE = std::numeric_limits<size_t>::max();

    PriceBitset()
        : l0_(new uint64_t[L0_WORDS]())
        , l1_(new uint64_t[L1_WORDS]()) {}

    void set(size_t i) {
        const size_t w0 = i >> 6;
        const size_t w1 = w0 >> 6;
        l0_[w0] |= 1ULL << (i  & 63);
        l1_[w1] |= 1ULL << (w0 & 63);
        l2_[w1 >> 6] |= 1ULL << (w1 & 63);
    }

    void clear(size_t i) {
        const size_t w0 = i >> 6;
        l0_[w0] &= ~(1ULL << (i & 63));
        if (l0_[w0]) return;                    // word still occupied

        const size_t w1 = w0 >> 6;
        l1_[w1] &= ~(1ULL << (w0 & 63));
        if (l1_[w1]) return;

        l2_[w1 >> 6] &= ~(1ULL << (w1 & 63));
    }

    bool test(size_t i) const {
        return (l0_[i >> 6] >> (i & 63)) & 1ULL;
    }

    // Highest occupied index, or NONE.
    size_t highest() const {
        for (size_t w2 = L2_WORDS; w2-- > 0; ) {
            if (!l2_[w2]) continue;
            const size_t w1 = (w2 << 6) + top_bit(l2_[w2]);
            const size_t w0 = (w1 << 6) + top_bit(l1_[w1]);
            return (w0 << 6) + top_bit(l0_[w0]);
        }
        return NONE;
    }

    // Lowest occupied index, or NONE.
    size_t lowest() const {
        for (size_t w2 = 0; w2 < L2_WORDS; ++w2) {
            if (!l2_[w2]) continue;
            const size_t w1 = (w2 << 6) + __builtin_ctzll(l2_[w2]);
            const size_t w0 = (w1 << 6) + __builtin_ctzll(l1_[w1]);
            return (w0 << 6) + __builtin_ctzll(l0_[w0]);
        }
        return NONE;
    }

private:
    static size_t top_bit(uint64_t w) { return 63 - __builtin_clzll(w); }
};
