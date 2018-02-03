#include <catch.hpp>
#include <Executor.h>
#include <iostream>

using namespace rabid;

struct Test : referenced::Object<Test> {
  static std::atomic<size_t> active;

  Test()
  {
    active.fetch_add( 1 );
  }

  ~Test()
  {
    active.fetch_add( -1 );
  }
};

std::atomic<size_t> Test::active{ 0 };

SCENARIO( "referenced::Pointer should behave according to refptr semantics" )
{
  GIVEN( "a test object type" )
  {
    THEN( "referenced::Pointer should adequately hand lifecycles" )
    {
      REQUIRE( Test::active.load() == 0 );
      referenced::Pointer<Test> a{ new Test{} };
      REQUIRE( Test::active.load() == 1 );
      referenced::Pointer<Test> b = a;
      REQUIRE( Test::active.load() == 1 );
      a = new Test{};
      REQUIRE( Test::active.load() == 2 );
      b = new Test{};
      REQUIRE( Test::active.load() == 2 );
      a = nullptr;
      REQUIRE( Test::active.load() == 1 );
      b = std::move( a );
      REQUIRE( Test::active.load() == 0 );
    }

    THEN( "usurping/leaking pointers shouldn't affect the reference count" )
    {
      Test * val = new Test{};
      acquire( val );
      referenced::Pointer<Test> a;
      REQUIRE( Test::active.load() == 1 );
      a.usurp( val );
      REQUIRE( Test::active.load() == 1 );
      a.leak();
      REQUIRE( Test::active.load() == 1 );
      a.leak();
      REQUIRE( Test::active.load() == 1 );
      release( val );
      REQUIRE( Test::active.load() == 0 );
    }
  }
}


SCENARIO( "executor should run things in parallel" )
{
  GIVEN( "an executor" )
  {
    THEN( "a task should run" )
    {
      std::atomic<size_t> count{ 0 };
      std::atomic<size_t> followup{ 0 };
      using Exec = Executor<interconnect::Direct, ThreadModel>;
      const auto capacity = std::thread::hardware_concurrency();
      Exec executor{ capacity };

      executor.inject( 0, [&count,&followup]{
          const auto limit = Exec::concurrency();
          for( size_t index = 0; index < limit; ++index )
          {
            Exec::async( index, [&count]{ count.fetch_add( 1 ); })
              .then( [&followup]{ followup.fetch_add( 1 ); })
              .then( (index+1) % limit, [&followup]{ followup.fetch_add( 1 ); } );
          }
        });

      std::this_thread::sleep_for(std::chrono::milliseconds( 1000 ));
      REQUIRE( count == capacity );
      REQUIRE( followup == 2 * capacity );
    }
  }
}
