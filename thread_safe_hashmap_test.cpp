// Tests for thread_safe_hashmap.cpp
//
// Build & run:
//   g++ -std=c++23 -pthread thread_safe_hashmap_test.cpp -o test_tshm && ./test_tshm
//
// thread_safe_hashmap.cpp defines its own main(); we rename it so it does not
// collide with the test runner's main() below, without modifying the source.
#define main impl_main_unused
#include "thread_safe_hashmap.cpp"
#undef main

#include <atomic>
#include <memory>
#include <print>
#include <set>
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

void test_missing_key_returns_default() {
    hashmap<int, int> m;
    CHECK(m.value_for(123) == 0);          // default-constructed Value
    CHECK(m.value_for(123, -1) == -1);     // caller-supplied default
    CHECK(m.size() == 0);
}

void test_add_then_lookup() {
    hashmap<int, int> m;
    m.add_or_update_mapping(1, 100);
    CHECK(m.value_for(1) == 100);
    CHECK(m.value_for(2, -1) == -1);       // unrelated key untouched
    CHECK(m.size() == 1);
}

void test_update_existing_key() {
    hashmap<int, int> m;
    m.add_or_update_mapping(1, 100);
    m.add_or_update_mapping(1, 200);       // overwrite, not insert
    CHECK(m.value_for(1) == 200);
    CHECK(m.size() == 1);                  // size must not grow on update
}

void test_remove_existing_key() {
    hashmap<int, int> m;
    m.add_or_update_mapping(1, 100);
    m.add_or_update_mapping(2, 200);
    m.remove_mapping(1);
    CHECK(m.value_for(1, -1) == -1);       // gone
    CHECK(m.value_for(2) == 200);          // sibling survives
    CHECK(m.size() == 1);
}

void test_remove_missing_key_is_noop() {
    hashmap<int, int> m;
    m.add_or_update_mapping(1, 100);
    m.remove_mapping(999);                 // not present
    CHECK(m.value_for(1) == 100);
    CHECK(m.size() == 1);                  // count unchanged
}

void test_add_remove_readd() {
    hashmap<int, int> m;
    m.add_or_update_mapping(5, 50);
    m.remove_mapping(5);
    CHECK(m.size() == 0);
    m.add_or_update_mapping(5, 55);        // reuse key after removal
    CHECK(m.value_for(5) == 55);
    CHECK(m.size() == 1);
}

// Many keys, more than the bucket count, force collisions within buckets.
void test_many_keys_with_collisions() {
    hashmap<int, int> m(8);                // small table => guaranteed collisions
    constexpr int kN = 1000;
    for (int i = 0; i < kN; ++i) m.add_or_update_mapping(i, i * 2);
    CHECK(m.size() == kN);
    bool all_ok = true;
    for (int i = 0; i < kN; ++i)
        if (m.value_for(i, -1) != i * 2) { all_ok = false; break; }
    CHECK(all_ok);
}

void test_string_keys_and_values() {
    hashmap<std::string, std::string> m;
    m.add_or_update_mapping("hello", "world");
    m.add_or_update_mapping("foo", "bar");
    CHECK(m.value_for("hello") == "world");
    CHECK(m.value_for("foo") == "bar");
    CHECK(m.value_for("missing", "none") == "none");
    CHECK(m.value_for("missing") == "");   // default-constructed std::string
    CHECK(m.size() == 2);
}

// ---- shared_ptr values: references to stored objects for mutation -----------

// value_for() returns a *copy* of the stored Value. When Value is a
// shared_ptr<T>, that copy shares ownership of the same T, so callers can
// mutate the stored object in place through the returned pointer without
// re-inserting. (The user is responsible for synchronising concurrent
// mutation of a single shared object; the map only guards its own structure.)
struct Counter {
    int value = 0;
    std::string label;
};

void test_shared_ptr_value_mutation_visible() {
    hashmap<int, std::shared_ptr<Counter>> m;
    m.add_or_update_mapping(1, std::make_shared<Counter>(Counter{10, "a"}));

    // Fetch a handle to the stored object and mutate through it.
    auto handle = m.value_for(1);
    CHECK(handle != nullptr);
    handle->value = 42;
    handle->label = "changed";

    // A fresh lookup must observe the mutation: same underlying object.
    auto again = m.value_for(1);
    CHECK(again == handle);            // identical shared_ptr (same control block)
    CHECK(again->value == 42);
    CHECK(again->label == "changed");
    CHECK(m.size() == 1);
}

void test_shared_ptr_missing_key_is_null() {
    hashmap<int, std::shared_ptr<Counter>> m;
    CHECK(m.value_for(7) == nullptr);          // default-constructed shared_ptr
    CHECK(m.size() == 0);
}

// Replacing the mapping installs a *new* object; the old handle keeps the old
// object alive but is decoupled from the map.
void test_shared_ptr_update_replaces_object() {
    hashmap<int, std::shared_ptr<Counter>> m;
    auto first = std::make_shared<Counter>(Counter{1, "first"});
    m.add_or_update_mapping(1, first);

    auto second = std::make_shared<Counter>(Counter{2, "second"});
    m.add_or_update_mapping(1, second);        // overwrite the value

    auto current = m.value_for(1);
    CHECK(current == second);
    CHECK(current != first);
    CHECK(current->value == 2);
    CHECK(first->value == 1);                  // old object untouched, still alive
    CHECK(m.size() == 1);
}

// ---- concurrency ------------------------------------------------------------

// Each writer owns a disjoint key range, so there are no write/write conflicts
// on a single key. Afterwards every key must be present with the right value
// and the count must equal the total inserted.
void test_concurrent_disjoint_writers() {
    constexpr int kWriters = 8;
    constexpr int kPerWriter = 5000;
    constexpr int kTotal = kWriters * kPerWriter;

    hashmap<int, int> m;
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            int base = w * kPerWriter;
            for (int i = 0; i < kPerWriter; ++i)
                m.add_or_update_mapping(base + i, (base + i) + 7);
        });
    }
    for (auto &t : threads) t.join();

    CHECK(m.size() == static_cast<size_t>(kTotal));
    bool all_ok = true;
    for (int k = 0; k < kTotal; ++k)
        if (m.value_for(k, -1) != k + 7) { all_ok = false; break; }
    CHECK(all_ok);
}

// Many threads hammer the SAME key with add_or_update. The map must stay
// consistent: exactly one entry exists, its value is one of the written values,
// and the count is exactly 1 (no double-insert under the race).
void test_concurrent_same_key_updates() {
    constexpr int kWriters = 8;
    constexpr int kIters = 10000;

    hashmap<int, int> m;
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < kIters; ++i)
                m.add_or_update_mapping(42, w);   // value identifies the writer
        });
    }
    for (auto &t : threads) t.join();

    CHECK(m.size() == 1);
    int v = m.value_for(42, -1);
    CHECK(v >= 0 && v < kWriters);            // a valid last-writer value
}

// Readers run concurrently with writers. Readers must never crash or read torn
// data: any value they observe for a key is either the default or the exact
// value that was written for it.
void test_concurrent_readers_and_writers() {
    constexpr int kKeys = 2000;
    hashmap<int, int> m;

    std::atomic<bool> stop{false};
    std::atomic<bool> bad_read{false};

    std::thread writer([&] {
        for (int k = 0; k < kKeys; ++k) m.add_or_update_mapping(k, k * 3);
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (int k = 0; k < kKeys; ++k) {
                    int v = m.value_for(k, -1);
                    if (v != -1 && v != k * 3) bad_read.store(true);
                }
            }
        });
    }

    writer.join();
    for (auto &t : readers) t.join();

    CHECK(!bad_read.load());
    CHECK(m.size() == static_cast<size_t>(kKeys));
}

// Interleaved inserts and removals over disjoint key ranges. Each writer
// inserts its range then removes half of it; the surviving entries and the
// final count must match exactly.
void test_concurrent_inserts_and_removals() {
    constexpr int kWriters = 8;
    constexpr int kPerWriter = 4000;

    hashmap<int, int> m;
    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            int base = w * kPerWriter;
            for (int i = 0; i < kPerWriter; ++i)
                m.add_or_update_mapping(base + i, base + i);
            for (int i = 0; i < kPerWriter; i += 2)   // remove the even offsets
                m.remove_mapping(base + i);
        });
    }
    for (auto &t : threads) t.join();

    size_t expected_survivors = 0;
    bool all_ok = true;
    for (int w = 0; w < kWriters; ++w) {
        int base = w * kPerWriter;
        for (int i = 0; i < kPerWriter; ++i) {
            int k = base + i;
            if (i % 2 == 0) {
                if (m.value_for(k, -1) != -1) all_ok = false;   // removed
            } else {
                ++expected_survivors;
                if (m.value_for(k, -1) != k) all_ok = false;    // kept
            }
        }
    }
    CHECK(all_ok);
    CHECK(m.size() == expected_survivors);
}

int main() {
    RUN(test_missing_key_returns_default);
    RUN(test_add_then_lookup);
    RUN(test_update_existing_key);
    RUN(test_remove_existing_key);
    RUN(test_remove_missing_key_is_noop);
    RUN(test_add_remove_readd);
    RUN(test_many_keys_with_collisions);
    RUN(test_string_keys_and_values);
    RUN(test_shared_ptr_value_mutation_visible);
    RUN(test_shared_ptr_missing_key_is_null);
    RUN(test_shared_ptr_update_replaces_object);
    RUN(test_concurrent_disjoint_writers);
    RUN(test_concurrent_same_key_updates);
    RUN(test_concurrent_readers_and_writers);
    RUN(test_concurrent_inserts_and_removals);

    std::println("");
    std::println("{}/{} checks passed", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
