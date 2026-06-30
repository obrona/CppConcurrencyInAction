// Tests for thread_safe_queue.cpp
//
// Build & run:
//   clang++ -std=c++23 -stdlib=libc++ -pthread test_thread_safe_queue.cpp -o test_tsq && ./test_tsq
//
// thread_safe_queue.cpp defines its own main(); we rename it so it does not
// collide with the test runner's main() below, without modifying the source.
#define main impl_main_unused
#include "thread_safe_queue.cpp"
#undef main

#include <algorithm>
#include <atomic>
#include <print>
#include <string>
#include <thread>
#include <vector>

// ---- tiny test harness ------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::println("  FAIL [{}:{}] {}", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

#define RUN(test)                                                             \
    do {                                                                       \
        std::println("RUN  {}", #test);                                        \
        test();                                                                \
    } while (0)

// ---- single-threaded behaviour ---------------------------------------------

void test_pop_empty_returns_null() {
    queue<int> q;
    CHECK(q.try_pop() == nullptr);
    // Still empty after a failed pop.
    CHECK(q.try_pop() == nullptr);
}

void test_push_then_pop() {
    queue<int> q;
    q.push(42);
    auto v = q.try_pop();
    CHECK(v != nullptr);
    CHECK(*v == 42);
    // Drained again.
    CHECK(q.try_pop() == nullptr);
}

void test_fifo_order() {
    queue<int> q;
    for (int i = 0; i < 100; ++i) q.push(i);
    for (int i = 0; i < 100; ++i) {
        auto v = q.try_pop();
        CHECK(v != nullptr);
        CHECK(*v == i);
    }
    CHECK(q.try_pop() == nullptr);
}

void test_interleaved_push_pop() {
    queue<int> q;
    q.push(1);
    q.push(2);
    CHECK(*q.try_pop() == 1);
    q.push(3);
    CHECK(*q.try_pop() == 2);
    CHECK(*q.try_pop() == 3);
    CHECK(q.try_pop() == nullptr);
    // Reuse after draining.
    q.push(4);
    CHECK(*q.try_pop() == 4);
}

void test_non_trivial_type() {
    queue<std::string> q;
    q.push("hello");
    q.push("world");
    CHECK(*q.try_pop() == "hello");
    CHECK(*q.try_pop() == "world");
    CHECK(q.try_pop() == nullptr);
}

// ---- concurrency ------------------------------------------------------------

// Many producers and consumers run at once. Every produced value is distinct,
// so we can assert that the set of popped values exactly equals the set of
// pushed values -- no item lost, duplicated, or corrupted.
void test_concurrent_producers_consumers() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 10000;
    constexpr int kTotal = kProducers * kPerProducer;

    queue<int> q;
    std::atomic<int> popped{0};
    std::vector<std::vector<int>> collected(kConsumers);

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i)
                q.push(p * kPerProducer + i);
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c] {
            auto &out = collected[c];
            while (popped.load(std::memory_order_relaxed) < kTotal) {
                if (auto v = q.try_pop()) {
                    out.push_back(*v);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto &t : threads) t.join();

    std::vector<int> all;
    all.reserve(kTotal);
    for (auto &v : collected) all.insert(all.end(), v.begin(), v.end());

    CHECK(static_cast<int>(all.size()) == kTotal);
    std::sort(all.begin(), all.end());
    bool exact = true;
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
        if (all[i] != i) { exact = false; break; }
    CHECK(exact);                       // every value 0..kTotal-1 popped once
    CHECK(q.try_pop() == nullptr);      // queue fully drained
}

// A single producer streaming into a single consumer must preserve FIFO order.
void test_single_producer_single_consumer_fifo() {
    constexpr int kN = 50000;
    queue<int> q;
    std::vector<int> got;
    got.reserve(kN);

    std::thread producer([&] {
        for (int i = 0; i < kN; ++i) q.push(i);
    });
    std::thread consumer([&] {
        int seen = 0;
        while (seen < kN) {
            if (auto v = q.try_pop()) { got.push_back(*v); ++seen; }
            else std::this_thread::yield();
        }
    });
    producer.join();
    consumer.join();

    CHECK(static_cast<int>(got.size()) == kN);
    bool ordered = true;
    for (int i = 0; i < static_cast<int>(got.size()); ++i)
        if (got[i] != i) { ordered = false; break; }
    CHECK(ordered);
}

int main() {
    RUN(test_pop_empty_returns_null);
    RUN(test_push_then_pop);
    RUN(test_fifo_order);
    RUN(test_interleaved_push_pop);
    RUN(test_non_trivial_type);
    RUN(test_concurrent_producers_consumers);
    RUN(test_single_producer_single_consumer_fifo);

    std::println("");
    std::println("{}/{} checks passed", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
