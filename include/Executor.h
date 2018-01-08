#pragma once

#include "interconnect.h"

#include <thread>
#include <condition_variable>
#include <functional>


namespace rabid {

  class ThreadModel {
   public:
    class Idle {
     public:
      bool yeild()
      {
        std::unique_lock<std::mutex> lock( mutex ); 
        condition.wait( lock );
        return enabled.load( std::memory_order_relaxed );
      }

      void interrupt()
      {
        condition.notify_one();
      }

      void enable( bool value )
      {
        enabled.store( value, std::memory_order_relaxed );
        condition.notify_one();
      }
     protected:
      std::mutex mutex;
      std::condition_variable condition;
      std::atomic<bool> enabled{ true };
    };

    template < typename Iterator >
    ThreadModel( const Iterator & begin, const Iterator & end )
    {
      for( auto func = begin; func != end; ++func )
      {
        threads.emplace_back( *func );
      }
    }

   protected:
    struct Thread {
      std::unique_ptr<Idle> idle;
      std::thread thread;

      template < typename Function >
      Thread( Function && function )
      : idle( std::make_unique<Idle>() )
      , thread( [&]{ function( *idle ); } )
      {}

      ~Thread()
      {
        thread.join();
      }
    };
    std::vector<Thread> threads;
  };

  template < typename Interconnect, typename ExecutionModel >
  class Executor {
   public:
    template < typename Function >
    static void send( size_t index, Function && function )
    {
      current_worker->send( index, std::forward<Function>( function ) );
    }

    Executor( size_t cache, size_t size = std::thread::hardware_concurrency() )
    : interconnect( size )
    , workers( make_workers( interconnect, size, cache ) )
    , execution( workers.begin(), workers.end() )
    {}
    
   protected:
    using Executable = std::function<void()>;

    struct Task : public interconnect::Message {
      enum class Tag {
        reverse,
        normal,
        delay
      };
      Executable executable;
    };

    static_assert( valid_static_cast<Task,interconnect::Message>::value, "Wahahah" );

    class Worker {
     public:
      Worker( const typename Interconnect::NodeType & node_arg, size_t capacity )
      : node( node_arg )
      , cache( capacity )
      {}

      template < typename Function >
      void send( size_t index, Function && function )
      {
        auto task = cache.capture( std::forward<Function>( function ) );
        task.tag( Task::Tag::normal );
        TaggedPointer<Task> wake{};
        node.route( index ).send( task, [&wake]( const interconnect::Message::PointerType & prior )
          {
            wake = prior;
            switch( prior.tag<Task::Tag>() )
            {
              case( Task::Tag::reverse ):
                return interconnect::Message::PointerType{ nullptr, Task::Tag::normal };
              default:
                return prior;
            }
          });

        if( wake.template tag<Task::Tag>() == Task::Tag::reverse )
        {
          wake->executable();
        }
      }

      template < typename Idle >
      void operator()( Idle && idle )
      {
        current_worker = this;
        for(;;)
        {
          bool prepare_idle = true;
          for( auto & connection : node.all() )
          {
            TaggedPointer<interconnect::Message> sentinel = make_sentinel( idle, prepare_idle );
            auto batch = connection.receive( sentinel );
            while( !batch.empty() )
            {
              TaggedPointer<Task> task = batch.remove();
              task->executable();
              cache.put( task );
            }
          }

          if( prepare_idle )
          {
            bool exit = idle.yeild();
            if( exit )
            {
              break;
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
          return cache.capture( [&idle](){ idle.interrupt(); } );
        }
        else
        {
          return TaggedPointer<Task>{ nullptr, Task::Tag::normal };
        }
      }

      class TaskCache {
       public:
        TaggedPointer<Task> get()
        {
          TaggedPointer<Task> result;
          if( cache.size() )
          {
            result = cache.back();
            cache.pop_back();
          }
          else
          {
            result = TaggedPointer<Task>( new Task{} );
          }
          return result;
        }

        template < typename Function >
        TaggedPointer<Task> capture( Function && function )
        {
          auto result = get();
          result->executable = std::forward<Function>( function );
          return result;
        }

        void put( const TaggedPointer<Task> & task )
        {
          if( cache.size() < cache.capacity )
          {
            cache.emplace_back( task );
          }
          else
          {
            delete task.get();
          }
        }

        TaskCache( size_t capacity )
        {
          cache.reserve( capacity );
        }

        ~TaskCache()
        {
          for( auto & task : cache )
          {
            delete task.get();
          }
        }
       protected:
        std::vector<TaggedPointer<Task>> cache;
      };

      const typename Interconnect::NodeType & node;
      TaskCache cache;
    };

    static std::vector<Worker> make_workers( Interconnect & interconnect, size_t count, size_t cache )
    {
      std::vector<Worker> workers;
      workers.reserve( count );
      for( size_t index = 0; index < count; ++index )
      {
        workers.emplace_back( interconnect.node( index ), cache );
      }
      return workers;
    }

    Interconnect interconnect;
    std::vector<Worker> workers;
    ExecutionModel execution;
    static thread_local Worker * current_worker;
  };
  

  template < typename Interconnect, typename ExecutionModel >
  thread_local typename Executor<Interconnect,ExecutionModel>::Worker * current_worker = nullptr;
}
