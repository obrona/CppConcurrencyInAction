#include <memory>
#include <atomic>

template <typename T>
struct lock_free_stack {
    struct node {
        std::shared_ptr<T> data;
        node *next = nullptr;

        node() {}

        node(const T& data): data(std::make_shared<T>(data)) {}
    };

    std::atomic<node*> head;
    
    std::atomic<int> threads_in_pop;
    std::atomic<node*> to_be_deleted;

    void push(const T& data) {
        node *new_node = new node(data);
        new_node->next = head.load();
        while (!head.compare_exchange_weak(new_node->next, new_node));
    }

    std::shared_ptr<T> pop() {
        ++threads_in_pop;
        node *old_head = head.load();
        while (old_head && head.compare_exchange_weak(old_head, old_head->next));
        
        std::shared_ptr<T> res;
        if (old_head) {
            res.swap(old_head->data);
        }
        try_reclaim(old_head);
        return res;
    }

    void delete_nodes(node* nodes) {
        while (nodes != nullptr) {
            auto next = nodes->next;
            delete nodes;
            nodes = next;
        }
    }

    void try_reclaim(node* old_head) {
        if (threads_in_pop == 1) {
            node *nodes_to_delete = to_be_deleted.exchange(nullptr);
            if (--threads_in_pop == 0) {
                delete_nodes(nodes_to_delete);
            } else {
                chain_pending_nodes(nodes_to_delete);
            }
            delete old_head;
        } else {
            chain_pending_nodes(old_head);
            --threads_in_pop;
        }
    }

    void chain_pending_nodes(node* n) {
        if (n == nullptr) return;
        node *last = n;
        while (last->next != nullptr) {
            last = last->next;
        }
        chain_pending_nodes(n, last);
    }

    void chain_pending_nodes(node *first, node *last) {
        last->next = to_be_deleted.load();
        while (!to_be_deleted.compare_exchange_weak(last->next, first));
    }
};