#pragma once

#include "interconnect.h"
#include "future.h"

#include <thread>
#include <condition_variable>
#include <functional>

namespace rabid {

  namespace detail {

    /// Idle classes are used to define "idle" behavior for threads.
    ///
    /// When an Executor's worker has nothing to do, it yields control via
    /// idle.yield(). An idle implementation may do abitrary work inside yield.
    /// Idle::yield() may indicate that the worker should exit by returning
    /// false.
    ///
    /// Other workers (or even external threads) may wake up the current worker
    /// via idle.interrupt(). Idle::interrupt() should wake the next(including
    /// current) yield attempt.
    ///
    /// TODO: At some point it would be nice to have an indication of how many
    /// threads are running, to enable "Executor::wait()" style blocking until
    /// all threads are idle. However, this has so far proven non-trivial to
    /// generalize.
    ///
    namespace idle {

      /// Implementation of idle that uses stdlib to suspend execution.
      ///
      /// Yielding uses an atomic bool, a mutex, and a condition variable.
      /// The bool inidcates if yield should sleep, with two important side
      /// effects:
      ///
      ///   - If a thread is interupted prior to yielding, it will not sleep.
      ///   - Only one call to interrupt() will syscall to wake the thread per
      ///     call to yield(). (Additional calls quickly return)
      ///
      /// Otherwise, the mutex synchronizes behavior around sleeping and waking
      /// the condition variable.
      ///
      /// In addition to required yield() and interrupt() behavior, enabled() is
      /// also implemented to stop threads.
      ///
      class Wait {
       public:

        /// Yield control per API, sleeping in this implementation.
        ///
        /// @return boolean indication of if worker may continue to run.
        ///
        bool yield()
        {
          std::unique_lock<std::mutex> lock{ mutex };
          if( enabled )
          {
            if( armed.load( std::memory_order_relaxed ) )
            {
              condition.wait( lock );
            }
            armed.store( true, std::memory_order_relaxed );
          }
          return enabled;
        }

        /// Interrupt the current or next attempt to yield.
        ///
        /// armed indicates if the thread is allowed to sleep, debouncing
        /// mutex locking and condition notifying.
        ///
        void interrupt()
        {
          const auto signal = armed.exchange( false, std::memory_order_relaxed );
          if( signal )
          {
            std::unique_lock<std::mutex> lock{ mutex };
            lock.unlock();
            condition.notify_one();
          }
        }

        /// Set enabled state for the given worker, waking them if sleeping.
        ///
        /// @param true or false.
        ///
        void enable( bool value )
        {
          std::unique_lock<std::mutex> lock{ mutex };
          enabled = value;
          lock.unlock();
          condition.notify_one();
        }

       protected:
        std::atomic<bool> armed{true};      ///< Is thread allowed to sleep.
        std::mutex mutex;                   ///< Synchronizes condition var.
        std::condition_variable condition;  ///< Sleep/wakeup.
        bool enabled = true;                ///< Thread enabled status.
      };
    }

    /// DEPRECATED: use Counter instead.
    ///
    /// @deprecated
    ///
    class Join {
     public:
      Join( ssize_t events ) 
      : count( events )
      {}

      void reset( ssize_t events )
      {
        count.store( events, std::memory_order_relaxed );
      }

      void wait()
      {
        std::unique_lock<std::mutex> lock{ mutex };
        if( count.load( std::memory_order_relaxed ) )
        {
          condition.wait( lock );
        }
      }

      // Error: notify may over trigger wait() rebounding from zero.
      //
      void notify( ssize_t amount = 1 )
      {
        if( count.fetch_add( -amount, std::memory_order_relaxed ) == amount )
        {
          mutex.lock();
          mutex.unlock();
          condition.notify_all();
        }
      }

     protected:
      std::atomic<ssize_t> count;
      std::mutex mutex;
      std::condition_variable condition;
    };

    /// Blocks until no pending events remain.
    ///
    /// TODO: implement more central methods for tracking execution.
    ///   - Promise/Future: any( ... )/all( ... ) semantics.
    ///   - Exectutor: wait() semantics.
    ///
    class Counter {
     public:

      /// Create a counter with the given number of pending events.
      ///
      /// @param events Initial number of pending events.
      ///
      Counter( size_t events ) 
      : count( events )
      {}

      /// Reset the number of pending events to a specific number.
      ///
      /// @param events Initial number of pending events.
      ///
      void reset( size_t events )
      {
        count.store( events, std::memory_order_relaxed );
      }

      /// Suspend execution until there are no pending events.
      ///
      void wait()
      {
        std::unique_lock<std::mutex> lock{ mutex };
        if( count.load( std::memory_order_relaxed ) )
        {
          condition.wait( lock );
        }
      }

      /// Decrement then number of pending events.
      ///
      void decrement()
      {
        if( count.fetch_sub( 1, std::memory_order_relaxed ) == 1 )
        {
          mutex.lock();
          mutex.unlock();
          condition.notify_all();
        }
      }

      /// Increment the number of pending events.
      ///
      void increment()
      {
        count.fetch_add( 1, std::memory_order_relaxed );
      }

     protected:
      std::atomic<ssize_t> count;         ///< Number of pending events.
      std::mutex mutex;                   ///< Synchronizes condition var.
      std::condition_variable condition;  ///< Sleep/wakeup.
    };
  }

  /// Execution classes provide access to hardware parallelism.
  ///
  /// Execution implementations adapt rabid to various concurrency models.
  /// Executor provides a series of workers to be run in parallel, and the
  /// implementation provides an idle object defining idle behavior.
  ///
  /// Execution classes invoke functors by reference in a parallel context. An
  /// implementing class has the following behavior:
  ///   - It is constructed from an iterable range of functors to be run.
  ///     - Functors are invoked/passed BY REFERENCE.
  ///     - Each functor is invoked with an Idle object.
  ///   - It's destructor stops and joins all functors.
  ///
  /// TODO: Refine interface to be more modular, i.e. interacting gracefully
  /// with work queues and other concurrency patterns.
  ///
  namespace execution {

    /// Execution class that uses dedicated threads to provide parallelism.
    ///
    class ThreadModel {
     public:
      using Idle = detail::idle::Wait;

      /// Create a thread per worker.
      ///
      /// @tparam Iterator Type of iterator.
      /// @param begin first iterator to run.
      /// @param end last iterator to run.
      ///
      template < typename Iterator >
      ThreadModel( const Iterator & begin, const Iterator & end )
      {
        for( auto func = begin; func != end; ++func )
        {
          threads.emplace_back( std::make_unique<Thread>( *func ) );
        }
      }

      /// Stop all threads, joining them.
      ///
      ~ThreadModel()
      {
        for( auto & thread : threads )
        {
          thread->idle.enable( false );
          thread->thread.join();
        }
      }

     protected:
      // Helper class that wraps a thread handle and idle object
      //
      struct Thread {
        Idle idle;
        std::thread thread;

        // Spawn thread, running function by reference.
        //
        template < typename Function >
        Thread( Function && function )
        : thread( [&]{ function( idle ); } )
        {}
      };

      std::vector<std::unique_ptr<Thread>> threads;
    };
  }

  /// Core task executor class in rabid.
  ///
  /// Executor constructs workers with the specified interconnect, and runs the
  /// workers with the specified execution model. The workers remain available
  /// throughout the Executor object's life cycle, and are gracefully stopped
  /// when it is destroyed.
  ///
  /// Executor captures and dispatches functors as tasks, and provides a
  /// vocabulary of global/static utility methods:
  ///
  ///   - defer(target): Re-evaluate the current task in the specified thread.
  ///   - concurrency(): Query the number of threads in the range [0,n).
  ///   - current():  Query the index of the current thread.
  ///   - async(target, functor): Evaluate the functor in the specfied thread.
  ///
  /// These static methods are only valid within threads managed by Executor.
  ///
  /// Use 'Executor::inject(target,functor)' to insert functors into executor.
  /// 
  /// @tparam Interconnect Type of interconnect to use to connect workers.
  /// @tparam ExecutionModel Type of parallel execution to use for workers.
  ///
  template < typename Interconnect, typename ExecutionModel >
  class Executor {
   protected:

    // Forward declare dispatch type for future.
    //
    // TODO: Restructure not to require forward declaration?
    //
    struct TaskDispatch;

    // Type of future used for continuations in framework.
    //
    // TODO: Provide promises as a public type.
    //
    template <typename Function>
    using TaskFuture = Future<typename function_traits<Function>::return_type, TaskDispatch>;

   public:
    /// Create a new executor with the specified number of workers.
    ///
    /// @param size Number of workers to insantiate.
    /// 
    Executor( size_t size = std::thread::hardware_concurrency() )
    : /* active( size )
    , */ interconnect( size )
    , workers( make_workers( interconnect, size, *this ) )
    , execution( workers.begin(), workers.end() )
    {}

    /// Query the number of workers provided by the Executor.
    ///
    size_t size() const { return workers.size(); }

    /// Asynchronously evaluate a functor in the framework.
    ///
    /// Functors within the framework may use Executor's rich vocabulary of
    /// static methods.
    ///
    /// Note: This is strictly less performant than Executor::async(), since
    /// it may induce cache-line contention on the target worker's loopback
    /// connection.
    ///
    /// @tparam Function Type of functor to execute.
    /// @param index Specifies worker to run functor.
    /// @param function Functor to capture and run.
    ///
    template< typename Function >
    void inject( size_t index, Function && function )
    {
      auto task = make_task( index, std::forward<Function>( function ) );
      workers[ index ].send( task.leak() );
    }

    /// Asynchronously evaluate a functor in the framework.
    ///
    /// Efficiently sends the functor to the specified worker from the current
    /// worker.
    ///
    /// Note: Only valid within Executor!
    ///
    /// @tparam Function Type of functor to execute.
    /// @param index Specifies worker to run functor.
    /// @param function Functor to capture and run.
    /// @return future for asynchronous task.
    ///
    template < typename Function >
    static auto async( size_t index, Function && function )
      -> TaskFuture<Function>
    {
      auto task = make_task( index, std::forward<Function>( function ) );
      acquire( task );
      current_worker->send( task );
      return task;
    }

    /// Re-evaluate the current task elsewhere.
    ///
    /// Note: Only valid within Executor! May only be called once per task
    /// invocation!
    ///
    /// After the current task ends, it is moved to the specified worker and
    /// re-evaluated. The result is discarded and pending tasks remain pending.
    ///
    /// @param index Specifies worker to run functor.
    ///
    template < typename ...Args >
    static void defer( Args && ...args )
    {
      detail::expression::Expression<TaskDispatch>::defer( std::forward<Args>( args )... );
    }

    /// Query the number of worker/threads available.
    ///
    /// Note: Only valid within Executor!
    ///
    static size_t concurrency() { return current_worker->parent.workers.size(); }

    /// Query the index of the current worker/thread.
    ///
    /// Note: Only valid within Executor!
    ///
    static size_t current() { return current_worker->index; }

    /// Query if currently inside an Executor.
    ///
    static bool available() { return current_worker != nullptr; }

    /*void wait() { active.wait(); }*/

   protected:

    /// Task pointer tag values, used to indicate the type of task.
    ///
    /// Tasks are passed via lock-free lists of tagged pointers. The tags
    /// allows recipients to request wakeup, and may be used for other future
    /// specializations.
    ///
    /// Task types:
    ///   - normal: task should be executed by recipient.
    ///   - reverse: task should be executed by sender, and removed from list.
    ///   - delay: TODO: task should be evaluated only when no other work is
    ///     available.
    ///       - Concept: improve cache locality and throughput by
    ///         opportunistically reducing the number of tasks executing at
    ///         once.
    ///       - Obstacle: w/o coroutines, there isn't a clear implicit
    ///         distinction between related and new tasks--most tasks are 
    ///         one-shot.
    ///
    enum class Tag {
      normal,   ///< Task evaluated by recipient.
      reverse,  ///< Task removed and evaluated by sender.
      delay     ///< TODO: See enum discussion.
    };

    /// Adapter that dispatches promises within an Executor.
    ///
    /// It provides the message linkage required by interconnect, and uses
    /// Executor's static variables to dispatch expressions within Executor.
    ///
    struct TaskDispatch : public interconnect::Message {
     public:

      // Use message's constructor.
      //
      using interconnect::Message::Message;

      /// Accept a referenced::pointer<Expression<TaskDispatch>> and send it.
      ///
      /// Steals the provided reference. TODO: Strict type?
      ///
      template < typename T >
      friend void dispatch( T && task )
      {
        current_worker->send( task );
        task.leak();
      }
    };

    // Leverage future/promise/continuation for execution
    //
    using Task = detail::expression::Expression<TaskDispatch>;

    /// Helper to safely get the function arg type at the specified index.
    ///
    /// Defaults to void if the index exceeds the number of arguments.
    ///
    /// TODO: Move this somewhere more appropriate--maybe
    /// function_traits::safe_arg<Index>::type?
    ///
    template < typename Function, size_t Index, typename Traits = function_traits<Function> >
    using function_arg_t = typename std::conditional<(Index < Traits::nargs), typename Traits::template args<Index>, void >::type;

    /// Abbreviate template instantiation for creating tasks.
    ///
    template < typename Function,
      typename Arg = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::return_type >
    using Continuation = detail::expression::Continuation<TaskDispatch,Function,Arg,Result>;

    // Sanity check compatibility with interconnect.
    //
    static_assert( valid_static_cast<Task,interconnect::Message>::value, "Incompatible implementation!" );

    /// Helper to capture a function into a new task.
    ///
    /// TODO: evaluate ways allow caching behavior via template args.
    ///
    template < typename DispatchSpec,
      typename Function,
      typename Arg = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::return_type >
    static referenced::Pointer<Task> make_task( DispatchSpec && dispatch, Function && function )
    {
      return new Continuation<Function, Arg, Result>{ std::forward<DispatchSpec>( dispatch ), std::forward<Function>( function ) };
    }

    /// Executor worker, executes and sends tasks within the ExecutionModel.
    ///
    /// Workers are functors that accept an Idle class and execute/send tasks.
    ///
    class Worker {
     public:
      Worker( const typename Interconnect::NodeType & node_arg, Executor & parent_arg, const size_t index_arg )
      : node( node_arg )
      , parent( parent_arg )
      , index( index_arg )
      {}

      // Unclear why we need to force the move constructor generation.
      //
      Worker( Worker && ) noexcept = default;

      /// Clean up all pending messages.
      ///
      /// Messages may be waiting in:
      ///   - The interconnect node's connections.
      ///   - The sentinel_cache, which re-uses unused sentinels.
      ///
      ~Worker()
      {
        auto sentinel = [](){ return TaggedPointer<Task>{ nullptr, Tag::normal }.template cast<interconnect::Message>(); };
        node.receive( sentinel, []( const TaggedPointer<interconnect::Message> & message )
          {
            release( message.template cast<Task>() );
          });

        while( ! sentinel_cache.empty() )
        {
          auto task = sentinel_cache.remove().template cast<Task>();
          release( task );
        }
      }

      /// Send a task, usurping the current reference.
      ///
      /// Wakes the recipient if requested via a Tag::reverse message.
      ///
      /// Note: this is used both by the worker(via Executor::async) and other
      /// callers (via Executor::inject).
      ///
      void send( Task * task )
      {
        node.send( TaggedPointer<Task>{ task, Tag::normal }.template cast<interconnect::Message>(), PrepareMessage{} );
        /*
        TaggedPointer<Task> tagged{ task, Tag::normal };
        TaggedPointer<interconnect::Message> message;
        node.send( tagged.template cast<typename interconnect::Message>(), [&message]( const interconnect::Message::PointerType & prior )
          {
            message = prior;
            switch( prior.tag<Tag>() )
            {
              case( Tag::reverse ):
                return interconnect::Message::PointerType{ nullptr, Tag::normal };
              default:
                return prior;
            }
          });

        if( message.template tag<Tag>() == Tag::reverse )
        {
          auto wake = message.cast<Task>();
          wake->evaluate();
          release( wake );
        }*/
      }

      /// Event loop for the worker, specialized based on idle type.
      ///
      /// Runs until (1) no tasks remain and (2) idle.yield() indicates exit.
      ///
      /// Messaging is done in an optimisitic manner: Wake-up is only requested
      /// when no messages were processed during the previous survey of all
      /// interconnect connections. Additionally, unevaluated sentinel messages
      /// are cached for reuse.
      ///
      /// TODO: Find a way to efficiently notify executor when no workers are
      /// running for Executor::wait() concept.
      /// 
      /// @tparam Idle Type of idle implementation (see detail::idle).
      /// @param Idle reference to idle object.
      ///
      template < typename Idle >
      void operator()( Idle && idle )
      {
        current_worker = this;
        //bool prepare_idle = false;
        MessageAgent<Idle> agent{ idle };
        for(;;)
        {
          node.operate( agent );
          if( agent.processed == 0 )
          {
            if( agent.prepare_idle )
            {
              const bool exit = !idle.yield();
              if( exit )
              {
                break;
              }
            }
            agent.prepare_idle = !agent.prepare_idle;
          }
          else
          {
            agent.prepare_idle = false;
          }
          agent.processed = 0;
            
          /*if( prepare_idle )
          {
            IdleAgent<Idle> agent{ sentinel_cache, idle };
            node.operate( agent );

            if( agent.processed == 0 )
            {
              // Sleep unless instructed to exit.
              //
              const bool exit = !idle.yield();
              if( exit )
              {
                break;
              }
            }
            prepare_idle = false;
          }
          else
          {
            BasicAgent agent{ sentinel_cache };
            node.operate( agent );
            prepare_idle = (agent.processed == 0);
          }*/
          /*
          size_t processed = 0;
          auto sentinel = [&]() { return make_sentinel( idle, prepare_idle ); };
          node.receive( sentinel, [&]( const TaggedPointer<interconnect::Message> & message )
            {
              if( message.template tag<Tag>() == Tag::normal )
              {
                const auto task = message.template cast<Task>();
                task->evaluate();
                processed += 1;
                release( task );
              }
              else
              {
                //const auto task = message.template cast<Task>();
                //release( task );
                sentinel_cache.insert( message );
              }
            });

          if( processed == 0 )
          {
            if( prepare_idle )
            {
              // Signal we're out of work for Executor::wait()
              //
              //parent.active.decrement();

              // Sleep unless instructed to exit.
              //
              const bool exit = !idle.yield();
              if( exit )
              {
                break;
              }
              else
              {
                // TODO: notify should actually be in the wake task to avoid
                // false-positives, but accurate counting from that context
                // isn't straight forward.
                //
                //parent.join.notify( -1 );
                prepare_idle = false;
              }
            }
            else
            {
              prepare_idle = true;
            }
          }
          else
          {
            prepare_idle = false;
          }*/
        }
        current_worker = nullptr;
      }
     protected:
      struct PrepareMessage
      {
        TaggedPointer<interconnect::Message> message;
        TaggedPointer<interconnect::Message> operator() ( const TaggedPointer<interconnect::Message> & prior )
        {
          message = prior;
          switch( prior.tag<Tag>() )
          {
            case( Tag::reverse ):
              return interconnect::Message::PointerType{ nullptr, Tag::normal };
            default:
              return prior;
          }
        }

        ~PrepareMessage()
        {
          if( message.template tag<Tag>() == Tag::reverse )
          {
            auto wake = message.cast<Task>();
            wake->evaluate();
            release( wake );
          }
        }
      };
    
      struct BasicAgent
      {
        interconnect::Batch & cache;
        size_t processed = 0;

        BasicAgent( interconnect::Batch & cache_arg )
        : cache( cache_arg )
        {}

        void receive( const TaggedPointer<interconnect::Message> & message )
        {
          if( message.template tag<Tag>() == Tag::normal )
          {
            const auto task = message.template cast<Task>();
            task->evaluate();
            processed += 1;
            release( task );
          }
          else
          {
            cache.insert( message );
          }
        }

        PrepareMessage preparer() { return PrepareMessage{}; }

        TaggedPointer<interconnect::Message> sentinel() const
        {
          return TaggedPointer<interconnect::Message>{ nullptr, Tag::normal };
        }
      };

      template <typename Idle>
      struct IdleAgent : BasicAgent
      {
        using BasicAgent::cache;
        Idle & idle;

        IdleAgent( interconnect::Batch & cache_arg, Idle & idle_arg )
        : BasicAgent( cache_arg )
        , idle( idle_arg )
        {}

        TaggedPointer<interconnect::Message> sentinel() const
        {
          if( cache.empty() )
          {
            auto task = make_task( typename TaskDispatch::Unaddressed{}, [&idle = idle]()
              {
                idle.interrupt();
              });
            TaggedPointer<Task> tagged{ task.leak(), Tag::reverse };
            return tagged.template cast<interconnect::Message>();
          }
          else
          {
            auto message = cache.remove();
            message->next() = nullptr;
            return message;
          }
        }
      };

      template <typename Idle>
      struct MessageAgent
      {
        Idle & idle;
        interconnect::Batch cache;
        size_t processed = 0;
        bool prepare_idle = false;

        MessageAgent( Idle & idle_arg )
        : idle( idle_arg )
        {}

        ~MessageAgent()
        {
          while( !cache.empty() )
          {
            release( cache.remove().template cast<Task>() );
          }
        }

        void receive( const TaggedPointer<interconnect::Message> & message )
        {
          if( message.template tag<Tag>() == Tag::normal )
          {
            const auto task = message.template cast<Task>();
            task->evaluate();
            processed += 1;
            release( task );
          }
          else
          {
            cache.insert( message );
          }
        }

        PrepareMessage preparer() { return PrepareMessage{}; }

        TaggedPointer<interconnect::Message> sentinel()
        {
          if( prepare_idle )
          {
            if( cache.empty() )
            {
              auto task = make_task( typename TaskDispatch::Unaddressed{}, [&idle = idle]()
                {
                  idle.interrupt();
                });
              TaggedPointer<Task> tagged{ task.leak(), Tag::reverse };
              return tagged.template cast<interconnect::Message>();
            }
            else
            {
              auto message = cache.remove();
              message->next() = nullptr;
              return message;
            }
          }
          else
          {
            return TaggedPointer<interconnect::Message>{ nullptr, Tag::normal };
          }
        }
      };

      /// Make a sentinel message for receiving messages.
      ///
      /// If preparing to idle, returns an Tag::reverse executable sentinel message,
      /// otherwise nullptr. Attempts to use cached sentinels.
      ///
      /// @tparam Idle type of Idle to wrap for wakeup/interrupt request.
      /// @param idle reference to idle to wrap into sentinel message.
      /// @param prepare_idle indicate if a message should be prepared.
      ///
      template < typename Idle >
      TaggedPointer<interconnect::Message> make_sentinel( Idle && idle, bool prepare_idle )
      {
        if( prepare_idle )
        {
          if( sentinel_cache.empty() )
          {
            auto task = make_task( typename TaskDispatch::Unaddressed{}, [this,&idle]()
              {
                idle.interrupt();
              });
            TaggedPointer<Task> tagged{ task.leak(), Tag::reverse };
            return tagged.template cast<interconnect::Message>();
          }
          else
          {
            auto message = sentinel_cache.remove();
            message->next() = nullptr;
            return message;
          }
        }
        else
        {
          return TaggedPointer<interconnect::Message>{ nullptr, Tag::normal };
        }
      }

      const typename Interconnect::NodeType & node;
      interconnect::Batch sentinel_cache;
     public:
      Executor & parent;
      const size_t index;
    };

    /// Helper to initialize a vector of workers--cleans up constructor.
    ///
    static std::vector<Worker> make_workers( Interconnect & interconnect, size_t count, Executor & parent )
    {
      std::vector<Worker> workers;
      workers.reserve( count );
      for( size_t index = 0; index < count; ++index )
      {
        workers.emplace_back( interconnect.node( index ), parent, index );
      }
      return workers;
    }

    //detail::Counter active;
    Interconnect interconnect;
    std::vector<Worker> workers;
    ExecutionModel execution;
    static thread_local Worker * current_worker;
  };

  template < typename Interconnect, typename ExecutionModel >
  thread_local typename Executor<Interconnect,ExecutionModel>::Worker * Executor<Interconnect,ExecutionModel>::current_worker = nullptr;
}
