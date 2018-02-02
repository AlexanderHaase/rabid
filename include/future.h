#pragma once

#include "referenced.h"
#include "detail/expression.h"

namespace rabid {

  template < typename Value, typename Dispatch = detail::expression::ImmediateDispatch >
  class Future {
   public:
    using Concept = detail::expression::Expression<Dispatch>;
    template < typename Function, typename Arg, typename Result >
    using Expression = detail::expression::Continuation<Dispatch, Function, Arg, Result >;

    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type, Dispatch>
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

  template < typename Value, typename Dispatch = detail::expression::ImmediateDispatch >
  class Promise {
   public:
    using Concept = typename Future<Value>::Concept;
    template < typename Function, typename Arg, typename Result >
    using Expression = detail::expression::Continuation<Dispatch, Function, Arg, Result >;
    using Argument = detail::expression::Argument<Dispatch, Value >;

    template < typename Function >
    auto then( Function && function )
      -> Future<typename function_traits<Function>::return_type, Dispatch>
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
