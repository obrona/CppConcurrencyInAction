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