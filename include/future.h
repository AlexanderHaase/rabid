#pragma once

#include "referenced.h"
#include "detail/container.h"

namespace rabid {

  namespace detail {

    /// An interface exposing one half of an expression.
    ///
    /// Expression is chainable via next.
    ///
    template < typename Value > 
    struct Expression : referenced::Object<Expression<Value>> {
      referenced::Pointer<Expression> next;
      virtual ~Expression() = default;
      virtual void evaluate( Container<Value> & container ) = 0;
    };

    /// A point that eventually supplies a value to zero or more expressions.
    ///
    template < typename Value >
    class SequencePoint : public referenced::Object<SequencePoint<Value>> {
     public:

      /// Evaluate the expression when the value becomes available.
      ///
      /// Expressions may not be shared among SequencePoints.
      ///
      void invoke( Expression<Value> * expression )
      {
        acquire( expression );
        for(;;)
        {
          Expression<Value> * prior = pending.load( std::memory_order_relaxed );
          if( prior == sentinel() )
          {
            release( expression );
            expression->evaluate( container );
            break;
          }
          expression->next.usurp( prior );
          if( pending.compare_exchange_strong( prior, expression, std::memory_order_relaxed ) )
          {
            break;
          }
          else
          {
            expression->next.leak();
          }
        }
      }

      /// Evaluate a function, supplying the result to pending expressions.
      ///
      template < typename Function, typename Arg >
      void capture( Function && function, Container<Arg> & arg )
      {
        apply( function, container, arg );
        transition();
      }

      /// Construct a value, supplying the result to pending expressions. 
      template < typename ...Args >
      void construct( Args && ... args )
      {
        container.construct( std::forward<Args>( args )... );
        transition();
      }

      ~SequencePoint()
      {
        auto waiting = pending.load( std::memory_order_relaxed );
        if( waiting == sentinel() )
        {
          container.destruct();
        }
        else
        {
          release( waiting );
        }
      }
     protected:
      void transition()
      {
        referenced::Pointer<Expression<Value>> waiting;
        waiting.usurp( pending.exchange( sentinel(), std::memory_order_relaxed ) );
        while( waiting )
        {
          waiting->evaluate( container );
          waiting = std::move( waiting->next );
        }
      }

      Expression<Value> * sentinel() { return reinterpret_cast<Expression<Value>*>( this ); }

      std::atomic<Expression<Value>*> pending{nullptr};
      Container<Value> container;
    };

    template < typename Function, size_t Position>
    using function_arg_t = std::conditional_t<(Position < function_traits<Function>::nargs),
      typename function_traits<Function>::template args<Position>::type,
      void >;


    template < typename Function,
      typename Value = function_arg_t<Function,0>,
      typename Result = typename function_traits<Function>::result_type >
    class Continuation final : public Expression<Value> {
     public:
      virtual void evaluate( Container<Value> & value ) override
      {
        result->capture( function, value );
      }

      template < typename ...Args >
      Continuation( SequencePoint<Result> * sequence, Args && ... args )
      : result( sequence )
      , function( std::forward<Args>( args )... )
      {}

     protected:
      referenced::Pointer<SequencePoint<Result>> result;
      Function function;
    };
  }

  template < typename Value >
  class Future {
   public:
    template < typename Function, typename Result = typename function_traits<Function>::return_type >
    auto then( Function && function ) const
      -> Future<Result>
    {
      Future<Result> result;
      referenced::Pointer<detail::Expression<Value>> continuation =
        new detail::Continuation<Function,Value,Result>{ result.value, std::forward<Function>( function ) };
      value->invoke( continuation );
      return result;
    }
    Future()
    : value( new detail::SequencePoint<Value>{} )
    {}

    void invoke( const referenced::Pointer<detail::Expression<Value>> & expression ) const
    {
      value->invoke( expression );
    }
   protected:
    template < typename Other > friend class Future;
    referenced::Pointer<detail::SequencePoint<Value>> value;
  };

  template < typename Value >
  class Promise : public Future<Value> {
   public:
    void complete( Value && result )
    {
      value->construct( std::move( result ) );
    }

    template <typename Other>
    void complete( const Future<Other> & future )
    {
      auto adapter = []( Other & value ) -> Value { return value; };
      referenced::Pointer<detail::Expression<Other>> continuation =
        new detail::Continuation<decltype(adapter),Other,Value>{ value, adapter };
      future.invoke( continuation );
    }
   protected:
    using Future<Value>::value;
  };
}
