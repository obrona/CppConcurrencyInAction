#pragma once
#include <atomic>

std::atomic<int> timestamp{0};

int get_time() {
    return timestamp.fetch_add(1, std::memory_order_relaxed);
}