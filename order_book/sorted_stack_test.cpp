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
    };
    for (auto& t : tests) {
        printf("running %s...\n", t.name);
        fflush(stdout);
        t.fn();
    }
    printf("all tests passed\n");
}
