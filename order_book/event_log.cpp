#pragma once
#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

enum class event_kind { match, rest, cancel_ok, cancel_fail };

struct log_event {
    int time = -1;
    event_kind kind = event_kind::match;
    int active_id = -1;   // the acting order: active order for match/rest, target order for cancels
    int resting_id = -1;  // match only: the resting order that was hit
    int price = -1;       // match: trade price (= resting price); rest: limit price
    int quantity = -1;    // match: traded quantity; rest: leftover quantity that went to the book
};

// Each thread appends to its own buffer, so logging never contends and does not
// reduce the concurrency level of the run. collect() merges and sorts by
// timestamp once every worker has been joined.
struct event_log {
    std::mutex mut;
    // deque: growing never moves existing buffers, so the thread_local pointers stay valid.
    std::deque<std::vector<log_event>> buffers;

    std::vector<log_event>& local_buffer() {
        thread_local std::vector<log_event>* buf = [this] {
            std::lock_guard lk(mut);
            buffers.emplace_back().reserve(1 << 12);
            return &buffers.back();
        }();
        return *buf;
    }

    void push(const log_event& e) { local_buffer().push_back(e); }

    // only call after every logging thread has been joined.
    std::vector<log_event> collect() {
        std::lock_guard lk(mut);
        std::vector<log_event> all;
        for (auto& b : buffers) all.insert(all.end(), b.begin(), b.end());
        std::sort(all.begin(), all.end(),
                  [](const log_event& a, const log_event& b) { return a.time < b.time; });
        return all;
    }

    // only call between runs, when no thread that ever logged is still alive
    // (their thread_local pointers dangle after this).
    void reset() {
        std::lock_guard lk(mut);
        buffers.clear();
    }
};

inline event_log global_log;
