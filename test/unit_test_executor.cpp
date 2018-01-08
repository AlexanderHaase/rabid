#include <catch.hpp>

#include <Executor.h>

using namespace rabid;

SCENARIO( "executor should run things in parallel" )
{
  GIVEN( "an executor" )
  {
    THEN( "a task should run" )
    {
      Executor<interconnect::Direct, ThreadModel> executor{ 1 };
    }
  }
}
