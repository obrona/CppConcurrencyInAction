#pragma once
#include <mutex>
#include <functional>
#include <array>
#include <condition_variable>

// only 2 ways go left or go right.
struct single_lane_bridge {
    private:
    int limit = 10;
    std::mutex mut;
    std::condition_variable cond;
    std::array<int, 2> cnts = {0, 0};

    public:
    void enter(bool side) {
        std::unique_lock lk{mut};
        cond.wait(lk, [this, side] () { return cnts[!side] == 0 && cnts[side] < limit; });
        cnts[side]++;
    }

    void leave(bool side, std::function<void()> cleanup = [] {}) {
        std::unique_lock lk{mut};
        cnts[side]--;
        if (cnts[side] == 0) {
            cleanup();
            cond.notify_all();
        }
    }
};

struct lock_bridge {
    single_lane_bridge& slb;
    int side;
    std::function<void()> cleanup;

    lock_bridge(single_lane_bridge& slb, int side, std::function<void()> cleanup = [] {}): slb(slb), side(side), cleanup(cleanup) {}

    ~lock_bridge() {
        slb.leave(side, cleanup);
    }
};