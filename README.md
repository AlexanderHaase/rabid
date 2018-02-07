# Rabid: Concurrency, Efficiently #

Rabid minimizes contention and concurrency overhead to maximize processor time spent on actual work. It provides a straight-forward API for efficient non-blocking concurrency in user-space (not to be confused with non-blocking IO such as libuv and boost::asio). The API is desiged to decouple concurrent programming from thread scheduling, making it effortless to efficiently harness any number of hardware threads. Rather than using blocking OS APIs, threads cooperate via lock-free message passing, sleeping only when no work is available. Messages are simply asynchronous continuations executed by threads, and exclusion is modeled as thread ownership. The result is an lightweight header-only framework that readily enables more efficient concurrency than possible with blocking primatives.

## State ##

Rabid is currently in _alpha_: it mostly works, but there are ugly spots, unfinished spots, and several _would-be-nice-to-haves_. There is no published stable release--the entire project is currently a development branch.

## Core Concepts ##

Rabid is designed to maximize the throughput of concurrent algorithms on in-memory data. Blocking behavior is problematic unless there is absolutely no other work to be done: First, switching OS threads requires an expensive context switch and reduces cache locality. Second, it requires the programmer to make a trade-off between under-utilizing and over-subscribing hardware threads, with associated performance concerns. For maiximum throughtput, ideally each hardware thread would run exactly one software thread that never blocked. Rabid provides a concurrent programming model that makes that possible.

## Getting Started ##

Rabid is intended to be accessible to anyone familiar with threads and mutexes. Commonly threads are used to run asynchronous tasks, and mutexes used to safely coordinate threads. Rabid provides a pool of threads with a very efficent interconnect. Each thread in the pool is capabable of executing arbitrary user-defined tasks. Instead of mutexes, rabid uses thread-ownership to coordinate tasks: An exclusive resource should be accessed from one and only one thread. To use an exclusive resource, a task can either send itself to the owning thread, or dispatch a new task to that thread. Rabid replaces blocking mutex programming with non-blocking event/callback style programming.

```c++

#include <rabid/include/Executor.h>
#include <vector>

// Specify an executor with a direct thread interconnect and a set of dedicated
// threads.
//
using Exec = rabid::Executor<rabid::interconnect::Direct,rabid::ThreadModel>;

void simple_example( void )
{
  // Choose the number of threads to spawn.
  //
  const size_t threads = std::thread::hardware_concurrency();

  // Create exclusive resources to use within rabid.
  //
  struct Resource {
    size_t use_count = 0;
    /* some data here */
  };
  std::vector resources;
  resources.resize( threads );

  // Create and start the executor.
  //
  Exec executor{ threads };

  // Set up an event to wait for
  //
  rabid::detail::Join join{ threads };

  // Inject some tasks into the executor from outside
  //
  for( size_t thread = 0; thread < threads; ++thread )
  {
    exec.inject( thread, [&]()
      {
        resouces[ Exec::current() ].use_count += 1;
        join.notify();
      });
  }

  // Wait for tasks to complete
  //
  join.wait();
}
```

### Complex Synchronization ###

More complex algorithms are expressed as an extension of thread-exclusive resources. For example, locking algorithms involving multiple locks can be modeled as mutable thread-exclusive resources with pending tasks. Furture versions of rabid will include abstractions to help model more complex algorithms.

