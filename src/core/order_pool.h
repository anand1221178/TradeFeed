#pragma once

#include "types.h"
#include <vector>
#include <cassert>

class OrderPool {
    std::vector<Order> arena_;
    std::vector<Order*> free_list_;

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
