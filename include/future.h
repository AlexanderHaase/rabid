#pragma once

#include "referenced.h"
#include "detail/coupling.h"

namespace rabid {

  template < typename Value >
  class Future {
   public:
    using Dispatch = detail::expression::ImmediateDispatch;
    using Concept = detail::expression::Expression<Dispatch>;
    template < typename Function, typename Arg, typename Result >
    using Expression = detail::expression::Continuation<Dispatch, Function, Arg, Result >;

    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type>
    {
      using Result = typename function_traits<Function>::return_type;
      referenced::Pointer<Concept> result{ new Expression<Function,Value,Result>{ std::forward<Function>( function ) } };
      value->chain( result );
      return result;
    }

    Future( referenced::Pointer<Concept> && coupling )
    : value( std::move( coupling ) )
    {}

   protected:
    referenced::Pointer<Concept> value;
  };

  template < typename Value >
  class Promise {
   public:
    using Dispatch = typename Future<Value>::Dispatch;
    using Concept = typename Future<Value>::Concept;
    template < typename Function, typename Arg, typename Result >
    using Expression = detail::expression::Continuation<Dispatch, Function, Arg, Result >;
    using Argument = detail::expression::Argument<Dispatch, Value >;

    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type>
    {
      using Result = typename function_traits<Function>::return_type;
      referenced::Pointer<Concept> result{ new Expression<Function,Value,Result>{ std::forward<Function>( function ) } };
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
    : value( new Argument{} )
    {}

   protected:
    referenced::Pointer<Argument> value;
    };
 }
