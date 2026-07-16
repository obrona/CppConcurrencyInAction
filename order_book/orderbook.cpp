#include <atomic>
#include "single_lane_bridge.cpp"
#include "sorted_stack.cpp"

enum class order_type { buy, sell };

struct resting_order {
    const order_type type = order_type::buy;
    const int id = -1;
    const int price = -1;
    std::atomic<int> cnt{0};

    resting_order() {}

    resting_order(order_type type, int id, int price, int cnt): 
        type(type), 
        id(id), 
        price(price), 
        cnt{cnt} 
    {}
    
    resting_order(const resting_order& other): 
        type(other.type), 
        id(other.id), 
        price(price), 
        cnt{other.cnt.load()} 
    {}
};

// comparator is >=, because we want older orders to appear first.
// the time it was inserted is the 'time' of the order. 
struct CmpBuy {
    bool operator()(const resting_order& r1, const resting_order& r2) {
        return r1.price >= r2.price;
    }
};

struct CmpSell {
    bool operator()(const resting_order& r1, const resting_order& r2) {
        return r1.price <= r2.price;
    }
};

struct orderbook {
    single_lane_bridge slb;
    sorted_stack<resting_order, CmpBuy> resting_buys;
    sorted_stack<resting_order, CmpSell> resting_sells;

    // buy is 0, sell is 1
    bool type_to_side(order_type type) {
        return type == order_type::sell;
    }

    void cleanup() {
        resting_buys.free_deleted_nodes();
        resting_sells.free_deleted_nodes();
    }

    void cancel_order(order_type type, int id) {
        static auto fn = [this] (resting_order& r) {
            int cnt = r.cnt.load();
            while (1) {
                if (cnt == 0) return false;
                bool res = r.cnt.compare_exchange_strong(cnt, 0);
                if (res) return true;
            }
        };

        int side = type_to_side(type);
        lock_bridge lk(slb, side, [this] { this->cleanup(); });

        if (type == order_type::buy) {
            resting_buys.read_and_delete(fn);
        } else {
            resting_sells.read_and_delete(fn);
        }
    }

    void enter_active_order(order_type type, int id, int price, int cnt) {
        static auto fn = [this, type, price, &cnt] (resting_order& r) {
            bool valid = type == order_type::buy && price >= r.price || type == order_type::sell && price <= r.price;
            if (!valid) return false;

            int curr_cnt = r.cnt.load();
            while (1) {
                if (curr_cnt == 0) return false;
                int quantity = std::min(curr_cnt, cnt);
                if (r.cnt.compare_exchange_strong(curr_cnt, curr_cnt - quantity)) {
                    cnt -= quantity;
                    return curr_cnt == 0;
                }
            }
        };

        int side = type_to_side(type);
        lock_bridge lk(slb, side, [this] { this->cleanup(); });
        
        if (type == order_type::buy) {
            resting_sells.read_and_delete(fn);
        } else {
            resting_buys.read_and_delete(fn);
        }

        if (cnt > 0) {
            if (type == order_type::buy) {
                resting_buys.add(resting_order{type, id, price, cnt});
            } else {
                resting_sells.add(resting_order{type, id, price, cnt});
            }
        }
    }
};