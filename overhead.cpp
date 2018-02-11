
#include <limits>
#include <iostream>
#include <chrono>

#include "include/Executor.h"

using namespace rabid;

auto overhead_executor_copy( size_t iterations,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;

  using rabid::detail::Join;

  struct Job {
    size_t limit;
    size_t iterations;
    Join & join;

    void operator() ()
    {
      if( ++iterations < limit )
      {
        Exec::async( Exec::current(), Job{*this} );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    executor.inject( job % concurrency, Job{ iterations, 0, join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

auto overhead_executor_defer( size_t iterations,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;

  using rabid::detail::Join;

  struct Job {
    size_t limit;
    size_t iterations;
    Join & join;

    void operator() ()
    {
      if( ++iterations < limit )
      {
        Exec::defer( Exec::current() );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    executor.inject( job % concurrency, Job{ iterations, 0, join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

auto rotate_executor_copy( size_t iterations,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;

  using rabid::detail::Join;

  struct Job {
    size_t limit;
    size_t iterations;
    Join & join;

    void operator() ()
    {
      if( ++iterations < limit )
      {
        auto next = (Exec::current() + 1) % Exec::concurrency();
        Exec::async( next, Job{*this} );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    executor.inject( job % concurrency, Job{ iterations, 0, join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

auto rotate_executor_defer( size_t iterations,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;

  using rabid::detail::Join;

  struct Job {
    size_t limit;
    size_t iterations;
    Join & join;

    void operator() ()
    {
      if( ++iterations < limit )
      {
        auto next = (Exec::current() + 1) % Exec::concurrency();
        Exec::defer( next );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    executor.inject( job % concurrency, Job{ iterations, 0, join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

int main( int argc, char ** argv )
{
  const size_t iterations = ( argc > 1 ? strtoul( argv[ 1 ], nullptr, 10 ) : 10000 );
  const size_t concurrency = ( argc > 3 ? strtoul( argv[ 3 ], nullptr, 10 ) : std::thread::hardware_concurrency() );
  const size_t job_multipler = ( argc > 2 ? strtoul( argv[ 2 ], nullptr, 10 ) : concurrency * concurrency );

  {
    const auto duration = overhead_executor_copy( iterations, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  {
    const auto duration = overhead_executor_defer( iterations, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  /*{
    const auto duration = rotate_executor_copy( iterations, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  {
    const auto duration = rotate_executor_defer( iterations, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }*/

  return 0;
}
