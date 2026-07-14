#include "lock_free_queue.cpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
using namespace std;

// pop() returns an empty unique_ptr when the queue looks empty, so
// consumers must spin until a value actually arrives.
template <typename T>
T pop_blocking(lock_free_queue<T>& q) {
    for (;;) {
        if (auto p = q.pop()) return *p;
        this_thread::yield();
    }
}

// Counts live instances so tests can detect leaked / double-destroyed values.
struct tracked {
    static inline atomic<int> live{0};
    int v;
    tracked(int v = 0) : v(v) { live.fetch_add(1, memory_order_relaxed); }
    tracked(const tracked& o) : v(o.v) { live.fetch_add(1, memory_order_relaxed); }
    ~tracked() { live.fetch_sub(1, memory_order_relaxed); }
};

void test_single_thread_fifo() {
    lock_free_queue<int> q;
    for (int i = 0; i < 1000; i++) q.push(i);
    for (int i = 0; i < 1000; i++) {
        auto p = q.pop();
        assert(p && *p == i);
    }
    assert(!q.pop());
}

void test_pop_empty_returns_null() {
    lock_free_queue<int> q;
    assert(!q.pop());
    assert(!q.pop());
    q.push(42);
    auto p = q.pop();
    assert(p && *p == 42);
    assert(!q.pop());
}

void test_interleaved_push_pop() {
    lock_free_queue<int> q;
    for (int i = 0; i < 500; i++) {
        q.push(2 * i);
        q.push(2 * i + 1);
        auto p = q.pop();
        assert(p && *p == i);
    }
    for (int i = 500; i < 1000; i++) {
        auto p = q.pop();
        assert(p && *p == i);
    }
    assert(!q.pop());
}

void test_destruction_with_remaining_items() {
    tracked::live = 0;
    {
        lock_free_queue<tracked> q;
        for (int i = 0; i < 100; i++) q.push(tracked{i});
        // destructor must free the 100 values still in the queue
    }
    assert(tracked::live == 0);
}

void test_no_value_leaks_after_pop() {
    tracked::live = 0;
    {
        lock_free_queue<tracked> q;
        for (int i = 0; i < 100; i++) q.push(tracked{i});
        for (int i = 0; i < 100; i++) {
            auto p = q.pop();
            assert(p && p->v == i);
        }
    }
    assert(tracked::live == 0);
}

void test_multi_producers_single_consumer() {
    lock_free_queue<int> q;
    long long sum = 0;
    constexpr int PRODUCERS = 10, PER_PRODUCER = 1000;
    {
        vector<jthread> threads;
        threads.emplace_back([&q, &sum] {
            for (int i = 0; i < PRODUCERS * PER_PRODUCER; i++)
                sum += pop_blocking(q);
        });
        for (int i = 0; i < PRODUCERS; i++) {
            threads.emplace_back([&q] {
                for (int j = 0; j < PER_PRODUCER; j++) q.push(1);
            });
        }
    }
    assert(sum == PRODUCERS * PER_PRODUCER);
}

void test_single_producer_multi_consumers() {
    constexpr int CONSUMERS = 4, TOTAL = 1000;
    lock_free_queue<int> q;
    atomic<int> popped{0};
    vector<vector<int>> got(CONSUMERS);
    {
        vector<jthread> threads;
        for (int c = 0; c < CONSUMERS; c++) {
            threads.emplace_back([&, c] {
                while (popped.load() < TOTAL) {
                    if (auto p = q.pop()) {
                        got[c].push_back(*p);
                        popped.fetch_add(1);
                    } else {
                        this_thread::yield();
                    }
                }
            });
        }
        threads.emplace_back([&q] {
            for (int i = 0; i < TOTAL; i++) q.push(i);
        });
    }

    // Single producer + FIFO queue: each consumer's sequence must be increasing.
    vector<int> all;
    for (auto& v : got) {
        assert(is_sorted(v.begin(), v.end()));
        all.insert(all.end(), v.begin(), v.end());
    }
    // Every value received exactly once.
    assert((int)all.size() == TOTAL);
    sort(all.begin(), all.end());
    for (int i = 0; i < TOTAL; i++) assert(all[i] == i);
}

void test_multi_producers_multi_consumers() {
    constexpr int PRODUCERS = 4, CONSUMERS = 4, PER_PRODUCER = 10000;
    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;
    lock_free_queue<int> q;
    atomic<int> popped{0};
    vector<vector<int>> got(CONSUMERS);
    {
        vector<jthread> threads;
        for (int c = 0; c < CONSUMERS; c++) {
            threads.emplace_back([&, c] {
                while (popped.load() < TOTAL) {
                    if (auto p = q.pop()) {
                        got[c].push_back(*p);
                        popped.fetch_add(1);
                    } else {
                        this_thread::yield();
                    }
                }
            });
        }
        for (int i = 0; i < PRODUCERS; i++) {
            threads.emplace_back([&q, i] {
                for (int j = 0; j < PER_PRODUCER; j++)
                    q.push(i * PER_PRODUCER + j);  // globally unique values
            });
        }
    }

    // Every value received exactly once, across all consumers.
    vector<int> all;
    for (auto& v : got) all.insert(all.end(), v.begin(), v.end());
    assert((int)all.size() == TOTAL);
    sort(all.begin(), all.end());
    for (int i = 0; i < TOTAL; i++) assert(all[i] == i);

    // FIFO per producer: within one consumer's stream, values from the
    // same producer must appear in the order they were pushed.
    for (auto& v : got) {
        vector<int> last(PRODUCERS, -1);
        for (int x : v) {
            int producer = x / PER_PRODUCER, seq = x % PER_PRODUCER;
            assert(seq > last[producer]);
            last[producer] = seq;
        }
    }
}

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"single_thread_fifo", test_single_thread_fifo},
        {"pop_empty_returns_null", test_pop_empty_returns_null},
        {"interleaved_push_pop", test_interleaved_push_pop},
        {"destruction_with_remaining_items", test_destruction_with_remaining_items},
        {"no_value_leaks_after_pop", test_no_value_leaks_after_pop},
        {"multi_producers_single_consumer", test_multi_producers_single_consumer},
        {"single_producer_multi_consumers", test_single_producer_multi_consumers},
        {"multi_producers_multi_consumers", test_multi_producers_multi_consumers},
    };
    for (auto& t : tests) {
        printf("running %s...\n", t.name);
        fflush(stdout);
        t.fn();
    }
    printf("all tests passed\n");
}
