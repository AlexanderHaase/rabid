#include "include/function_traits.h"
#include "include/future.h"
#include "include/detail/expression.h"

#include <iostream>

namespace rabid {

  struct Foo {
    int operator() () const volatile noexcept { return 0; }
  };

  using Bar = function_traits<Foo>;

  Bar::return_type example;
}

int main()
{
  {
    rabid::Promise<int> promise;
    auto before = promise.then( []( int value ) { std::cout << "before: " << value << std::endl; return value; } );
    before
      .then([]( int value ) { return value + 1; })
      .then([]( int value ) { return value + 1; })
      .then([]( int value ) { std::cout << "deep: " << value << std::endl; return value; });
    promise.complete( 0 );
    auto after = promise.then( []( int value ) { std::cout << "after: " << value << std::endl; return value; } );
  }

  // loop check for leaks
  for( size_t index = 0; index < 1000; ++index )
  {
    rabid::Promise<int> promise;
    auto before = promise.then( []( int value ) { return value; } );
    before
      .then([]( int value ) { return value + 1; })
      .then([]( int value ) { return value + 1; })
      .then([]( int value ) { return value * 10; });
    promise.complete( 0 );
    auto after = promise.then( []( int value ) { return value; } );
  }

  rabid::detail::Container<void> void_value;
  rabid::detail::apply( []{ std::cout << "void -> void" << std::endl; }, void_value, void_value );

  rabid::detail::Container<int> int_value{};
  rabid::detail::apply( []{ return 1; }, int_value, void_value );
  rabid::detail::apply( []( int ) { return -1; }, int_value, int_value );
  rabid::detail::apply( []( int ) {}, void_value, int_value );

  return 0;
}
