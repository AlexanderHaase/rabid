# About #

Rabid provides an efficient fine-grain concurrency model for throughput oriented computing. It employs a novel hybrid actor / m:n threading model to amortize synchronization overhead through batch processing. In essence, rabid exposes a programming model to saturate N hardware threads with minimal concurrancy/framework overhead.

The goal of Rabid is to provide efficient, throughput oriented framework, with overhead on the order of a virtual function call, regardless of contention. To do so, it prioritizes efficient throughput over immediate evaluation, allowing OS threads to avoid OS and hardware contention.

# Background #

Kernel facilitated synchronization introduces several inefficiencies for throughput computing. Because kernel based synchronization may suspend OS thread execution, it is difficult for applications to choose an optimal number of OS threads for available hardware resources. Without thread suspention, the natural choice would be one OS thread per hardware thread, corresponding to the best cache locality and thus highest throughput(without considering effects of hardware m:n threading/hyperthreading). However, considering os thread suspension, it offers poor utilization contention, as one to all-but-one hardware threads may go unutilized.

As an alternative, introducing additional OS threads offers more opportunities to utilize hardware threads: By over-subscribing the hardware threads, should some OS threads become suspend, ideally other candidates abound. However, introducing additional OS threads is not without side effects and pitfalls: As contention reduces, over-subscription of hardware threads reduces cache locality and overall throughput since hardware threads become increasingly time-shared. At the same time, increased time-sharing increases the likelihood of contention in non-trivial synchronization patterns: if a thread executes an algorithm acquiring multiple locks, it is possible for that thread to be suspended while holding one or more locks, which can cascade under the effects of increasing time sharing, artificially resource-constrainging the system. In addition to contention, synchronous IO wait can cause similar gaps in hardware utilization. Traditionally the remedy is to handle IO carefully, avoid synchronization, and attempt to choose a number of OS threads that strikes the optimal balance between over-saturation, and under-utilization.

The actor model presents a share-nothing alternative based on asynchronous message passing. Rather than explicit synchronization, the actor model expresses concurrency as set of actors that cooperate via asynchrounous message passing, which may be implemented via lock-free operations. The efficency of actors depends upon the partitionability of the algorithm in question: ideally the set of actors with messages to process always equals or exceeds the number of hardware threads. Partitioning can be thought of in terms of parallelization and pipelining. Under parallelization, the same operation is applied to different data in concurrently. Under pipelining, sequential operations on data are divided to occur in parallel, such that one element may execute operation op-0 on data d-2 while another element performs operation op-1 on d-1 that already underwent op-0. Ideally an algorithm can be coerced to exhibit sufficient nested combinations of parallelism and pipelining to saturate hardware threads.

As a general strategy, batching execution amortizes synchronization overhead over several operations. By grouping n operations following the same synchronization path, batching reduces the per-operation overhead by a factor of up to 1/n. Efficiency gains from batching depend upon the cost of an operation relative to synchronization and the likelihood of contention when attempting synchronization: When the execution time of the combined operations is much less than that of synchronization, efficiency increases almost as a factor of n, since the probability of contention is approximately constant. However, when n increases such that execution time of combined operations rivals or exceeds execution time of synchronization, batching increasingly influences the likelihood of contention as it extends the amount of time a synchronized resource is held. In the first domain, an algorithm's throughput is constrained by the likelihood and cost of contention to first approximation. In the second, likelihood of contention must be approximated as a function of n rather than a constant, reducing efficiency gains below linear. Additionally, batching introduces additional organizing and dispatching mechanics that may not be without cost.

Synchronization overhead tends to exhibit a step function since the contended path usually neccessitates a trip through the OS scheduler and associated context switch overhead. For common constructs such as futexes/critical sections the desparity steps from nanoseconds on the fast path and to microseconds(or even milliseconds!) on the slow path, spanning several orders of magnitude! The descrepancy in overhead deminishes the effectiveness of parallelizing fine grain operations since the the overhead of a single contended operation can rival tens of thousands uncontended operations.

# Design #

Rabid is designed for efficient throughput-oriented computing. At it's core is a low-overhead actor model leveraging a lock-free message passing algorithm. Rabid build familiar execution and synchronization patterns on top of the model for easy adoption. In all aspects rabid prioritizes lock-free and constant time operation to provide a reliable foundation for performant applications. At the same time, rabid emphasizes end-user simplicity: programmers familiar with POSIX and WINDOWs APIs should find it simple and familiar to produce correct code. Finally, rabid aims to be widely compatible with platforms and other frameworks: Rabid provides hooks to mix-in IO and sleep functionality in an efficient manner taylored to an application's needs.

Rabid exists in two forms: an abstract reusable design and a reference implementation. Design discussion will focus on the former, with occasional considerations of the later as relevant to design decisions.

## Core Design: Actors and Message Passing ##

Rabid builds upon an actor model in order to deliver efficient throughput-oriented computing. The model is taylored specifically to that end, unifying the expression of several actor-model concepts. Rabid expresses one OS thread per hardware thread, and attempts to keep each OS thread satruated with messages. Rabid refactors actor responsibilities for efficiency. In Rabid, messages are directly executable: each message contains a function to run rather than require the recipient to maintain facilities for looking up, registering, and dispatching message handling. The intent is two-fold: first, to only require a single indirect/virtual call during message processing, and second to elimintate data structure complexity and overhead. In that sense, each message is an actor with a single handler to receive itself.

On the other hand, rabid operates with implicit per-OS-thread resource ownership. From that perspective, each OS thread is an actor with ownership of it's own data structures. In rabid, messages move from OS thread to OS thread to interact with thread-owned resources. At the same time, messages may acquire and move shared memory resources with them. Rather than an exclusive local-access model, rabid expresses a per-thread permissions model of shared resources. That model translates to message-exclusive access, providing implicit synchronization of shared resources. However, acheiving that model requires aditional abstractions on top of the base actor/message passing model. Two primative operations are exposed at this level are: are creating new messages and moving messages to a specific OS thread.

### Message Passing: Lock-free Lists with Pointer Tagging ###

Lock-free message passing provides the core strategy for avoiding contention. The message passing topology is built upon connections linking pairs of OS threads: Each connection has a pair of lock-free lists that function as inbound/outbound buffers between the two threads. The lists are cache-line sized and cache-line aligned to avoid false-sharing. Exchanging messages relies on MESI mechanics for relatively wait-free efficiency: Each list is primarily modified via atomic-compare-exchange by the sending OS thread, where the recieving OS thread occasionally interrupts MESI ownership via an unconditional atomic exchange to drain the list. The sending thread publishes each message individually to improve availability when the receiving thread looks for work. As a side-effect, hardware-efficency improves with queue depth, as batching amortizes the MESI cache transfer overhead over an increasing number of operations.

Arbitrary topologies may be used to interconnect OS threads: Direct messaging provides a simple solution where N^2 memory is acceptable for connecting N os threads. Where unacceptable, N*log(N) memory is possible via message forwarding across multiple OS threads using any number of mesh network topologies. NUMA considerations should be taken into account when considering both topology and algorithm implementation.

Pointer-tagging facilitates yeilding when work is not available. Prior to sleeping/yeilding, a thread must be able to reliably message all connected threads that it should be woken when work is available to avoid yeild-locking all threads. Rather than attempt to synchornize entire connections, pointer-tagging is used to indicate special message considerations within a lock-free list. Tagging allows the receving OS thread to place a message as a sentinel value. The sending thread recognizes the sentinel value by tag, and evaluates it, waking the receving thread. The strategy scales across any number of direct connections: The OS thread drains all inbound messages, publishing a "wakeup" sentinel message, and processing any pending messages prior to sleeping. Any connected thread that attempts to communicate will discover and evalutate the "wakeup" sentinel message.

Yeilding provides the primary compatibility point with other frameworks: Yeilding may be impliemented directly via an events/condition variables, or integrated into async/callback IO frameworks. Sentinel "wakeup" messages are only published when no work is expected, since evaluating them may induce hardware or OS overhead.

### Message Scheduling for Throughput ###

## Higher Level Abstractions ##

### Promises, Futures, and Continuations ###

WIP 2017-12-24: Rich programing environments provide higher level abstractions for synchronizing flow control. The core mechanics of creating and moving messages leave synchronization up to the author: thread-local, shared memory, and/or atomic values, etc. However, at the core, messages express an async operation which has a result, and requires some input. Chaining messages provides a natural context for asynchronous flow control. If messages are extended to support capturing results, promises, futures, and continuations become possible via type-specialized wrappers around generic messages.

### Capturing Results ###

Since messages are effectively erasure buffers, it's possible to leverage the erasure buffer space to both store the executable contents of the message and to capture the result of evaluating the message. The desired effect is for the erasure to capture the results of evaluation for reuse later:

```c++
template < typename Functor, typename Result, typename Param >
struct Capture : Erasure::Concept<Result,Param>, Functor
{
  /// Storage for the result, used with placement new.
  ///
  std::aligned_storage_t<sizeof(Result),alignof(Result)> storage;

  /// Construct the Capture
  ///
  Capture( Functor && functor )
  : Functor( std::move( functor ) ) 
  {}

  /// Evaluate, passing return value out of band.
  ///
  virtual void call( Param & param ) final
  {
    new ( &storage ) Result{ Functor::operator()( param ); };
    /* ...TODO: apply results to continuation... */
  }

  // Non-virtual destructor, we'll call this
  ~Capture() { reinterpret_cast<Result*>( &storage )->~Result(); }
};
```

With the ultimate goal of being able to express a continuation operation:

```c++
  // Arbitrary chains of invocation, optionally with location specification.
  //
  Address location[] = /* locations for continuation to be evalutated. */
  auto future0 = rabid::Evaluator::send( location[ 0 ], []() -> Result0 { /*...*/ } );
  auto future1 = future0.then( location[ 1 ], []( Result0 & ) -> Result1 { /*...*/ } );
  auto future2 = future1.then( location[ 2 ], []( Result1 & ) -> Result2 { /*...*/ } );
  auto future3 = future2.then( []( Result2 & ) -> Result3 { /*...*/ });

  // Copyable, thread-safe futures
  //
  auto future4 = 
*/

