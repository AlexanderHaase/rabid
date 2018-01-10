#include <catch.hpp>

#include <Executor.h>

using namespace rabid;

SCENARIO( "executor should run things in parallel" )
{
  GIVEN( "an executor" )
  {
    THEN( "a task should run" )
    {
      size_t count = 0;
      Executor<interconnect::Direct, ThreadModel> executor{ 1 };
      executor.inject( 0, [&count]{ ++count; } );
      std::this_thread::sleep_for(std::chrono::milliseconds( 1000 ));
      REQUIRE( count == 1 );
    }
  }
}
