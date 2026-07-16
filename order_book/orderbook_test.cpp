#include "orderbook.cpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace std;

// ---------------------------------------------------------------------------
// How this framework works
//
// 1. Every fulfilled event (match, order coming to rest, cancel ok/fail) is
//    logged with a timestamp from the global atomic counter. Each thread logs
//    into its own buffer (event_log.cpp), so logging never serializes the run.
// 2. After all user threads are joined, the buffers are merged and sorted by
//    timestamp. Timestamps are unique (fetch_add), so this gives a total order
//    that is a valid linearization of the run: within a bridge phase a resting
//    order's cnt only ever decreases, and every event takes its timestamp
//    before publishing its effect, so an order observed empty was already
//    empty at every later timestamp.
// 3. The sorted log is replayed through a single-threaded model book, checking
//    the README rules at every step:
//      - matches only hit live resting orders of the opposite side,
//        at the resting order's price, within the active order's limit;
//      - an active order always hits the best resting price first;
//      - traded quantity == min(active remaining, resting remaining);
//      - an order rests exactly its unfilled remainder, and only when nothing
//        crossable is left on the other side;
//      - cancels succeed at most once (idempotent), succeed only on live
//        resting orders, and never fail while the target is live;
//      - each user's events replay their ops strictly in program order;
//      - the model's final book matches the real book, both sides are sorted,
//        and the final book is not crossed.
// ---------------------------------------------------------------------------

struct order_info { int user; bool is_sell; int price; int qty; };

struct op {
    enum kind_t { buy, sell, cancel } kind;
    int id;              // submit: this order's id; cancel: the target order's id
    int price = 0;
    int qty = 0;
};

struct scenario {
    vector<vector<op>> per_user;
    vector<order_info> orders;   // indexed by order id
};

int add_order(scenario& sc, int user, bool is_sell, int price, int qty) {
    int id = (int)sc.orders.size();
    sc.orders.push_back({user, is_sell, price, qty});
    sc.per_user[user].push_back({is_sell ? op::sell : op::buy, id, price, qty});
    return id;
}

void add_cancel(scenario& sc, int user, int target_id) {
    sc.per_user[user].push_back({op::cancel, target_id});
}

// ---------------------------------------------------------------------------
// runner
// ---------------------------------------------------------------------------

struct book_entry {
    int id, price, qty;
    bool operator==(const book_entry&) const = default;
};

// single-threaded walk of a book after the run; skips cancelled-but-unswept
// nodes (cnt == 0) and checks the live orders appear in comparator order.
template <typename C>
vector<book_entry> dump_book(sorted_stack<resting_order, C>& s, bool descending, bool& sorted_ok) {
    vector<book_entry> out;
    sorted_ok = true;
    for (auto* n = s.head->next.load(); n; n = n->next.load()) {
        int q = n->data->cnt.load();
        if (q <= 0) continue;
        if (!out.empty()) {
            int prev = out.back().price;
            if (descending ? n->data->price > prev : n->data->price < prev) sorted_ok = false;
        }
        out.push_back({n->data->id, n->data->price, q});
    }
    return out;
}

struct run_result {
    vector<log_event> events;
    vector<book_entry> final_buys, final_sells;
    bool buys_sorted = true, sells_sorted = true;
};

run_result run_scenario(const scenario& sc) {
    global_log.reset();
    timestamp.store(0);

    orderbook ob;
    atomic<bool> go{false};
    {
        vector<jthread> ts;
        for (int u = 0; u < (int)sc.per_user.size(); u++) {
            ts.emplace_back([&, u] {
                while (!go.load(memory_order_acquire)) {}
                for (const op& o : sc.per_user[u]) {
                    switch (o.kind) {
                        case op::buy:  ob.active_buy(o.id, o.price, o.qty); break;
                        case op::sell: ob.active_sell(o.id, o.price, o.qty); break;
                        case op::cancel:
                            if (sc.orders[o.id].is_sell) ob.cancel_sell_order(o.id);
                            else ob.cancel_buy_order(o.id);
                            break;
                    }
                }
            });
        }
        go.store(true, memory_order_release);
    }  // join every user thread before reading logs or the book

    run_result r;
    r.events = global_log.collect();
    r.final_buys = dump_book(ob.resting_buys, /*descending=*/true, r.buys_sorted);
    r.final_sells = dump_book(ob.resting_sells, /*descending=*/false, r.sells_sorted);
    return r;
}

// ---------------------------------------------------------------------------
// replay checker
// ---------------------------------------------------------------------------

string ev_str(const log_event& e) {
    char buf[128];
    switch (e.kind) {
        case event_kind::match:
            snprintf(buf, sizeof buf, "t=%d match active=%d resting=%d price=%d qty=%d",
                     e.time, e.active_id, e.resting_id, e.price, e.quantity);
            break;
        case event_kind::rest:
            snprintf(buf, sizeof buf, "t=%d rest id=%d price=%d qty=%d",
                     e.time, e.active_id, e.price, e.quantity);
            break;
        case event_kind::cancel_ok:
            snprintf(buf, sizeof buf, "t=%d cancel_ok id=%d", e.time, e.active_id);
            break;
        case event_kind::cancel_fail:
            snprintf(buf, sizeof buf, "t=%d cancel_fail id=%d", e.time, e.active_id);
            break;
    }
    return buf;
}

#define CHECK(cond, ...)                        \
    do {                                        \
        if (!(cond)) {                          \
            printf("    CHECK FAILED: ");       \
            printf(__VA_ARGS__);                \
            printf("\n");                       \
            return false;                       \
        }                                       \
    } while (0)

void erase_one(multiset<int>& s, int v) {
    auto it = s.find(v);
    assert(it != s.end());
    s.erase(it);
}

bool replay_check(const scenario& sc, const run_result& r) {
    const auto& ev = r.events;
    int n = (int)sc.orders.size();

    vector<int> active_filled(n, 0);   // quantity this order matched while active
    vector<int> rested_qty(n, -1);     // -1: never came to rest
    vector<int> live_qty(n, 0);        // current resting remainder
    vector<int> cancel_oks(n, 0);
    multiset<int> live_buy_prices, live_sell_prices;

    for (size_t i = 0; i < ev.size(); i++) {
        const log_event& e = ev[i];
        if (i > 0)
            CHECK(ev[i - 1].time < e.time, "event #%zu: duplicate timestamp: %s", i, ev_str(e).c_str());

        switch (e.kind) {
        case event_kind::match: {
            int a = e.active_id, rid = e.resting_id;
            CHECK(a >= 0 && a < n && rid >= 0 && rid < n && a != rid,
                  "event #%zu: bad ids: %s", i, ev_str(e).c_str());
            const order_info& sa = sc.orders[a];
            const order_info& sr = sc.orders[rid];

            CHECK(sa.is_sell != sr.is_sell, "event #%zu: match within one side: %s", i, ev_str(e).c_str());
            CHECK(rested_qty[rid] >= 0 && live_qty[rid] > 0,
                  "event #%zu: resting order not live (live=%d): %s", i, live_qty[rid], ev_str(e).c_str());
            CHECK(e.price == sr.price,
                  "event #%zu: trade price != resting price %d: %s", i, sr.price, ev_str(e).c_str());
            // active order's limit must cross the trade price
            if (sr.is_sell)
                CHECK(sa.price >= e.price, "event #%zu: buy limit %d below trade price: %s",
                      i, sa.price, ev_str(e).c_str());
            else
                CHECK(sa.price <= e.price, "event #%zu: sell limit %d above trade price: %s",
                      i, sa.price, ev_str(e).c_str());

            int a_rem = sa.qty - active_filled[a];
            CHECK(a_rem > 0, "event #%zu: active order already fully filled: %s", i, ev_str(e).c_str());
            CHECK(e.quantity == min(a_rem, live_qty[rid]),
                  "event #%zu: qty != min(active rem %d, resting rem %d): %s",
                  i, a_rem, live_qty[rid], ev_str(e).c_str());

            // best-price rule: the resting order hit must carry the best live price
            int best = sr.is_sell ? *live_sell_prices.begin() : *live_buy_prices.rbegin();
            CHECK(e.price == best,
                  "event #%zu: matched at %d but best resting %s is %d: %s",
                  i, e.price, sr.is_sell ? "sell" : "buy", best, ev_str(e).c_str());

            active_filled[a] += e.quantity;
            live_qty[rid] -= e.quantity;
            if (live_qty[rid] == 0)
                erase_one(sr.is_sell ? live_sell_prices : live_buy_prices, sr.price);
            break;
        }
        case event_kind::rest: {
            int id = e.active_id;
            CHECK(id >= 0 && id < n, "event #%zu: bad id: %s", i, ev_str(e).c_str());
            const order_info& s = sc.orders[id];

            CHECK(rested_qty[id] == -1, "event #%zu: order rested twice: %s", i, ev_str(e).c_str());
            CHECK(e.price == s.price, "event #%zu: rest price != limit %d: %s", i, s.price, ev_str(e).c_str());
            CHECK(e.quantity > 0 && e.quantity == s.qty - active_filled[id],
                  "event #%zu: rest qty != unfilled remainder %d: %s",
                  i, s.qty - active_filled[id], ev_str(e).c_str());

            // nothing crossable may be live on the other side, else it should have matched
            if (s.is_sell) {
                if (!live_buy_prices.empty())
                    CHECK(*live_buy_prices.rbegin() < e.price,
                          "event #%zu: sell rested while buy at %d is live: %s",
                          i, *live_buy_prices.rbegin(), ev_str(e).c_str());
            } else {
                if (!live_sell_prices.empty())
                    CHECK(*live_sell_prices.begin() > e.price,
                          "event #%zu: buy rested while sell at %d is live: %s",
                          i, *live_sell_prices.begin(), ev_str(e).c_str());
            }

            rested_qty[id] = live_qty[id] = e.quantity;
            (s.is_sell ? live_sell_prices : live_buy_prices).insert(e.price);
            break;
        }
        case event_kind::cancel_ok: {
            int id = e.active_id;
            CHECK(id >= 0 && id < n, "event #%zu: bad id: %s", i, ev_str(e).c_str());
            CHECK(cancel_oks[id] == 0, "event #%zu: order cancelled twice: %s", i, ev_str(e).c_str());
            CHECK(rested_qty[id] >= 0 && live_qty[id] > 0,
                  "event #%zu: successful cancel of non-live order: %s", i, ev_str(e).c_str());
            erase_one(sc.orders[id].is_sell ? live_sell_prices : live_buy_prices, sc.orders[id].price);
            live_qty[id] = 0;
            cancel_oks[id]++;
            break;
        }
        case event_kind::cancel_fail: {
            int id = e.active_id;
            CHECK(id >= 0 && id < n, "event #%zu: bad id: %s", i, ev_str(e).c_str());
            CHECK(!(rested_qty[id] >= 0 && live_qty[id] > 0),
                  "event #%zu: cancel failed but order is live with qty %d: %s",
                  i, live_qty[id], ev_str(e).c_str());
            break;
        }
        }
    }

    // per-user program order: each user's events, in timestamp order, must
    // replay exactly their op sequence (ops are processed one at a time).
    int n_users = (int)sc.per_user.size();
    vector<vector<int>> by_user(n_users);
    for (size_t i = 0; i < ev.size(); i++)
        by_user[sc.orders[ev[i].active_id].user].push_back((int)i);

    for (int u = 0; u < n_users; u++) {
        const auto& evs = by_user[u];
        size_t k = 0;
        for (const op& o : sc.per_user[u]) {
            if (o.kind == op::cancel) {
                CHECK(k < evs.size(), "user %d: no log for cancel of order %d", u, o.id);
                const log_event& e = ev[evs[k]];
                CHECK((e.kind == event_kind::cancel_ok || e.kind == event_kind::cancel_fail) &&
                      e.active_id == o.id,
                      "user %d: expected cancel(%d) log, got: %s", u, o.id, ev_str(e).c_str());
                k++;
            } else {
                int filled = 0;
                while (k < evs.size() && ev[evs[k]].kind == event_kind::match &&
                       ev[evs[k]].active_id == o.id) {
                    filled += ev[evs[k]].quantity;
                    k++;
                }
                CHECK(filled <= o.qty, "user %d: order %d overfilled %d/%d", u, o.id, filled, o.qty);
                if (filled < o.qty) {
                    CHECK(k < evs.size() && ev[evs[k]].kind == event_kind::rest &&
                          ev[evs[k]].active_id == o.id,
                          "user %d: order %d filled %d/%d but next log is not its rest%s%s",
                          u, o.id, filled, o.qty, k < evs.size() ? ": " : "",
                          k < evs.size() ? ev_str(ev[evs[k]]).c_str() : "");
                    k++;
                }
            }
        }
        CHECK(k == evs.size(), "user %d: %zu unexplained trailing events, first: %s",
              u, evs.size() - k, ev_str(ev[evs[k]]).c_str());
    }

    // final book: the real book must equal the model book, sorted, not crossed
    CHECK(r.buys_sorted, "final resting_buys not in decreasing price order");
    CHECK(r.sells_sorted, "final resting_sells not in increasing price order");

    auto model_side = [&](bool sell) {
        vector<book_entry> v;
        for (int id = 0; id < n; id++)
            if (live_qty[id] > 0 && sc.orders[id].is_sell == sell)
                v.push_back({id, sc.orders[id].price, live_qty[id]});
        sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.id < b.id; });
        return v;
    };
    auto by_id = [](vector<book_entry> v) {
        sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.id < b.id; });
        return v;
    };
    vector<book_entry> real_buys = by_id(r.final_buys), real_sells = by_id(r.final_sells);
    vector<book_entry> want_buys = model_side(false), want_sells = model_side(true);
    CHECK(real_buys == want_buys, "final buy book diverges from model (%zu real vs %zu model orders)",
          real_buys.size(), want_buys.size());
    CHECK(real_sells == want_sells, "final sell book diverges from model (%zu real vs %zu model orders)",
          real_sells.size(), want_sells.size());
    if (!live_buy_prices.empty() && !live_sell_prices.empty())
        CHECK(*live_buy_prices.rbegin() < *live_sell_prices.begin(),
              "final book is crossed: best buy %d >= best sell %d",
              *live_buy_prices.rbegin(), *live_sell_prices.begin());

    return true;
}

// ---------------------------------------------------------------------------
// deterministic tests: single user (fully sequential), so the exact event
// sequence is predictable. They validate the checker and the book semantics.
// ---------------------------------------------------------------------------

bool expect_events(const run_result& r, const vector<log_event>& want) {
    CHECK(r.events.size() == want.size(), "expected %zu events, got %zu", want.size(), r.events.size());
    for (size_t i = 0; i < want.size(); i++) {
        const log_event& g = r.events[i];
        const log_event& w = want[i];
        CHECK(g.kind == w.kind && g.active_id == w.active_id && g.resting_id == w.resting_id &&
              g.price == w.price && g.quantity == w.quantity,
              "event #%zu: expected {%s}, got {%s}", i, ev_str(w).c_str(), ev_str(g).c_str());
    }
    return true;
}

// e.time is ignored by expect_events; -1 keeps the aggregate init readable.
log_event M(int a, int r, int p, int q) { return {-1, event_kind::match, a, r, p, q}; }
log_event R(int id, int p, int q) { return {-1, event_kind::rest, id, -1, p, q}; }
log_event COK(int id) { return {-1, event_kind::cancel_ok, id}; }
log_event CFAIL(int id) { return {-1, event_kind::cancel_fail, id}; }

bool test_cancel_is_idempotent() {
    scenario sc;
    sc.per_user.resize(1);
    int b = add_order(sc, 0, false, 100, 10);   // rests
    add_cancel(sc, 0, b);                       // succeeds
    add_cancel(sc, 0, b);                       // must fail: already cancelled
    auto r = run_scenario(sc);
    return expect_events(r, {R(b, 100, 10), COK(b), CFAIL(b)}) && replay_check(sc, r);
}

bool test_full_match_at_resting_price() {
    scenario sc;
    sc.per_user.resize(1);
    int b = add_order(sc, 0, false, 100, 10);
    int s = add_order(sc, 0, true, 90, 10);     // crosses; trades at resting price 100
    auto r = run_scenario(sc);
    return expect_events(r, {R(b, 100, 10), M(s, b, 100, 10)}) && replay_check(sc, r);
}

bool test_partial_match_walks_best_price_first() {
    scenario sc;
    sc.per_user.resize(1);
    int s0 = add_order(sc, 0, true, 90, 5);
    int s1 = add_order(sc, 0, true, 95, 5);
    int bBig = add_order(sc, 0, false, 100, 8); // eats all of s0, 3 of s1
    add_cancel(sc, 0, s1);                      // cancels s1's remaining 2
    auto r = run_scenario(sc);
    return expect_events(r, {R(s0, 90, 5), R(s1, 95, 5),
                             M(bBig, s0, 90, 5), M(bBig, s1, 95, 3), COK(s1)}) &&
           replay_check(sc, r);
}

bool test_sell_matches_highest_buy_first() {
    scenario sc;
    sc.per_user.resize(1);
    int b0 = add_order(sc, 0, false, 100, 5);
    int b1 = add_order(sc, 0, false, 101, 5);
    int s = add_order(sc, 0, true, 99, 10);     // hits 101 first, then 100
    auto r = run_scenario(sc);
    return expect_events(r, {R(b0, 100, 5), R(b1, 101, 5),
                             M(s, b1, 101, 5), M(s, b0, 100, 5)}) &&
           replay_check(sc, r);
}

bool test_active_leftover_rests() {
    scenario sc;
    sc.per_user.resize(1);
    int s0 = add_order(sc, 0, true, 95, 4);
    int b = add_order(sc, 0, false, 100, 10);   // fills 4, rests 6 at 100
    int c = add_order(sc, 0, true, 200, 3);     // does not cross, rests
    add_cancel(sc, 0, s0);                      // s0 fully consumed: cancel fails
    auto r = run_scenario(sc);
    return expect_events(r, {R(s0, 95, 4), M(b, s0, 95, 4), R(b, 100, 6),
                             R(c, 200, 3), CFAIL(s0)}) &&
           replay_check(sc, r);
}

// ---------------------------------------------------------------------------
// randomized concurrent stress: N users on N threads, each running its op
// stream sequentially; the replay checker validates the merged log.
// ---------------------------------------------------------------------------

scenario gen_scenario(uint32_t seed, int users, int ops_per_user,
                      int price_lo, int price_hi, int max_qty,
                      int cancel_pct, int dup_cancel_pct) {
    mt19937 rng(seed);
    uniform_int_distribution<int> pct(0, 99);
    uniform_int_distribution<int> price(price_lo, price_hi);
    uniform_int_distribution<int> qty(1, max_qty);

    scenario sc;
    sc.per_user.resize(users);
    vector<vector<int>> submitted(users), cancelled(users);

    for (int u = 0; u < users; u++) {
        for (int k = 0; k < ops_per_user; k++) {
            if (!submitted[u].empty() && pct(rng) < cancel_pct) {
                int target;
                if (!cancelled[u].empty() && pct(rng) < dup_cancel_pct) {
                    // re-cancel something already cancelled: exercises idempotency
                    target = cancelled[u][uniform_int_distribution<int>(0, (int)cancelled[u].size() - 1)(rng)];
                } else {
                    target = submitted[u][uniform_int_distribution<int>(0, (int)submitted[u].size() - 1)(rng)];
                    cancelled[u].push_back(target);
                }
                add_cancel(sc, u, target);
            } else {
                bool sell = rng() & 1;
                submitted[u].push_back(add_order(sc, u, sell, price(rng), qty(rng)));
            }
        }
    }
    return sc;
}

struct stress_config {
    const char* name;
    int users, ops, price_lo, price_hi, max_qty, cancel_pct, dup_cancel_pct, iters;
};

bool run_stress(const stress_config& c, uint32_t base_seed) {
    for (int i = 0; i < c.iters; i++) {
        uint32_t seed = base_seed + i;
        scenario sc = gen_scenario(seed, c.users, c.ops, c.price_lo, c.price_hi,
                                   c.max_qty, c.cancel_pct, c.dup_cancel_pct);
        run_result r = run_scenario(sc);
        if (!replay_check(sc, r)) {
            printf("    stress '%s' failed: seed=%u users=%d ops=%d (%zu events logged)\n",
                   c.name, seed, c.users, c.ops, r.events.size());
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 20;
    uint32_t base_seed = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 12345;

    struct { const char* name; bool (*fn)(); } tests[] = {
        {"cancel_is_idempotent", test_cancel_is_idempotent},
        {"full_match_at_resting_price", test_full_match_at_resting_price},
        {"partial_match_walks_best_price_first", test_partial_match_walks_best_price_first},
        {"sell_matches_highest_buy_first", test_sell_matches_highest_buy_first},
        {"active_leftover_rests", test_active_leftover_rests},
    };

    bool ok = true;
    for (auto& t : tests) {
        printf("running %s...\n", t.name);
        fflush(stdout);
        if (!t.fn()) { printf("  FAILED\n"); ok = false; }
    }

    stress_config configs[] = {
        // narrow price band: heavy contention on the same few price levels
        {"high_contention", 8, 400, 95, 105, 10, 20, 30, iters},
        // wide band: deep books, long list walks
        {"wide_band", 8, 400, 1, 500, 20, 10, 20, iters},
        // cancel heavy with many duplicate cancels: idempotency under load
        {"cancel_heavy", 6, 300, 50, 60, 8, 50, 50, iters},
        // two users, long streams: tight active-vs-active races
        {"two_users", 2, 1500, 90, 110, 5, 15, 25, iters},
    };
    for (auto& c : configs) {
        printf("running stress %s (%d iters)...\n", c.name, c.iters);
        fflush(stdout);
        if (!run_stress(c, base_seed)) ok = false;
    }

    printf(ok ? "all tests passed\n" : "TESTS FAILED\n");
    return ok ? 0 : 1;
}
