#pragma once

#include "interconnect.h"
#include "future.h"

#include <thread>
#include <condition_variable>
#include <functional>

namespace rabid {

  namespace detail {

    class FastIdle {
     public:
      bool yeild()
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

      void interrupt()
      {
        auto signal = armed.exchange( false, std::memory_order_relaxed );
        if( signal )
        {
          std::unique_lock<std::mutex> lock{ mutex };
          lock.unlock();
          condition.notify_one();
        }
      }

      void enable( bool value )
      {
        std::unique_lock<std::mutex> lock{ mutex };
        enabled = value;
        lock.unlock();
        condition.notify_one();
      }

     protected:
      std::atomic<bool> armed{true};
      std::mutex mutex;
      std::condition_variable condition;
      bool enabled = true;
    };

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

      void notify( ssize_t amount = 1 )
      {
        if( count.fetch_add( -amount, std::memory_order_relaxed ) == 1 )
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
  }

  class ThreadModel {
   public:
    using Idle = detail::FastIdle;

    template < typename Iterator >
    ThreadModel( const Iterator & begin, const Iterator & end )
    {
      for( auto func = begin; func != end; ++func )
      {
        threads.emplace_back( std::make_unique<Thread>( *func ) );
      }
    }

    ~ThreadModel()
    {
      for( auto & thread : threads )
      {
        thread->idle.enable( false );
        thread->thread.join();
      }
    }

   protected:
    struct Thread {
      Idle idle;
      std::thread thread;

      template < typename Function >
      Thread( Function && function )
      : thread( [&]{ function( idle ); } )
      {}
    };
    std::vector<std::unique_ptr<Thread>> threads;
  };

  template < typename Interconnect, typename ExecutionModel >
  class Executor {
   protected:
    struct TaskDispatch;

    template <typename Function>
    using TaskFuture = Future<typename function_traits<Function>::return_type, TaskDispatch>;

   public:
    template < typename Function >
    static auto async( size_t index, Function && function )
      -> TaskFuture<Function>
    {
      auto task = make_task( index, std::forward<Function>( function ) );
      acquire( task );
      current_worker->send( task );
      return task;
    }

    Executor( size_t size = std::thread::hardware_concurrency() )
    : interconnect( size )
    , workers( make_workers( interconnect, size, *this ) )
    , execution( workers.begin(), workers.end() )
    {}

    template< typename Function >
    void inject( size_t index, Function && function )
    {
      auto task = make_task( index, std::forward<Function>( function ) );
      workers[ index ].send( task.leak() );
    }

    static void defer( size_t index )
    {
      detail::expression::Expression<TaskDispatch>::defer( index );
    }

    static size_t concurrency() { return current_worker->parent.workers.size(); }
    static size_t current() { return current_worker->index; }

   protected:

    enum class Tag {
      normal,
      reverse,
      delay
    };

    struct TaskDispatch : public interconnect::Message {
     public:
      using interconnect::Message::Message;

      template < typename T >
      friend void dispatch( T && task )
      {
        current_worker->send( task );
        task.leak();
      }
    };

    using Task = detail::expression::Expression<TaskDispatch>;

    template < typename Function, size_t Index, typename Traits = function_traits<Function> >
    using function_arg_t = typename std::conditional<(Index < Traits::nargs), typename Traits::template args<Index>, void >::type;

    template < typename Function,
      typename Arg = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::return_type >
    using Continuation = detail::expression::Continuation<TaskDispatch,Function,Arg,Result>;

    static_assert( valid_static_cast<Task,interconnect::Message>::value, "Incompatible implementation!" );

    template < typename DispatchSpec,
      typename Function,
      typename Arg = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::return_type >
    static referenced::Pointer<Task> make_task( DispatchSpec && dispatch, Function && function )
    {
      return new Continuation<Function, Arg, Result>{ std::forward<DispatchSpec>( dispatch ), std::forward<Function>( function ) };
    }

    class Worker {
     public:
      Worker( const typename Interconnect::NodeType & node_arg, const Executor & parent_arg, const size_t index_arg )
      : node( node_arg )
      , parent( parent_arg )
      , index( index_arg )
      {}

      Worker( Worker && ) noexcept = default;

      ~Worker()
      {
        auto sentinel = TaggedPointer<Task>{ nullptr, Tag::normal }.template cast<interconnect::Message>();
        for( auto & connection : node.all() )
        {
          auto batch = connection.receive( sentinel );
          while( !batch.empty() )
          {
            auto task = batch.remove().template cast<Task>();
            release( task );
          }
        }
        while( ! sentinel_cache.empty() )
        {
          auto task = sentinel_cache.remove().template cast<Task>();
          release( task );
        }
      }
      
      void send( Task * task )
      {
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
        }
      }

      template < typename Idle >
      void operator()( Idle && idle )
      {
        current_worker = this;
        bool prepare_idle = false;
        for(;;)
        {
          size_t processed = 0;
          for( auto & connection : node.all() )
          {
            auto sentinel = make_sentinel( idle, prepare_idle );
            auto batch = connection.receive( sentinel );
            while( !batch.empty() )
            {
              const auto message = batch.remove();
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
            }
          }
          if( processed == 0 )
          {
            if( prepare_idle )
            {
              bool exit = !idle.yeild();
              if( exit )
              {
                break;
              }
              prepare_idle = false;
            }
            else
            {
              prepare_idle = true;
            }
          }
          else
          {
            prepare_idle = false;
          }
        }
        current_worker = nullptr;
      }
     protected:
      template < typename Idle >
      TaggedPointer<interconnect::Message> make_sentinel( Idle && idle, bool prepare_idle )
      {
        if( prepare_idle )
        {
          if( sentinel_cache.empty() )
          {
            auto task = make_task( typename TaskDispatch::Unaddressed{}, [&idle](){ idle.interrupt(); } );
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
      const Executor & parent;
      const size_t index;
    };

    static std::vector<Worker> make_workers( Interconnect & interconnect, size_t count, const Executor & parent )
    {
      std::vector<Worker> workers;
      workers.reserve( count );
      for( size_t index = 0; index < count; ++index )
      {
        workers.emplace_back( interconnect.node( index ), parent, index );
      }
      return workers;
    }

    Interconnect interconnect;
    std::vector<Worker> workers;
    ExecutionModel execution;
    static thread_local Worker * current_worker;
  };

  template < typename Interconnect, typename ExecutionModel >
  thread_local typename Executor<Interconnect,ExecutionModel>::Worker * Executor<Interconnect,ExecutionModel>::current_worker = nullptr;
}
