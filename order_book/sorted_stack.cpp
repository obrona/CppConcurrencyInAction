#pragma once
#include <atomic>
#include <functional>
#include "timestamp.cpp"

// we can have 1 phase where threads can read the nodes and delete nodes.
// 1 phase where threads add nodes.
// 1 phase where 1 thread does garbage collection.
template <typename T, typename Compare = std::less<T>>
struct sorted_stack {
    struct node {
        T* data = nullptr;
        std::atomic<node*> next{nullptr};
        std::atomic<node*> next_delete{nullptr};

        node() {}

        node(T* data): data(data) {}
    };

    Compare cmp;
    node* head = new node();
    std::atomic<node*> to_be_deleted{nullptr};

    // the queue has no stale pointers, but this is up to user to ensure.
    // i.e user must call free_deleted_nodes with a consistent predicate.
    // otherwise we have stale pointers which results in double free.
    // precondition: no queued node is still reachable from head, i.e. the
    // caller ran free_deleted_nodes with a predicate that is true for every
    // queued node (cancels queue without unlinking, and racing unlinks can
    // resurrect queued nodes). otherwise the walk and free_queue both delete
    // the same node -> double free.
    ~sorted_stack() {
        node* curr = head;
        while (1) {
            node* next_node = curr->next.load();
            delete curr->data;
            delete curr;
            
            if (!next_node) break;
            curr = next_node;
        }

        free_queue();
    }

    // cb (callback) takes in the printing function.
    // we cannot print after the add() function finishes because then the timestamp can be wrong.
    // eg thread 1 adds first, but thread 2 prints first.
    void add(const T& val, std::function<void(int)> cb = [] (int time) {}) {
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
            int time = get_time();
            if (curr->next.compare_exchange_weak(next_node, new_node)) {
                cb(time);
                return;
            } else {
                goto start;
            }
            // CAS failed: another thread mutated curr->next.
            // next_node and curr->next holds the new value, so just repeat the process.
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
            n->next_delete.store(next_node);
            if (to_be_deleted.compare_exchange_strong(next_node, n)) break;
        }
    }

    // only 1 thread call this at the end for the phase.
    // now the list is weird with skip pointers.
    // but most importantly nodes with cnt > 0 all accessible from head.
    // but we can encounter some nodes with cnt == 0.
    // these nodes are in to_be_deleted, so unlink them here and let
    // free_queue delete them exactly once.
    void free_deleted_nodes(std::function<bool(const T&)> pred) {
        node* curr = head;
        while (node* next_node = curr->next.load()) {
            if (pred(*next_node->data)) {
                curr->next.store(next_node->next.load());
            } else {
                curr = next_node;
            }
        }

        free_queue();
    }

    // caller must ensure no queued node is still reachable from head
    // (true after the sweep in free_deleted_nodes, and at destruction
    // because every phase end runs free_deleted_nodes).
    void free_queue() {
        node* curr = to_be_deleted.exchange(nullptr);
        while (curr) {
            node* next = curr->next_delete.load();
            delete curr->data;
            delete curr;
            curr = next;
        }
    }
};