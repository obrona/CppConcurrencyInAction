#include <atomic>
#include <functional>

// special stack for phase concurrency.
// either all threads call add
// or all threads iterate the stack from the head, cannot modify the stack but can modify data.
// or 1 thread cleans up the stack.
template <typename T, typename Compare = std::less<T>>
struct sorted_stack {
    struct node {
        T* data = nullptr;
        std::atomic<node*> next{nullptr};

        node() {}

        node(T* data): data(data) {}
    };

    Compare cmp;
    node* head = new node();

    ~sorted_stack() {
        node* curr = head;
        while (1) {
            node* next_node = curr->next.load();
            delete curr->data;
            delete curr;
            
            if (!next_node) break;
            curr = next_node;
        }
    }

    void add(const T& val) {
        node* new_node = new node(new T(val));
        node* curr = head;

        while (1) {
            node* next_node = curr->next.load();

            // Advance past every node that compares strictly less than val,
            // so val lands before the first node that is >= val.
            if (next_node && cmp(*next_node->data, val)) {
                curr = next_node;
                continue;
            }

            // Splice new_node between curr and next_node.
            new_node->next.store(next_node);
            if (curr->next.compare_exchange_weak(next_node, new_node)) {
                return;
            }
            // CAS failed: another thread mutated curr->next. next_node now
            // holds the fresh value, so re-evaluate from the same curr.
        }
    }

    // only 1 thread calls this.
    void clean(function<bool(T)> pred) {
        node* curr = head;
        while (1) {
            node* next_node = curr->next.load();
            if (pred(*head->data)) {
                curr->next.store(next_node->next.load());
                delete next_node->data;
                delete next_node;
            } else {
                curr = next_node;
            }
        }
    }
};