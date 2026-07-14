#pragma once
//
// Lock-free multi-producer / multi-consumer queue using split reference counts.
// Based on the design presented by Anthony Williams in
// "C++ Concurrency in Action" (chapter 7), reimplemented and commented.
//
// Key idea: every counted_node_ptr carries an *external* count that is
// incremented each time a thread takes a reference to the node through that
// pointer. When a pointer is swung away, the accumulated external count is
// folded into the node's *internal* count. A node is deleted only when the
// internal count reaches zero and both external counters that once pointed
// at it (head/tail and the previous node's next) have been released.
//
#include <atomic>
#include <memory>
#include <utility>

template <typename T>
class lock_free_queue {
private:
    struct node;

    struct counted_node_ptr {
        int   external_count;
        node* ptr;
    };

    struct node_counter {
        unsigned internal_count : 30;
        // A node can be referenced by at most two external counters:
        // head/tail, and the previous node's next pointer.
        unsigned external_counters : 2;
    };

    struct node {
        std::atomic<T*>               data;
        std::atomic<node_counter>     count;
        std::atomic<counted_node_ptr> next;

        node() {
            node_counter new_count;
            new_count.internal_count    = 0;
            new_count.external_counters = 2;
            count.store(new_count);

            data.store(nullptr);

            counted_node_ptr null_next;
            null_next.ptr            = nullptr;
            null_next.external_count = 0;
            next.store(null_next);
        }

        // Drop one internal reference; delete the node if it was the last.
        void release_ref() {
            node_counter old_counter = count.load(std::memory_order_relaxed);
            node_counter new_counter;
            do {
                new_counter = old_counter;
                --new_counter.internal_count;
            } while (!count.compare_exchange_strong(
                         old_counter, new_counter,
                         std::memory_order_acquire,
                         std::memory_order_relaxed));

            if (new_counter.internal_count == 0 &&
                new_counter.external_counters == 0) {
                delete this;
            }
        }
    };

    std::atomic<counted_node_ptr> head;
    std::atomic<counted_node_ptr> tail;

    // Acquire a reference to the node behind `counter` by bumping the
    // external count, looping until we install our increment on the
    // pointer value we actually read.
    static void increase_external_count(
        std::atomic<counted_node_ptr>& counter,
        counted_node_ptr&              old_counter) {
        counted_node_ptr new_counter;
        do {
            new_counter = old_counter;
            ++new_counter.external_count;
        } while (!counter.compare_exchange_strong(
                     old_counter, new_counter,
                     std::memory_order_acquire,
                     std::memory_order_relaxed));

        old_counter.external_count = new_counter.external_count;
    }

    // The pointer `old_node_ptr` has been swung away from its slot.
    // Merge its accumulated external count into the node's internal
    // counter and release one external-counter slot.
    static void free_external_counter(counted_node_ptr& old_node_ptr) {
        node* const ptr = old_node_ptr.ptr;
        // -2: one for this thread's own reference, one because the
        // pointer no longer exists.
        int const count_increase = old_node_ptr.external_count - 2;

        node_counter old_counter = ptr->count.load(std::memory_order_relaxed);
        node_counter new_counter;
        do {
            new_counter = old_counter;
            --new_counter.external_counters;
            new_counter.internal_count += count_increase;
        } while (!ptr->count.compare_exchange_strong(
                     old_counter, new_counter,
                     std::memory_order_acquire,
                     std::memory_order_relaxed));

        if (new_counter.internal_count == 0 &&
            new_counter.external_counters == 0) {
            delete ptr;
        }
    }

    // Help move tail forward to new_tail; whichever thread succeeds
    // releases the old external counter, the rest just drop their ref.
    void set_new_tail(counted_node_ptr&       old_tail,
                      counted_node_ptr const& new_tail) {
        node* const current_tail_ptr = old_tail.ptr;

        while (!tail.compare_exchange_weak(old_tail, new_tail) &&
               old_tail.ptr == current_tail_ptr) {
            // keep trying while tail still refers to the same node
        }

        if (old_tail.ptr == current_tail_ptr)
            free_external_counter(old_tail);   // we swung the tail
        else
            current_tail_ptr->release_ref();   // someone else did
    }

public:
    lock_free_queue() {
        counted_node_ptr initial;
        initial.external_count = 1;
        initial.ptr            = new node;  // dummy node
        head.store(initial);
        tail.store(initial);
    }

    lock_free_queue(const lock_free_queue&)            = delete;
    lock_free_queue& operator=(const lock_free_queue&) = delete;

    ~lock_free_queue() {
        while (pop()) {}
        delete head.load().ptr;
    }

    void push(T new_value) {
        std::unique_ptr<T> new_data(new T(std::move(new_value)));

        counted_node_ptr new_next;
        new_next.ptr            = new node;
        new_next.external_count = 1;

        counted_node_ptr old_tail = tail.load();

        for (;;) {
            increase_external_count(tail, old_tail);

            T* old_data = nullptr;
            // Try to claim the current tail node's data slot.
            if (old_tail.ptr->data.compare_exchange_strong(
                    old_data, new_data.get())) {
                // We own the slot. Link in our new dummy node, unless
                // a helping thread already linked one for us.
                counted_node_ptr old_next = {0, nullptr};
                if (!old_tail.ptr->next.compare_exchange_strong(
                        old_next, new_next)) {
                    // Another thread helped; discard our spare node
                    // and use theirs as the new tail.
                    delete new_next.ptr;
                    new_next = old_next;
                }
                set_new_tail(old_tail, new_next);
                new_data.release();
                break;
            } else {
                // Slot already claimed by another producer: help it
                // finish by linking our spare node as the next node.
                counted_node_ptr old_next = {0, nullptr};
                if (old_tail.ptr->next.compare_exchange_strong(
                        old_next, new_next)) {
                    old_next     = new_next;
                    new_next.ptr = new node;  // need a fresh spare
                }
                set_new_tail(old_tail, old_next);
            }
        }
    }

    std::unique_ptr<T> pop() {
        counted_node_ptr old_head = head.load(std::memory_order_relaxed);

        for (;;) {
            increase_external_count(head, old_head);
            node* const ptr = old_head.ptr;

            if (ptr == tail.load().ptr) {
                // Queue is empty (head == tail dummy node).
                ptr->release_ref();
                return std::unique_ptr<T>();
            }

            counted_node_ptr next = ptr->next.load();
            if (head.compare_exchange_strong(old_head, next)) {
                T* const res = ptr->data.exchange(nullptr);
                free_external_counter(old_head);
                return std::unique_ptr<T>(res);
            }

            ptr->release_ref();  // lost the race; retry
        }
    }
};
