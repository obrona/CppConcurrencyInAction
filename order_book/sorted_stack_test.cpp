#include "sorted_stack.cpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>
#include <vector>
using namespace std;

// Walks the internal list (skipping the dummy head) into a vector, so tests
// can inspect contents and order. Safe only in the "read" phase, when no
// thread is mutating the stack.
template <typename T, typename C>
vector<T> to_vector(sorted_stack<T, C>& s) {
    vector<T> out;
    for (auto* n = s.head->next.load(); n; n = n->next.load())
        out.push_back(*n->data);
    return out;
}

// Counts live instances so tests can detect leaked / double-destroyed values.
struct tracked {
    static inline atomic<int> live{0};
    int v;
    tracked(int v = 0) : v(v) { live.fetch_add(1, memory_order_relaxed); }
    tracked(const tracked& o) : v(o.v) { live.fetch_add(1, memory_order_relaxed); }
    ~tracked() { live.fetch_sub(1, memory_order_relaxed); }
    bool operator<(const tracked& o) const { return v < o.v; }
};

void test_empty_stack() {
    sorted_stack<int> s;
    assert(to_vector(s).empty());
}

void test_single_element() {
    sorted_stack<int> s;
    s.add(42);
    auto v = to_vector(s);
    assert(v.size() == 1 && v[0] == 42);
}

void test_keeps_sorted_ascending() {
    sorted_stack<int> s;
    int input[] = {5, 1, 4, 2, 8, 0, 3, 9, 7, 6};
    for (int x : input) s.add(x);
    auto v = to_vector(s);
    vector<int> expected(begin(input), end(input));
    sort(expected.begin(), expected.end());
    assert(v == expected);
}

void test_already_sorted_input() {
    sorted_stack<int> s;
    for (int i = 0; i < 100; i++) s.add(i);
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((int)v.size() == 100);
}

void test_reverse_sorted_input() {
    sorted_stack<int> s;
    for (int i = 99; i >= 0; i--) s.add(i);
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    for (int i = 0; i < 100; i++) assert(v[i] == i);
}

void test_duplicates_are_kept() {
    sorted_stack<int> s;
    int input[] = {3, 1, 3, 2, 1, 3, 2};
    for (int x : input) s.add(x);
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert(v.size() == 7);                       // no dedup: every add stored
    assert(count(v.begin(), v.end(), 3) == 3);
    assert(count(v.begin(), v.end(), 1) == 2);
    assert(count(v.begin(), v.end(), 2) == 2);
}

void test_custom_comparator_descending() {
    sorted_stack<int, greater<int>> s;
    int input[] = {5, 1, 4, 2, 8, 0};
    for (int x : input) s.add(x);
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end(), greater<int>{}));
    assert(v.front() == 8 && v.back() == 0);
}

void test_no_value_leaks_on_destruction() {
    tracked::live = 0;
    {
        sorted_stack<tracked> s;
        for (int i = 0; i < 100; i++) s.add(tracked{i});
        assert((int)to_vector(s).size() == 100);
        // destructor must free the 100 stored values (and the dummy head)
    }
    assert(tracked::live == 0);
}

// Phased concurrency: every thread adds, then a single thread reads. All
// values must be present exactly once and the list must be fully sorted.
void test_concurrent_adds() {
    constexpr int THREADS = 8, PER_THREAD = 5000;
    constexpr int TOTAL = THREADS * PER_THREAD;
    sorted_stack<int> s;
    {
        vector<jthread> ts;
        for (int t = 0; t < THREADS; t++) {
            ts.emplace_back([&s, t] {
                for (int j = 0; j < PER_THREAD; j++)
                    s.add(t * PER_THREAD + j);   // globally unique values
            });
        }
    }  // join all adders before reading

    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((int)v.size() == TOTAL);
    for (int i = 0; i < TOTAL; i++) assert(v[i] == i);  // each value once
}

// Same as above but many threads hammer a small set of duplicate keys, which
// stresses the CAS retry path around equal elements.
void test_concurrent_adds_with_duplicates() {
    constexpr int THREADS = 8, PER_THREAD = 4000, KEYS = 16;
    sorted_stack<int> s;
    {
        vector<jthread> ts;
        for (int t = 0; t < THREADS; t++) {
            ts.emplace_back([&s] {
                mt19937 rng(random_device{}());
                uniform_int_distribution<int> dist(0, KEYS - 1);
                for (int j = 0; j < PER_THREAD; j++) s.add(dist(rng));
            });
        }
    }

    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((int)v.size() == THREADS * PER_THREAD);
    for (int k : v) assert(k >= 0 && k < KEYS);
}

// ---------------------------------------------------------------------------
// clean(): removes every node whose value satisfies pred. Per the type's
// contract only one thread may call it, and no other thread may touch the
// stack while it runs, so these tests exercise it single-threaded.
// ---------------------------------------------------------------------------

void test_clean_on_empty_stack() {
    sorted_stack<int> s;
    s.clean([](int) { return true; });   // must not walk off the dummy head
    assert(to_vector(s).empty());
}

void test_clean_removes_nothing() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int) { return false; });
    auto v = to_vector(s);
    assert((int)v.size() == 10);
    for (int i = 0; i < 10; i++) assert(v[i] == i);
}

void test_clean_removes_everything() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int) { return true; });
    assert(to_vector(s).empty());
}

// Removing the first real node exercises relinking through the dummy head.
void test_clean_removes_front() {
    sorted_stack<int> s;
    for (int i = 0; i < 5; i++) s.add(i);
    s.clean([](int x) { return x == 0; });
    auto v = to_vector(s);
    assert((v == vector<int>{1, 2, 3, 4}));
}

// Removing the last node exercises the end-of-list case: after unlinking, the
// scan must stop rather than follow a null next.
void test_clean_removes_back() {
    sorted_stack<int> s;
    for (int i = 0; i < 5; i++) s.add(i);
    s.clean([](int x) { return x == 4; });
    auto v = to_vector(s);
    assert((v == vector<int>{0, 1, 2, 3}));
}

void test_clean_removes_middle() {
    sorted_stack<int> s;
    for (int i = 0; i < 5; i++) s.add(i);
    s.clean([](int x) { return x == 2; });
    auto v = to_vector(s);
    assert((v == vector<int>{0, 1, 3, 4}));
}

// A run of adjacent removals: after unlinking a node, curr must stay put and
// re-examine the new next, otherwise consecutive matches get skipped.
void test_clean_removes_consecutive_run() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int x) { return x >= 3 && x <= 6; });
    auto v = to_vector(s);
    assert((v == vector<int>{0, 1, 2, 7, 8, 9}));
}

void test_clean_removes_alternating() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int x) { return x % 2 == 0; });
    auto v = to_vector(s);
    assert((v == vector<int>{1, 3, 5, 7, 9}));
}

void test_clean_removes_all_duplicates_of_a_key() {
    sorted_stack<int> s;
    int input[] = {3, 1, 3, 2, 1, 3, 2};
    for (int x : input) s.add(x);
    s.clean([](int x) { return x == 3; });
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert(count(v.begin(), v.end(), 3) == 0);
    assert((v == vector<int>{1, 1, 2, 2}));
}

void test_clean_keeps_stack_sorted() {
    sorted_stack<int> s;
    int input[] = {5, 1, 4, 2, 8, 0, 3, 9, 7, 6};
    for (int x : input) s.add(x);
    s.clean([](int x) { return x % 3 == 0; });   // drops 0, 3, 6, 9
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((v == vector<int>{1, 2, 4, 5, 7, 8}));
}

void test_clean_respects_custom_comparator_order() {
    sorted_stack<int, greater<int>> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int x) { return x < 5; });
    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end(), greater<int>{}));
    assert((v == vector<int>{9, 8, 7, 6, 5}));
}

// clean must destroy the values it removes, not just unlink them, and the
// destructor must not then free them a second time.
void test_clean_frees_removed_values() {
    tracked::live = 0;
    {
        sorted_stack<tracked> s;
        for (int i = 0; i < 100; i++) s.add(tracked{i});
        assert(tracked::live == 100);

        s.clean([](const tracked& t) { return t.v < 60; });   // removes 60 values
        assert((int)to_vector(s).size() == 40);
        assert(tracked::live == 40);

        s.clean([](const tracked&) { return true; });
        assert(to_vector(s).empty());
        assert(tracked::live == 0);
    }
    assert(tracked::live == 0);   // destructor after a full clean: no double free
}

// Running clean a second time with the same predicate must be a no-op.
void test_clean_is_idempotent() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    auto pred = [](int x) { return x % 2 == 0; };
    s.clean(pred);
    auto first = to_vector(s);
    s.clean(pred);
    assert(to_vector(s) == first);
}

// Phase transition: the stack must still be a valid sorted list for adds after
// a clean, including re-inserting into the gaps clean opened up.
void test_add_after_clean() {
    sorted_stack<int> s;
    for (int i = 0; i < 10; i++) s.add(i);
    s.clean([](int x) { return x % 2 == 0; });   // leaves 1,3,5,7,9

    s.add(0);    // before the new front
    s.add(4);    // into a gap
    s.add(100);  // past the back

    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((v == vector<int>{0, 1, 3, 4, 5, 7, 9, 100}));
}

// Full phase cycle: concurrent adds, join, then a single-threaded clean, then
// read. clean itself is never concurrent with anything.
void test_clean_after_concurrent_adds() {
    constexpr int THREADS = 8, PER_THREAD = 5000;
    constexpr int TOTAL = THREADS * PER_THREAD;
    sorted_stack<int> s;
    {
        vector<jthread> ts;
        for (int t = 0; t < THREADS; t++) {
            ts.emplace_back([&s, t] {
                for (int j = 0; j < PER_THREAD; j++) s.add(t * PER_THREAD + j);
            });
        }
    }  // join all adders before cleaning

    s.clean([](int x) { return x % 2 == 0; });

    auto v = to_vector(s);
    assert(is_sorted(v.begin(), v.end()));
    assert((int)v.size() == TOTAL / 2);
    for (int i = 0; i < TOTAL / 2; i++) assert(v[i] == 2 * i + 1);
}

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"empty_stack", test_empty_stack},
        {"single_element", test_single_element},
        {"keeps_sorted_ascending", test_keeps_sorted_ascending},
        {"already_sorted_input", test_already_sorted_input},
        {"reverse_sorted_input", test_reverse_sorted_input},
        {"duplicates_are_kept", test_duplicates_are_kept},
        {"custom_comparator_descending", test_custom_comparator_descending},
        {"no_value_leaks_on_destruction", test_no_value_leaks_on_destruction},
        {"concurrent_adds", test_concurrent_adds},
        {"concurrent_adds_with_duplicates", test_concurrent_adds_with_duplicates},
        {"clean_on_empty_stack", test_clean_on_empty_stack},
        {"clean_removes_nothing", test_clean_removes_nothing},
        {"clean_removes_everything", test_clean_removes_everything},
        {"clean_removes_front", test_clean_removes_front},
        {"clean_removes_back", test_clean_removes_back},
        {"clean_removes_middle", test_clean_removes_middle},
        {"clean_removes_consecutive_run", test_clean_removes_consecutive_run},
        {"clean_removes_alternating", test_clean_removes_alternating},
        {"clean_removes_all_duplicates_of_a_key", test_clean_removes_all_duplicates_of_a_key},
        {"clean_keeps_stack_sorted", test_clean_keeps_stack_sorted},
        {"clean_respects_custom_comparator_order", test_clean_respects_custom_comparator_order},
        {"clean_frees_removed_values", test_clean_frees_removed_values},
        {"clean_is_idempotent", test_clean_is_idempotent},
        {"add_after_clean", test_add_after_clean},
        {"clean_after_concurrent_adds", test_clean_after_concurrent_adds},
    };
    for (auto& t : tests) {
        printf("running %s...\n", t.name);
        fflush(stdout);
        t.fn();
    }
    printf("all tests passed\n");
}
