// Tests for thread_safe_list.cpp
//
// Build & run:
//   g++ -std=c++23 -pthread thread_safe_list_test.cpp -o test_tsl && ./test_tsl
//
// thread_safe_list.cpp defines no main(), so we can include it directly.
#include "thread_safe_list.cpp"

#include <algorithm>
#include <atomic>
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

// Snapshot the list into a vector, front-to-back, via for_each.
template <typename T>
std::vector<T> to_vector(list<T> &l) {
    std::vector<T> out;
    l.for_each([&](T &v) { out.push_back(v); });
    return out;
}

// ---- single-threaded behaviour ---------------------------------------------

void test_empty_list_iterates_nothing() {
    list<int> l;
    CHECK(to_vector(l).empty());
}

// push_front prepends, so iteration order is the reverse of insertion order.
void test_push_front_order_is_reversed() {
    list<int> l;
    l.push_front(1);
    l.push_front(2);
    l.push_front(3);
    CHECK((to_vector(l) == std::vector<int>{3, 2, 1}));
}

// insert_at_pred appends when nothing satisfies the predicate.
void test_insert_at_pred_appends_at_end() {
    list<int> l;
    // predicate never true => every insert lands at the tail.
    l.insert_at_pred(1, [](int) { return false; });
    l.insert_at_pred(2, [](int) { return false; });
    l.insert_at_pred(3, [](int) { return false; });
    CHECK((to_vector(l) == std::vector<int>{1, 2, 3}));
}

// insert_at_pred inserts before the first element that satisfies the predicate.
void test_insert_at_pred_inserts_before_match() {
    list<int> l;
    l.push_front(30);
    l.push_front(20);
    l.push_front(10);                          // list: 10, 20, 30
    // insert 25 before the first element that is > 25 (i.e. 30).
    l.insert_at_pred(25, [](int x) { return x > 25; });
    CHECK((to_vector(l) == std::vector<int>{10, 20, 25, 30}));
}

// *** Maintain an increasing (ascending) list of ints. ***
// To keep the list sorted, each new value is inserted before the first element
// strictly greater than it. Values equal-or-smaller are skipped, so the value
// slots in just after the run of things <= it, preserving ascending order.
void test_maintains_increasing_list() {
    list<int> l;
    auto sorted_insert = [&](int v) {
        l.insert_at_pred(v, [v](int x) { return x > v; });
    };

    // Insert in a deliberately scrambled order, including duplicates.
    for (int v : {5, 1, 9, 3, 7, 2, 8, 4, 6, 0, 5, 3}) sorted_insert(v);

    auto got = to_vector(l);

    // The list must be non-decreasing at every step...
    CHECK(std::is_sorted(got.begin(), got.end()));
    // ...and hold exactly what we inserted (as a multiset).
    std::vector<int> expected{5, 1, 9, 3, 7, 2, 8, 4, 6, 0, 5, 3};
    std::sort(expected.begin(), expected.end());
    CHECK(got == expected);
}

// remove_if deletes every element matching the predicate.
void test_remove_if_removes_all_matches() {
    list<int> l;
    l.push_front(3);
    l.push_front(2);
    l.push_front(2);
    l.push_front(1);                           // list: 1, 2, 2, 3
    l.remove_if([](int x) { return x == 2; }); // drops both 2s
    CHECK((to_vector(l) == std::vector<int>{1, 3}));
}

// Matches scattered through the list (including head and tail) are all removed.
void test_remove_if_removes_scattered_matches() {
    list<int> l;
    l.push_front(2);
    l.push_front(3);
    l.push_front(2);
    l.push_front(1);
    l.push_front(2);                           // list: 2, 1, 2, 3, 2
    l.remove_if([](int x) { return x == 2; });
    CHECK((to_vector(l) == std::vector<int>{1, 3}));
}

// A predicate matching everything empties the list in a single call.
void test_remove_if_matching_all_empties_list() {
    list<int> l;
    l.push_front(3);
    l.push_front(2);
    l.push_front(1);
    l.remove_if([](int) { return true; });
    CHECK(to_vector(l).empty());
}

void test_remove_if_no_match_is_noop() {
    list<int> l;
    l.push_front(3);
    l.push_front(2);
    l.push_front(1);                           // list: 1, 2, 3
    l.remove_if([](int x) { return x == 99; });
    CHECK((to_vector(l) == std::vector<int>{1, 2, 3}));
}

void test_remove_head_element() {
    list<int> l;
    l.push_front(3);
    l.push_front(2);
    l.push_front(1);                           // list: 1, 2, 3
    l.remove_if([](int x) { return x == 1; }); // remove the first node
    CHECK((to_vector(l) == std::vector<int>{2, 3}));
}

// remove_if on an already-empty list is a safe no-op.
void test_remove_if_on_empty_list_is_noop() {
    list<int> l;
    l.remove_if([](int) { return true; });
    CHECK(to_vector(l).empty());
    l.push_front(1);
    l.remove_if([](int) { return true; });     // empties it
    l.remove_if([](int) { return true; });     // now a no-op
    CHECK(to_vector(l).empty());
}

void test_works_with_strings() {
    list<std::string> l;
    l.push_front("c");
    l.push_front("b");
    l.push_front("a");                         // list: a, b, c
    CHECK((to_vector(l) == std::vector<std::string>{"a", "b", "c"}));
    l.remove_if([](const std::string &s) { return s == "b"; });
    CHECK((to_vector(l) == std::vector<std::string>{"a", "c"}));
}

// ---- concurrency ------------------------------------------------------------

// Many threads push disjoint value ranges concurrently. Every value must show
// up exactly once; the list's internal locking must not lose or duplicate any.
void test_concurrent_push_front() {
    constexpr int kThreads = 8;
    constexpr int kPer = 2000;

    list<int> l;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            int base = t * kPer;
            for (int i = 0; i < kPer; ++i) l.push_front(base + i);
        });
    }
    for (auto &t : ts) t.join();

    auto got = to_vector(l);
    CHECK(got.size() == static_cast<size_t>(kThreads * kPer));
    std::set<int> seen(got.begin(), got.end());
    CHECK(seen.size() == got.size());          // no duplicates
    CHECK(*seen.begin() == 0);
    CHECK(*seen.rbegin() == kThreads * kPer - 1);
}

// Writers push while a reader repeatedly walks the list. The reader must never
// crash or observe garbage; each value it sees is one that was actually pushed.
void test_concurrent_push_and_iterate() {
    constexpr int kPer = 20000;
    list<int> l;
    std::atomic<bool> done{false};
    std::atomic<bool> bad{false};

    std::thread writer([&] {
        for (int i = 0; i < kPer; ++i) l.push_front(i);
        done.store(true);
    });

    std::thread reader([&] {
        while (!done.load()) {
            l.for_each([&](int p) {
                if (p < 0 || p >= kPer) bad.store(true);
            });
        }
    });

    writer.join();
    reader.join();

    CHECK(!bad.load());
    CHECK(to_vector(l).size() == static_cast<size_t>(kPer));
}

// Concurrent inserts and removals over disjoint ranges: each thread pushes its
// range, then removes the even values. Only the odd values should survive.
void test_concurrent_insert_and_remove() {
    constexpr int kThreads = 6;
    constexpr int kPer = 3000;

    list<int> l;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            int base = t * kPer;
            for (int i = 0; i < kPer; ++i) l.push_front(base + i);
            for (int i = 0; i < kPer; i += 2)
                l.remove_if([v = base + i](int x) { return x == v; });
        });
    }
    for (auto &t : ts) t.join();

    auto got = to_vector(l);
    bool all_odd_offsets = true;
    for (int v : got) if ((v % kPer) % 2 == 0) all_odd_offsets = false;
    CHECK(all_odd_offsets);
    CHECK(got.size() == static_cast<size_t>(kThreads * (kPer / 2)));
}

// *** Maintain an ascending list under concurrent push / read / delete. ***
//
// Producers insert values with predicate `x > v`, i.e. before the first
// element strictly greater than v. insert_at_pred locks hand-over-hand, so a
// value always lands between a node <= v and the first node > v; that keeps the
// list ascending no matter how many producers insert at once. Readers walk the
// list (front-to-back) and assert each value is >= the previous one they saw:
// any value inserted between two observed nodes is, by the predicate, between
// their values, so an in-order walk can never observe a descending step.
// Deleters concurrently drop values via remove_if, which is also hand-over-hand
// and therefore can't break the ordering either.
void test_concurrent_sorted_invariant() {
    constexpr int kProducers = 4;
    constexpr int kPer = 4000;
    constexpr int kReaders = 3;
    constexpr int kDeleters = 3;
    constexpr int kMax = kProducers * kPer;   // exclusive upper bound on values
    constexpr int kStride = 7919;             // prime, coprime to kPer => permutes

    list<int> l;
    std::atomic<bool> done{false};
    std::atomic<bool> unsorted{false};        // a reader saw a descending step
    std::atomic<bool> out_of_range{false};    // a reader saw a bogus value

    auto sorted_insert = [&](int v) {
        l.insert_at_pred(v, [v](int x) { return x > v; });
    };

    // Start readers and deleters first so they race against the producers.
    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&] {
            while (!done.load()) {
                int prev = -1;                 // every real value is >= 0
                l.for_each([&](int v) {
                    if (v < prev) unsorted.store(true);
                    if (v < 0 || v >= kMax) out_of_range.store(true);
                    prev = v;
                });
            }
        });
    }

    std::vector<std::thread> deleters;
    for (int t = 0; t < kDeleters; ++t) {
        deleters.emplace_back([&, t] {
            int v = (t * 2) % kMax;            // each deleter starts staggered
            while (!done.load()) {
                l.remove_if([v](int x) { return x == v; });  // no-op if absent
                v = (v + 2) % kMax;            // sweep the even values
            }
        });
    }

    // Producers own disjoint value blocks, inserted in a scrambled (permuted)
    // order so insertions land all over the list rather than at one end.
    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            int base = t * kPer;
            for (int i = 0; i < kPer; ++i)
                sorted_insert(base + (i * kStride) % kPer);
        });
    }

    for (auto &t : producers) t.join();
    done.store(true);
    for (auto &t : readers) t.join();
    for (auto &t : deleters) t.join();

    // No reader ever observed a descending step or a garbage value.
    CHECK(!unsorted.load());
    CHECK(!out_of_range.load());

    // The final list is ascending, in range, and has no duplicates (every value
    // was inserted exactly once; removals only ever delete).
    auto got = to_vector(l);
    CHECK(std::is_sorted(got.begin(), got.end()));
    CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());
    for (int v : got) CHECK(v >= 0 && v < kMax);
    CHECK(got.size() <= static_cast<size_t>(kMax));
}

int main() {
    RUN(test_empty_list_iterates_nothing);
    RUN(test_push_front_order_is_reversed);
    RUN(test_insert_at_pred_appends_at_end);
    RUN(test_insert_at_pred_inserts_before_match);
    RUN(test_maintains_increasing_list);
    RUN(test_remove_if_removes_all_matches);
    RUN(test_remove_if_removes_scattered_matches);
    RUN(test_remove_if_matching_all_empties_list);
    RUN(test_remove_if_no_match_is_noop);
    RUN(test_remove_head_element);
    RUN(test_remove_if_on_empty_list_is_noop);
    RUN(test_works_with_strings);
    RUN(test_concurrent_push_front);
    RUN(test_concurrent_push_and_iterate);
    RUN(test_concurrent_insert_and_remove);
    RUN(test_concurrent_sorted_invariant);

    std::println("");
    std::println("{}/{} checks passed", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
