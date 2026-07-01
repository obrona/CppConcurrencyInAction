#include <mutex>
#include <memory>

// to make things simple we have a dummy head which does not contain anything.
template <typename T>
struct list {
    struct node {
        std::mutex m;
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;

        node() {}
        
        node(const T& val): data(std::make_shared<T>(val)) {}
    };

    node head;

    void push_front(const T& val) {
        auto new_node = std::make_unique<node>(val);
        std::unique_lock lk(head.m);
        new_node->next = std::move(head.next);
        head.next = std::move(new_node);
    }

    // insert at elem before the 1st elem we encounter that satisfy the predicate.
    template <typename Pred>
    void insert_at_pred(const T& val, Pred pred) {
        std::unique_lock lk(head.m);
        node *curr = &head;
        auto new_node = std::make_unique<node>(val);
        while (1) {
            if (!curr->next) {
                curr->next = std::move(new_node);
                return;
            }

            node *next = curr->next.get();
            std::unique_lock lk2(next->m);
            if (pred(*next->data)) {
                new_node->next = std::move(curr->next);
                curr->next = std::move(new_node);
                return;
            }

            curr = next;
            lk = std::move(lk2);
        }
    }

    // removes the all elements satisfying some predicate.
    template <typename Pred>
    void remove_if(Pred pred) {
        std::unique_lock lk(head.m);
        node* curr = &head;
        while (node *next = curr->next.get()) {
            std::unique_lock lk2(next->m);
            if (pred(*next->data)) {
                // need this line so that we do not destroy next (and its mutex) early
                auto old_next = std::move(curr->next);
                curr->next = std::move(next->next);
                
                // release lk2 before old_next (which owns next's mutex) is
                // destroyed, otherwise ~unique_lock unlocks a freed mutex.
                lk2.unlock();
            } else {
                curr = next;
                lk = std::move(lk2);
            }
        }
    }

    // hand over hand locking.
    // to get to the next node, we must have the lock on the prev node, 
    // then acquire the lock on the next node while still owning the prev node mutex.
    // then release the lock on the prev node.
    template <typename Func>
    void for_each(Func fn) {
        std::unique_lock lk(head.m);
        node* curr = &head;
        while (node *next = curr->next.get()) {
            std::unique_lock lk2(next->m);
            lk.unlock();
            fn(*next->data);
            curr = next;
            lk = std::move(lk2);
        }
    }
};