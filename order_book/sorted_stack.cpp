#include <atomic>
#include <functional>

// special queue for phased concurrency.
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
        node* curr = head;
        node* new_node = new node(new T(val));

        while (1) {
            node* next_node = curr->next.load();
            
            start:
            if (next_node && cmp(*next_node->data, val)) {
                curr = next;
                continue;
            }

            while (1) {
                new_node->next.store(next_node);
                if (curr->next.compare_exchange_strong(next_node, new_node)) {
                    break;
                } else if (cmp(*next_node->data, val)) {
                    curr = next_node;
                    goto start;
                }
            }
        }
    }


};