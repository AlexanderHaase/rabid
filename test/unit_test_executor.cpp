#include <catch.hpp>

#include <Executor.h>

using namespace rabid;

SCENARIO( "executor should run things in parallel" )
{
  GIVEN( "an executor" )
  {
    THEN( "a task should run" )
    {
      std::atomic<size_t> count{ 0 };
      using Exec = Executor<interconnect::Direct, ThreadModel>;
      Exec executor{ 1 };

      executor.inject( 0, [&count]{
          const auto limit = std::thread::hardware_concurrency();
          for( size_t index = 0; index < limit; ++index )
          {
            Exec::async( index, [&count]{ count.fetch_add( 1 ); });
          }
        });

      std::this_thread::sleep_for(std::chrono::milliseconds( 1000 ));
      REQUIRE( count == std::thread::hardware_concurrency() );
    }
  }
}
