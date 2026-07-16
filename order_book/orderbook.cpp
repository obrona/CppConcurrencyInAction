#include <atomic>
#include <algorithm>
#include "single_lane_bridge.cpp"
#include "sorted_stack.cpp"

enum class order_type { buy, sell };

std::atomic<int> timestamp{0};

int get_time() {
    return timestamp.fetch_add(1, std::memory_order_relaxed);
}

//TODO
void log_cancel(int time, int id, bool success) {}

void log_active_resting_match(int time, int active_id, int resting_id, int price, int quantity) {}

void log_add_resting_order(int time, int active_id, int price, int quantity) {}

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
        price(other.price), 
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
    
    sorted_stack<resting_order, CmpBuy> resting_buys;     // in decreasing order
    sorted_stack<resting_order, CmpSell> resting_sells;   // in increasing order
    
    std::function<bool(const resting_order&)> delete_pred = [] (const resting_order& r) { return r.cnt.load() == 0; };
    // buy is 0, sell is 1
    bool type_to_side(order_type type) {
        return type == order_type::sell;
    }

    // cancel buy/sell never relinks nodes, just set the cnt to 0.
    void cancel_buy_order(int id) {
        lock_bridge lk(slb, type_to_side(order_type::sell));

        auto curr = resting_buys.head;
        while (auto next_node = curr->next.load()) {
            resting_order& r = *next_node->data;
            int cnt = r.cnt.load();
            if (r.id != id || cnt == 0) {
                curr = next_node;
                continue;
            }

            while (cnt > 0) {
                int time = get_time();
                if (r.cnt.compare_exchange_strong(cnt, 0)) {
                    log_cancel(time, id, true);
                    return;
                }
            }

            break;
        }

        log_cancel(get_time(), id, false);
    }

    void cancel_sell_order(int id) {
        lock_bridge lk(slb, type_to_side(order_type::buy));

        auto curr = resting_sells.head;
        while (auto next_node = curr->next.load()) {
            resting_order& r = *next_node->data;
            int cnt = r.cnt.load();
            if (r.id != id || cnt == 0) {
                curr = next_node;
                continue;
            }

            while (cnt > 0) {
                int time = get_time();
                if (r.cnt.compare_exchange_strong(cnt, 0)) {
                    log_cancel(time, id, true);
                    return;
                }
            }

            break;
        }

        log_cancel(get_time(), id, false);
    }

    void active_buy(int id, int price, int cnt) {
        lock_bridge lk(slb, type_to_side(order_type::buy));

        auto curr = resting_sells.head;
        while (auto next_node = curr->next.load()) {
            resting_order& r = *next_node->data;
            if (r.price > price) break;
            
            int remaining = r.cnt.load();
            if (remaining == 0) {
                curr = next_node;
                continue;
            }

            while (remaining > 0) {
                int quantity = std::min(remaining, cnt);
                int time = get_time();
                if (!r.cnt.compare_exchange_strong(remaining, remaining - quantity)) continue;
                
                log_active_resting_match(time, id, r.id, r.price, quantity);
                cnt -= quantity;

                if (remaining - quantity == 0) {
                    curr->next.store(next_node->next.load());
                    resting_sells.add_node_to_delete(next_node);
                } else {
                    // active order cnt is 0.
                    return;   
                }
                break;
            }
        }

        if (cnt > 0) {
            resting_buys.add(resting_order{order_type::buy, id, price, cnt});
            log_add_resting_order(get_time(), id, price, cnt);
        }
       
    }

    void active_sell(int id, int price, int cnt) {
        lock_bridge lk(slb, type_to_side(order_type::sell));

        auto curr = resting_buys.head;
        while (auto next_node = curr->next.load()) {
            resting_order& r = *next_node->data;
            if (r.price < price) break;
            
            int remaining = r.cnt.load();
            if (remaining == 0) {
                curr = next_node;
                continue;
            }

            while (remaining > 0) {
                int quantity = std::min(remaining, cnt);
                int time = get_time();
                if (!r.cnt.compare_exchange_strong(remaining, remaining - quantity)) continue;
                
                log_active_resting_match(time, id, r.id, r.price, quantity);
                cnt -= quantity;

                if (remaining - quantity == 0) {
                    curr->next.store(next_node->next.load());
                    resting_buys.add_node_to_delete(next_node);
                } else {
                    // active order cnt is 0.
                    return;
                }
                break;
            }
        }

        if (cnt > 0) {
            resting_sells.add(resting_order{order_type::sell, id, price, cnt});
            log_add_resting_order(get_time(), id, price, cnt);
        }
        
    }
};