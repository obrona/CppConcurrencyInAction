#include "lock_free_stack.cpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

namespace {

int tests_run = 0;
int tests_passed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++tests_run;                                                           \
        if (cond) {                                                            \
            ++tests_passed;                                                    \
        } else {                                                               \
            std::cerr << "FAILED: " << #cond << " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
        }                                                                      \
    } while (0)

// pop() on an empty stack returns a null shared_ptr.
void test_pop_empty() {
    lock_free_stack<int> s;
    std::shared_ptr<int> r = s.pop();
    CHECK(r == nullptr);
}

// A single push followed by a pop returns the pushed value.
void test_push_then_pop() {
    lock_free_stack<int> s;
    s.push(42);
    std::shared_ptr<int> r = s.pop();
    CHECK(r != nullptr);
    CHECK(r != nullptr && *r == 42);
}

// The stack is LIFO: values come out in reverse order of insertion.
void test_lifo_order() {
    lock_free_stack<int> s;
    for (int i = 0; i < 5; ++i) s.push(i);
    for (int i = 4; i >= 0; --i) {
        std::shared_ptr<int> r = s.pop();
        CHECK(r != nullptr && *r == i);
    }
    CHECK(s.pop() == nullptr);
}

// Popping past the end keeps returning null, then push works again.
void test_pop_past_end_then_reuse() {
    lock_free_stack<int> s;
    s.push(1);
    CHECK(*s.pop() == 1);
    CHECK(s.pop() == nullptr);
    CHECK(s.pop() == nullptr);
    s.push(2);
    CHECK(*s.pop() == 2);
}

// Interleaved pushes and pops behave like a stack.
void test_interleaved() {
    lock_free_stack<int> s;
    s.push(1);
    s.push(2);
    CHECK(*s.pop() == 2);
    s.push(3);
    CHECK(*s.pop() == 3);
    CHECK(*s.pop() == 1);
    CHECK(s.pop() == nullptr);
}

// Works with a non-trivial element type.
void test_string_type() {
    lock_free_stack<std::string> s;
    s.push("hello");
    s.push("world");
    CHECK(*s.pop() == "world");
    CHECK(*s.pop() == "hello");
}

// Many concurrent producers; a single consumer must observe every value
// exactly once, with no duplicates and no losses.
void test_concurrent_push_then_pop() {
    constexpr int num_threads = 8;
    constexpr int per_thread = 10;
    lock_free_stack<int> s;

    std::vector<std::thread> producers;
    for (int t = 0; t < num_threads; ++t) {
        producers.emplace_back([&s, t] {
            for (int i = 0; i < per_thread; ++i) {
                s.push(t * per_thread + i);
            }
        });
    }
    for (auto& th : producers) th.join();

    std::set<int> seen;
    for (int i = 0; i < num_threads * per_thread; ++i) {
        std::shared_ptr<int> r = s.pop();
        CHECK(r != nullptr);
        if (r) {
            auto [_, inserted] = seen.insert(*r);
            CHECK(inserted);  // no duplicates
        }
    }
    CHECK(s.pop() == nullptr);
    CHECK(static_cast<int>(seen.size()) == num_threads * per_thread);
}

// Concurrent producers and consumers running simultaneously. Every value
// pushed must be popped exactly once across all consumers.
void test_concurrent_push_pop() {
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
    constexpr int per_thread = 20;
    constexpr int total = num_producers * per_thread;

    lock_free_stack<int> s;
    std::atomic<int> pop_count{0};
    std::vector<std::atomic<int>> counts(total);
    for (auto& c : counts) c.store(0);

    std::vector<std::thread> threads;
    for (int t = 0; t < num_producers; ++t) {
        threads.emplace_back([&s, t] {
            for (int i = 0; i < per_thread; ++i) {
                s.push(t * per_thread + i);
            }
        });
    }
    for (int t = 0; t < num_consumers; ++t) {
        threads.emplace_back([&] {
            while (pop_count.load() < total) {
                std::shared_ptr<int> r = s.pop();
                if (r) {
                    counts[*r].fetch_add(1);
                    pop_count.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(pop_count.load() == total);
    bool all_once = true;
    for (int i = 0; i < total; ++i) {
        if (counts[i].load() != 1) all_once = false;
    }
    CHECK(all_once);  // every value popped exactly once
    CHECK(s.pop() == nullptr);
}

}  // namespace

int main() {
    test_pop_empty();
    test_push_then_pop();
    test_lifo_order();
    test_pop_past_end_then_reuse();
    test_interleaved();
    test_string_type();
    test_concurrent_push_then_pop();
    test_concurrent_push_pop();

    std::cout << tests_passed << "/" << tests_run << " checks passed\n";
    return tests_passed == tests_run ? 0 : 1;
}
