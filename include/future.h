#pragma once

#include "referenced.h"
#include "detail/coupling.h"

namespace rabid {

  template < typename Value >
  class Future {
   public:
    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type>
    {
      using Result = typename function_traits<Function>::return_type;
      referenced::Pointer<detail::Coupling> result{ new detail::Statement<Function,Value,Result>{ std::forward<Function>( function ) } };
      value->chain( result );
      return result;
    }

    Future( referenced::Pointer<detail::Coupling> && coupling )
    : value( std::move( coupling ) )
    {}

   protected:
    referenced::Pointer<detail::Coupling> value;
  };

  template < typename Value >
  class Promise {
   public:
    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type>
    {
      using Result = typename function_traits<Function>::return_type;
      referenced::Pointer<detail::Coupling> result{ new detail::Statement<Function,Value,Result>{ std::forward<Function>( function ) } };
      value->chain( result );
      return result;
    }

    template < typename ...Args >
    void complete( Args && ... args )
    {
      value->template value<Value>().construct( std::forward<Args>( args )... );
      value->evaluate();
    }

    Promise()
    : value( new detail::Placeholder<Value>{} )
    {}

   protected:
    referenced::Pointer<detail::Placeholder<Value>> value;
    };
 }
