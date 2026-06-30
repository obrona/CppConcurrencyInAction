#include <memory>
#include <print>
#include <mutex>

template <typename T>
struct queue {
    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };
    
    std::unique_ptr<node> head;
    node *tail;
    std::mutex head_mutex, tail_mutex;

    std::shared_ptr<T> try_pop() {
        std::lock_guard(head_mutex);
        {
            std::lock_guard(tail_mutex);
            if (head.get() == tail) return std::shared_ptr<T>();
        }
        auto data = head->data;
        head = std::move(head->next);
        return data;
    }

    void push(T val) {
        auto new_data = std::make_shared<T>(val);
        auto new_tail = std::make_unique<node>();
        auto raw = new_tail.get();
        std::lock_guard(tail_mutex);
        tail->data = std::move(new_data);
        tail->next = std::move(new_tail);
        tail = raw;
    }
};