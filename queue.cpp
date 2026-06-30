#include <memory>
#include <print>

template <typename T>
struct queue {
    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };

    std::unique_ptr<node> head;
    node *tail;

    queue(): head(new node()), tail(head.get()) {}

    std::shared_ptr<T> try_pop() {
        if (head.get() == tail) {
            return std::shared_ptr<T>();
        }

        std::shared_ptr<T> data = std::move(head->data);
        head = std::move(head->next);
        return data;
    }

    void push(T val) {
        tail->data = std::make_shared<T>(val);
        auto new_tail = std::make_unique<node>();
        auto raw = new_tail.get();
        tail->next = std::move(new_tail);
        tail = raw;
    }
};

int main() {
    queue<int> q;
    for (int i = 0; i < 10; i++) {
        q.push(i);
    }

    for (int i = 0; i < 5; i++) {
        std::println("{}", *q.try_pop().get());
    }
}