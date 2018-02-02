#pragma once

#include "interconnect.h"
#include "future.h"

#include <thread>
#include <condition_variable>
#include <functional>

namespace rabid {

  namespace detail {

    class ConservativeIdle {
     public:
      bool yeild()
      {
        std::unique_lock<std::mutex> lock( mutex );
        if( enabled )
        {
          condition.wait( lock );
        }
        return enabled;
      }

      void interrupt()
      {
        std::unique_lock<std::mutex> lock( mutex );
        lock.unlock();
        condition.notify_one();
      }

      void enable( bool value )
      {
        std::unique_lock<std::mutex> lock( mutex );
        enabled = value;
        lock.unlock();
        condition.notify_one();
      }

     protected:
      std::mutex mutex;
      std::condition_variable condition;
      bool enabled = true;
    };

    class FastIdle {
     public:
      bool yeild()
      {
        std::unique_lock<std::mutex> lock( mutex );
        if( enabled )
        {
          indicator.store( &mutex, std::memory_order_relaxed );
          condition.wait( lock );
        }
        return enabled;
      }

      void interrupt()
      {
        auto signal = indicator.exchange( nullptr, std::memory_order_relaxed );
        if( signal )
        {
          std::unique_lock<std::mutex> lock( mutex );
          lock.unlock();
          condition.notify_one();
        }
      }

      void enable( bool value )
      {
        std::unique_lock<std::mutex> lock( mutex );
        enabled = value;
        lock.unlock();
        condition.notify_one();
      }

     protected:
      std::atomic<std::mutex*> indicator;
      std::mutex mutex;
      std::condition_variable condition;
      bool enabled = true;
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
      auto task = make_task( std::forward<Function>( function ) );
      task->address = index;
      acquire( task );
      current_worker->send( task );
      return task;
    }

    Executor( size_t size = std::thread::hardware_concurrency() )
    : interconnect( size )
    , workers( make_workers( interconnect, size ) )
    , execution( workers.begin(), workers.end() )
    {}

    template< typename Function >
    void inject( size_t index, Function && function )
    {
      auto task = make_task( std::forward<Function>( function ) );
      task->address = index;
      workers[ index ].send( task.leak() );
    }

   protected:

    enum class Tag {
      normal,
      reverse,
      delay
    };

    struct TaskDispatch : interconnect::Message {
      size_t address;

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

    template < typename Function,
      typename Arg = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::return_type >
    static referenced::Pointer<Task> make_task( Function && function )
    {
      return new Continuation<Function, Arg, Result>{ std::forward<Function>( function ) };
    }

    class Worker {
     public:
      Worker( const typename Interconnect::NodeType & node_arg )
      : node( node_arg )
      {}

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
      }
      
      void send( Task * task )
      {
        TaggedPointer<Task> tagged{ task, Tag::normal };
        TaggedPointer<Task> wake;
        node.route( task->address ).send( tagged.template cast<typename interconnect::Message>(), [&wake]( const interconnect::Message::PointerType & prior )
          {
            wake = prior.cast<Task>();
            switch( prior.tag<Tag>() )
            {
              case( Tag::reverse ):
                return interconnect::Message::PointerType{ nullptr, Tag::normal };
              default:
                return prior;
            }
          });

        if( wake.template tag<Tag>() == Tag::reverse )
        {
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
            auto sentinel = make_sentinel( idle, prepare_idle ).template cast<interconnect::Message>();
            auto batch = connection.receive( sentinel );
            while( !batch.empty() )
            {
              auto task = batch.remove().template cast<Task>();
              if( task.template tag<Tag>() == Tag::normal )
              {
                task->evaluate();
                processed += 1;
              }
              release( task );
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
            }
            else
            {
              prepare_idle = true;
            }
          }
        }
        current_worker = nullptr;
      }
     protected:
      template < typename Idle >
      TaggedPointer<Task> make_sentinel( Idle && idle, bool prepare_idle )
      {
        if( prepare_idle )
        {
          auto task = make_task( [&idle](){ idle.interrupt(); } );
          task->next() = nullptr;
          TaggedPointer<Task> tagged{ task.leak(), Tag::reverse };
          return tagged;
        }
        else
        {
          return TaggedPointer<Task>{ nullptr, Tag::normal };
        }
      }

      const typename Interconnect::NodeType & node;
    };

    static std::vector<Worker> make_workers( Interconnect & interconnect, size_t count )
    {
      std::vector<Worker> workers;
      workers.reserve( count );
      for( size_t index = 0; index < count; ++index )
      {
        workers.emplace_back( interconnect.node( index ) );
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
