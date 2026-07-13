#include "lock_free_queue.cpp"
#include <thread>
#include <vector>
#include <cassert>
using namespace std;

void test_multi_producers_single_consumer() {
    lock_free_queue<int> q;
    int sum = 0;
    {
        vector<jthread> producers;
        jthread consumer;
        
        for (int i = 0; i < 10; i++) {
            producers.emplace_back([&q] () {
                for (int i = 0; i < 10; i++) q.push(1);
            });
        }

        consumer = jthread{[&q, &sum] () {
            for (int i = 0; i < 100; i++) sum += *q.pop().get();
        }};
    }

    assert(sum == 100);  
}

int main() {
    test_multi_producers_single_consumer();
}