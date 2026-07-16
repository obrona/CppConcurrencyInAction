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

## data structures

**sorted_stack** (sorted_stack.cpp) — the book container. despite the name, a
singly-linked sorted list with a dummy head. insertion is lock-free: walk to
position, CAS `curr->next` to splice, retry on failure. each node also carries
a `next_delete` link threading an intrusive lock-free stack (`to_be_deleted`)
of nodes queued for reclamation; actual freeing runs single-threaded in
`free_deleted_nodes`/`free_queue`.

**single_lane_bridge** (single_lane_bridge.cpp) — mutex + condvar gate from
the classic single-lane-bridge problem. up to 10 threads of one side inside
concurrently, the two sides mutually exclusive. the last thread to leave a
side runs a cleanup callback under the mutex — that's where garbage
collection happens. `lock_bridge` is the RAII wrapper.

**orderbook** (orderbook.cpp) — two sorted_stacks: `resting_buys` sorted by
descending price, `resting_sells` ascending, so the head of each list is the
best price. comparators use `>=`/`<=` so equal-priced new orders insert after
existing ones, giving price-time (FIFO) priority. each resting_order's only
mutable field is `std::atomic<int> cnt` (remaining quantity); all consumption
and cancellation is CAS on it.

**event_log** (event_log.cpp) — per-thread log buffers (thread_local pointer
into a deque of vectors, so growth never invalidates other threads' pointers)
plus a global atomic timestamp counter. after all threads join, `collect()`
merges and sorts by timestamp.

## correctness

**phase separation makes each list either insert-only or consume-only.** the
bridge sides map to operation groups, not buy/sell users: side 0 is
active_buy + cancel_sell, side 1 is active_sell + cancel_buy. during a side-0
phase, resting_sells is only traversed and consumed (matched, cancelled,
unlinked) and resting_buys is only inserted into. no list ever sees concurrent
insertion and unlinking — the case that would need hazard pointers — so the
CAS-insert and CAS-decrement paths are each individually safe.

**quantity consumption linearizes on a single CAS.** matching reads
`remaining`, computes `quantity = min(remaining, cnt)`, and CASes
`remaining -> remaining - quantity`. if two threads race on the same resting
order, exactly one CAS wins; the loser re-reads and retries. quantity is never
double-consumed or lost.

**cancellation is idempotent for the same reason.** cancel CASes cnt from a
positive value to 0. a second cancel either finds cnt == 0 and skips the node,
or its CAS fails and the re-read sees 0 — either way it logs cancel_fail. a
cancel succeeds at most once.

**deletion is logical first, physical later.** cnt == 0 marks a node dead;
readers skip it. unlinking may leave dead nodes reachable through stale
pointers, which is fine because nothing is freed during a phase — dead nodes
go on the to_be_deleted stack. reclamation runs only in the bridge cleanup,
executed by the last thread leaving a phase while holding the bridge mutex,
when no other thread can be inside. free_deleted_nodes first sweeps
still-linked dead nodes out of the list, then free_queue frees each queued
node exactly once via the separate next_delete chain. no use-after-free,
no hazard pointers or RCU needed.

**best price and price-time priority come from sorted order.** traversal
starts at the head, so the first live (cnt > 0) node is the best price, and
the comparators keep same-price orders in arrival order.

caveat: within a phase, concurrent unlinks use plain stores on `next`, so a
racing unlink can transiently resurrect an already-skipped dead node. the
design tolerates this — dead nodes are skippable and the phase-end sweep
removes them — but it's why the sweep in free_deleted_nodes is necessary
rather than optional.

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