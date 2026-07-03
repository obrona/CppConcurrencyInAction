#include <memory>

template <typename T>
struct stack {
    struct node;
    
    struct counted_node_ptr {
        int external_count = 0;
        node *ptr = nullptr;
    };

    struct node {
        std::shared_ptr<T> data;
        std::atomic<int> internal_count;
        counted_node_ptr next;

        node(const T& data_): data(std::make_shared<T>(data_)), internal_count{0} {}
    };

    std::atomic<counted_node_ptr> head{};

    void push(const T& data_) {
        counted_node_ptr new_ptr;
        node *new_node = new node(data_);
        new_ptr.external_count = 1;
        new_ptr.ptr = new_node;
        new_ptr.ptr->next = head.load();
        while (!head.compare_exchange_weak(new_ptr.ptr->next, new_ptr));
    }

    void increase_head_count(counted_node_ptr& ptr) {
        counted_node_ptr new_ptr;
        do {
            new_ptr = ptr;
            new_ptr.external_count++;
        } while (!head.compare_exchange_strong(ptr, new_ptr));
        ptr.external_count = new_ptr.external_count;
    }


    std::shared_ptr<T> pop() {
        counted_node_ptr old_ptr;
        for (;;) {
            old_ptr = head.load();
            increase_head_count(old_ptr);
            if (!old_ptr.ptr) {
                // if ptr is null it is the dummy node at the back, no need to subtract the count.
                return std::shared_ptr<T>(); 
            } 
            
            else if (head.compare_exchange_strong(old_ptr, ptr->next)) {
                std::shared_ptr<T> res;
                res.swap(old_ptr.ptr->data);
                int cnt = old_ptr.external_count - 2;
                if (old_ptr.ptr->internal_count.fetch_add(cnt) == -cnt) {
                    delete old_ptr;
                }
                return res;
            } 
            
            else if (old_ptr.ptr->internal_count.fetch_sub(1) == 1) {
                delete old_ptr;
            }
        }
    }

};