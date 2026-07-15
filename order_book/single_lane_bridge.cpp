#include <mutex>
#include <functional>
#include <array>
#include <condition_variable>

// only 2 ways go left or go right.
struct single_lane_bridge {
    private:
    std::mutex mut;
    std::condition_variable cond;
    std::array<int, 2> cnts = {0, 0};

    public:
    void enter(int side) {
        std::unique_lock lk{mut};
        cond.wait(lk, [this, side] () { return cnts[!side] == 0; });
        cnts[side]++;
    }

    void leave(int side, std::function<void()> cleanup) {
        std::unique_lock lk{mut};
        cnts[side]--;
        if (cnts[side] == 0) {
            cleanup();
            cond.notify_all();
        }
    }
};