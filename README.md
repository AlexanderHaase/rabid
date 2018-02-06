# About #

Rabid: Concurrency, more efficiently.

Rabid provides an API for non-blocking concurrency(not to be confused with non-blocking IO libraries like libuv). It is a more performant alternative to traditional mutex/thread concurrency modeling. In rabid, tasks are asynchrounously dispatched to threads for execution. Exclusive access is acheived through thread-specific ownership.
