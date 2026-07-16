#pragma once
#include <atomic>
#include <functional>

// we can have 1 pahse where threads can read the nodes and delete nodes.
// 1 phase where threads add nodes.
// 1 phase where 1 thread does garbage collection.
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
    std::atomic<node*> to_be_deleted{nullptr};

    ~sorted_stack() {
        node* curr = head;
        while (1) {
            node* next_node = curr->next.load();
            delete curr->data;
            delete curr;
            
            if (!next_node) break;
            curr = next_node;
        }

        free_deleted_nodes();
    }

    void add(const T& val) {
        node* new_node = new node(new T(val));
        node* curr = head;

        while (1) {
            node* next_node = curr->next.load();

            start:
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
            } else {
                goto start;
            }
            // CAS failed: another thread mutated curr->next.
            // next_node and curr->next holds the new value, so just repeat the process.
        }
    }

    // accepts a function that may mutate data.
    // and returns a boolean, whether we can delete this node or not.
    // if multiple threads apply the function to be same node, then user must ensure
    // only 1 thread returns true, otherwise we have double free.
    // function is most likely to match active and resting orders in a cas loop.
    // the thread that successfully makes the order count to 0 is the one that returns true.
    //
    // this function is also used to cancel orders.
    // apply_and_maybe_delete tries to set the cnt to 0 in a cas loop.
    // the winning thread (only 1) deletes the node.
    //
    // the loading of the new next pointer may not be seen by other threads yet, but that is okay
    // as apply_and_maybe_delete will correctly handle lazily deleted nodes (by checking T* data in node).
    // and the iteration is still preserved, threads will still be able to access the later nodes, as we
    // do not do garbage collection in this phase and the next pointer is still preserved.
    void read_and_delete(std::function<bool(T&)> apply_and_maybe_delete) {
        node* curr = head;
        while (node* next_node = curr->next.load()) {
            bool can_delete = apply_and_maybe_delete(*next_node->data);
            if (can_delete) {
                curr->next.store(next_node->next.load());
                add_node_to_delete(next_node);
            } else {
                curr = next_node;
            }
        }
    } 

    // only 1 thread calls this.
    void clean(std::function<bool(const T&)> pred) {
        node* curr = head;
        while (1) {
            node* next_node = curr->next.load();
            if (!next_node) return;

            if (pred(*next_node->data)) {
                curr->next.store(next_node->next.load());
                delete next_node->data;
                delete next_node;
            } else {
                curr = next_node;
            }
        }
    }

    void add_node_to_delete(node* n) {
        node* next_node = to_be_deleted.load();
        while (1) {
            n->next.store(next_node);
            if (to_be_deleted.compare_exchange_strong(next_node, n)) break;
        }
    }

    // only 1 thread call this at the end for the phase.
    void free_deleted_nodes() {
        node* curr = to_be_deleted.load();
        while (curr) {
            node* next = curr->next.load();
            delete curr->data;
            delete curr;
            curr = next;
        }
    }
};