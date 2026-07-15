#include "single_lane_bridge.cpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
using namespace std;
using namespace std::chrono_literals;

// The bridge exposes no state, so the only things a test can observe are:
//   - whether enter() returns or blocks,
//   - when the cleanup callback runs.
// Blocking is negative evidence, so it can only be established by waiting: we
// give an operation that *should* happen a generous deadline, and an operation
// that *should not* happen a shorter grace period.
constexpr auto SETTLE = 100ms;   // long enough for a thread that can run, to run
constexpr auto DEADLINE = 2s;    // an operation that never completes within this is stuck

// Spins until flag is set or the deadline passes. Returns whether it was set.
bool wait_for(atomic<bool>& flag, chrono::milliseconds timeout = DEADLINE) {
    auto end = chrono::steady_clock::now() + timeout;
    while (chrono::steady_clock::now() < end) {
        if (flag.load()) return true;
        this_thread::sleep_for(1ms);
    }
    return flag.load();
}

auto no_cleanup = [] {};

// ---------------------------------------------------------------------------
// Single threaded: enter() never blocks when the far side is empty, so one
// thread can drive a whole crossing and watch when cleanup fires.
// ---------------------------------------------------------------------------

void test_single_crossing_runs_cleanup() {
    single_lane_bridge b;
    int cleaned = 0;
    b.enter(0);
    b.leave(0, [&] { cleaned++; });
    assert(cleaned == 1);   // last one off the bridge cleans it
}

void test_cleanup_runs_for_either_side() {
    single_lane_bridge b;
    int cleaned = 0;
    for (int side : {0, 1}) {
        b.enter(side);
        b.leave(side, [&] { cleaned++; });
    }
    assert(cleaned == 2);
}

// Cleanup marks the end of a batch, not the end of one crossing: it must run
// only when the last occupant of the side leaves.
void test_cleanup_only_on_last_leaver() {
    single_lane_bridge b;
    int cleaned = 0;
    auto cleanup = [&] { cleaned++; };

    b.enter(0);
    b.enter(0);
    b.enter(0);
    b.leave(0, cleanup);
    assert(cleaned == 0);
    b.leave(0, cleanup);
    assert(cleaned == 0);
    b.leave(0, cleanup);
    assert(cleaned == 1);
}

// Sides are counted independently: emptying one must not fire the other's
// cleanup, and each side gets its own cleanup when it drains.
void test_sides_are_counted_independently() {
    single_lane_bridge b;
    int cleaned0 = 0, cleaned1 = 0;

    b.enter(0);
    b.leave(0, [&] { cleaned0++; });
    assert(cleaned0 == 1 && cleaned1 == 0);

    b.enter(1);
    b.leave(1, [&] { cleaned1++; });
    assert(cleaned0 == 1 && cleaned1 == 1);
}

// Back-to-back batches on one side: the counter must return to zero each time,
// so every batch cleans up rather than only the first.
void test_repeated_batches_on_one_side() {
    single_lane_bridge b;
    int cleaned = 0;
    for (int i = 0; i < 100; i++) {
        b.enter(0);
        b.enter(0);
        b.leave(0, [&] { cleaned++; });
        b.leave(0, [&] { cleaned++; });
    }
    assert(cleaned == 100);   // one per drained batch, not one per leave
}

// ---------------------------------------------------------------------------
// Blocking behaviour
// ---------------------------------------------------------------------------

// Traffic in the same direction shares the bridge, so a second entrant on an
// occupied side must not wait for the first to leave.
void test_same_side_does_not_block() {
    single_lane_bridge b;
    atomic<bool> second_entered{false};

    b.enter(0);   // main thread holds the bridge for side 0
    jthread t{[&] {
        b.enter(0);
        second_entered = true;
        b.leave(0, no_cleanup);
    }};
    assert(wait_for(second_entered));

    b.leave(0, no_cleanup);
}

// The core exclusion property: while one side is on the bridge, the other must
// wait, and it must be released once the bridge drains.
void test_opposite_side_blocks_until_bridge_clears() {
    single_lane_bridge b;
    atomic<bool> crossed{false};

    b.enter(0);
    jthread t{[&] {
        b.enter(1);
        crossed = true;
        b.leave(1, no_cleanup);
    }};

    this_thread::sleep_for(SETTLE);
    assert(!crossed);   // still blocked: side 0 is on the bridge

    b.leave(0, no_cleanup);
    assert(wait_for(crossed));
}

// The far side waits for the whole batch to clear, not just for the count to
// drop. With two occupants, one leaving must not release it.
void test_opposite_side_waits_for_every_occupant() {
    single_lane_bridge b;
    atomic<bool> crossed{false};

    b.enter(0);
    b.enter(0);
    jthread t{[&] {
        b.enter(1);
        crossed = true;
        b.leave(1, no_cleanup);
    }};

    this_thread::sleep_for(SETTLE);
    assert(!crossed);

    b.leave(0, no_cleanup);       // one down, one still on the bridge
    this_thread::sleep_for(SETTLE);
    assert(!crossed);

    b.leave(0, no_cleanup);       // bridge now empty
    assert(wait_for(crossed));
}

// Every waiter on the far side is released by the one drain, not just one of
// them (the implementation notifies all).
void test_all_waiters_released_when_bridge_clears() {
    constexpr int WAITERS = 8;
    single_lane_bridge b;
    atomic<int> crossed{0};
    atomic<bool> all_crossed{false};

    b.enter(0);
    {
        vector<jthread> ts;
        for (int i = 0; i < WAITERS; i++) {
            ts.emplace_back([&] {
                b.enter(1);
                if (crossed.fetch_add(1) + 1 == WAITERS) all_crossed = true;
                b.leave(1, no_cleanup);
            });
        }

        this_thread::sleep_for(SETTLE);
        assert(crossed == 0);

        b.leave(0, no_cleanup);
        assert(wait_for(all_crossed));
    }
}

// Cleanup is the point of the handoff: whatever it does to the bridge must be
// done before the far side is let on.
void test_cleanup_runs_before_opposite_side_enters() {
    single_lane_bridge b;
    atomic<bool> cleaned{false};
    atomic<bool> saw_cleanup{false};
    atomic<bool> crossed{false};

    b.enter(0);
    jthread t{[&] {
        b.enter(1);
        saw_cleanup = cleaned.load();   // must observe the finished cleanup
        crossed = true;
        b.leave(1, no_cleanup);
    }};

    this_thread::sleep_for(SETTLE);   // let the waiter actually block
    b.leave(0, [&] { cleaned = true; });

    assert(wait_for(crossed));
    assert(saw_cleanup);
}

// ---------------------------------------------------------------------------
// Concurrent use
// ---------------------------------------------------------------------------

// Wraps the bridge with test-side bookkeeping: it tracks who is on the bridge
// and asserts, from inside the crossing, that the two sides never overlap.
struct traffic {
    single_lane_bridge b;
    atomic<int> on_bridge[2] = {};
    atomic<int> cleanups{0};

    void cross(int side) {
        b.enter(side);
        on_bridge[side].fetch_add(1);
        assert(on_bridge[!side].load() == 0);   // exclusion: never a head-on
        this_thread::sleep_for(chrono::microseconds(50));
        assert(on_bridge[!side].load() == 0);   // and not just at the moment of entry
        on_bridge[side].fetch_sub(1);
        b.leave(side, [&] {
            // cleanup runs on an empty bridge: the last occupant of this side
            // has left and the far side has not been admitted yet
            assert(on_bridge[0].load() == 0 && on_bridge[1].load() == 0);
            cleanups.fetch_add(1);
        });
    }
};

// Two sides pushing against each other: only one direction may hold the bridge
// at a time, and no thread may get stuck (all threads must join).
void test_sides_never_mix_under_contention() {
    constexpr int PER_SIDE = 4, CROSSINGS = 300;
    traffic t;
    {
        vector<jthread> ts;
        for (int i = 0; i < PER_SIDE * 2; i++) {
            ts.emplace_back([&t, side = i % 2] {
                for (int j = 0; j < CROSSINGS; j++) t.cross(side);
            });
        }
    }   // join: a deadlock or lost wakeup would hang here
    assert(t.on_bridge[0] == 0 && t.on_bridge[1] == 0);
    assert(t.cleanups > 0);
}

// Same invariant with threads picking sides at random, which mixes batch sizes
// and lets a side drain and refill unpredictably.
void test_random_sides_under_contention() {
    constexpr int THREADS = 8, CROSSINGS = 300;
    traffic t;
    {
        vector<jthread> ts;
        for (int i = 0; i < THREADS; i++) {
            ts.emplace_back([&t, i] {
                mt19937 rng(i);
                bernoulli_distribution coin(0.5);
                for (int j = 0; j < CROSSINGS; j++) t.cross(coin(rng));
            });
        }
    }
    assert(t.on_bridge[0] == 0 && t.on_bridge[1] == 0);
    assert(t.cleanups > 0);
}

// Strict alternation between the two sides: every crossing hands the bridge
// over, so this exercises the wait/notify path on each iteration.
void test_alternating_handoff() {
    constexpr int ROUNDS = 500;
    single_lane_bridge b;
    atomic<int> cleanups{0};
    atomic<int> turn{0};   // whose turn it is to cross

    {
        vector<jthread> ts;
        for (int side : {0, 1}) {
            ts.emplace_back([&, side] {
                for (int r = 0; r < ROUNDS; r++) {
                    while (turn.load() != side) this_thread::yield();
                    b.enter(side);
                    b.leave(side, [&] { cleanups.fetch_add(1); });
                    turn.store(!side);
                }
            });
        }
    }
    assert(cleanups == 2 * ROUNDS);   // each side drains once per round
}

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"single_crossing_runs_cleanup", test_single_crossing_runs_cleanup},
        {"cleanup_runs_for_either_side", test_cleanup_runs_for_either_side},
        {"cleanup_only_on_last_leaver", test_cleanup_only_on_last_leaver},
        {"sides_are_counted_independently", test_sides_are_counted_independently},
        {"repeated_batches_on_one_side", test_repeated_batches_on_one_side},
        {"same_side_does_not_block", test_same_side_does_not_block},
        {"opposite_side_blocks_until_bridge_clears", test_opposite_side_blocks_until_bridge_clears},
        {"opposite_side_waits_for_every_occupant", test_opposite_side_waits_for_every_occupant},
        {"all_waiters_released_when_bridge_clears", test_all_waiters_released_when_bridge_clears},
        {"cleanup_runs_before_opposite_side_enters", test_cleanup_runs_before_opposite_side_enters},
        {"sides_never_mix_under_contention", test_sides_never_mix_under_contention},
        {"random_sides_under_contention", test_random_sides_under_contention},
        {"alternating_handoff", test_alternating_handoff},
    };
    for (auto& t : tests) {
        printf("running %s...\n", t.name);
        fflush(stdout);
        t.fn();
    }
    printf("all tests passed\n");
}
