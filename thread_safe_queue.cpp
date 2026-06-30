#include <memory>
#include <print>
#include <mutex>
#include <condition_variable>

template <typename T>
struct queue {
    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };
    
    std::unique_ptr<node> head;
    node *tail;
    std::mutex head_mutex, tail_mutex;
    std::condition_variable data_cond;

    queue(): head(new node()), tail(head.get()) {}

    node* get_tail() {
        std::lock_guard lk(tail_mutex);
        return tail;
    }

    std::shared_ptr<T> try_pop() {
        std::lock_guard lk(head_mutex);
        if (head.get() == get_tail()) {
            return std::shared_ptr<T>();
        }
        auto data = head->data;
        head = std::move(head->next);
        return data;
    }

    std::shared_ptr<T> wait_and_pop() {
        std::unique_lock lk(head_mutex);
        data_cond.wait(lk, [this] () { return head.get() != get_tail(); });
        auto data = head->data;
        head = std::move(head->next);
        return data;
    }

    void push(T val) {
        auto new_data = std::make_shared<T>(val);
        auto new_tail = std::make_unique<node>();
        auto raw = new_tail.get();
        {
            std::lock_guard lk(tail_mutex);
            tail->data = std::move(new_data);
            tail->next = std::move(new_tail);
            tail = raw;
        }
        data_cond.notify_one();
    }
};