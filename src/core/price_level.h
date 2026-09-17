#pragma once

#include "types.h"

class PriceLevel {
    Order*   head_      = nullptr;
    Order*   tail_      = nullptr;
    Quantity total_qty_ = 0;
    uint32_t count_     = 0;

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
