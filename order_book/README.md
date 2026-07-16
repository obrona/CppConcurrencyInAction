concurrent order book implementation.

only match active orders and resting orders.

sorted_stack is the data structure we use to store resting orders.

users cancel their order. 
users cannot cancel other people order, this invariant will be maintained for test cases.
but users can submit multiple cancel for the same order, so idempotency is required.

each user is allocated 1 thread.
each user orders are processed sequentially, only process the next order when the prev order is fulfilled
i.e fully consumed, becomes resting, cancelled (un)succesfully.

but between users, no ordering is needed.

always match active orders with the best resting orders first.
i.e for active buy, match with resting sell with the lowest selling price.

## testing

log -> sort -> replay.

every fulfilled event (match, order coming to rest, cancel ok/fail) is logged
with a timestamp from the global atomic counter. each thread logs into its own
buffer (event_log.cpp), so logging does not reduce the concurrency level.

after all user threads are joined, the buffers are merged and sorted by
timestamp. this is a valid linearization: within a bridge phase a resting
order's cnt only decreases, and every event takes its timestamp before
publishing its effect, so an order observed empty was already empty at every
later timestamp.

the sorted log is replayed through a single-threaded model book
(orderbook_test.cpp) that checks the rules above at every step: matches hit
only live opposite-side orders at the resting price within the active limit,
always at the best live price, with qty == min(active remaining, resting
remaining); rests carry exactly the unfilled remainder and only when nothing
crossable is live; cancels succeed at most once and never fail while the
target is live; each user's events replay their ops in program order; and the
final model book equals the real book, sorted and uncrossed.

run:

    g++ -std=c++20 -O2 -pthread orderbook_test.cpp -o orderbook_test
    ./orderbook_test [stress-iters] [base-seed]     # defaults: 20 12345

also passes under -fsanitize=thread and -fsanitize=address
(tsan on kernels 6.5+ needs: setarch $(uname -m) -R ./orderbook_test).