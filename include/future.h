#pragma once

#include "referenced.h"
#include "capture.h"

namespace rabid {

  template < typename Value>
  class Future {
   protected:
    class Result;

   public:
    template < typename Function >
    auto then( Function && function ) const
      -> Future< typename function_traits<Function>::return_type >
    {
      referenced::Pointer<Erasure<Function>> erasure{ new Erasure<Function>{ std::forward<Function>( function ) } };
      result->apply( erasure );
      return erasure->future();
    }

    struct Promise {
      Promise()
      {
        future.result = new Result;
      }
      Future future;
      void complete( Value && value )
      {
        future.result->emplace( std::move( value ) );
      }
    };

   protected:
    struct Expression : referenced::Object<Expression> {
      referenced::Pointer<Expression> next;
      virtual ~Expression() = default;
      virtual void evaluate( Value & value ) = 0;
    };

    class Result : public referenced::Object<Result> {
     public:
      void apply( Expression * expression )
      {
        acquire( expression );
        for(;;)
        {
          Expression * prior = next.load( std::memory_order_relaxed );
          if( prior == sentinel() )
          {
            release( expression );
            expression->evaluate( value() );
            break;
          }
          expression->next.usurp( prior );
          if( next.compare_exchange_strong( prior, expression, std::memory_order_relaxed ) )
          {
            break;
          }
          else
          {
            expression->next.leak();
          }
        }
      }

      void emplace( Value && result )
      {
        new (&storage) Value{ result };
        referenced::Pointer<Expression> pending;
        pending.usurp( next.exchange( sentinel(), std::memory_order_relaxed ) );
        while( pending )
        {
          pending->evaluate( value() );
          pending = std::move( pending->next );
        }
      }

      ~Result()
      {
        auto pending = next.load( std::memory_order_relaxed );
        if( pending == sentinel() )
        {
          value().~Value();
        }
        else
        {
          release( pending );
        }
      }
     protected:
      Value & value() { return *reinterpret_cast<Value*>( &storage ); }
      Expression * sentinel() { return reinterpret_cast<Expression*>( this ); }
      std::atomic<Expression*> next{nullptr};
      std::aligned_storage_t< sizeof(Value), alignof(Value) > storage;
    };

    template < typename Function >
    class Erasure final : public Expression {
     public:
      using return_type = typename function_traits<Function>::return_type;
      using ResultType = typename Future<return_type>::Result;

      virtual void evaluate( Value & value ) override
      {
        result->emplace( function( value ) );
      }

      template < typename ...Args >
      Erasure( Args && ...args )
      : result( new ResultType{} )
      , function( std::forward<Args>( args )... )
      {}

      Future<return_type> future()
      {
        Future<return_type> future_value;
        future_value.result = result;
        return future_value;
      }

     protected:
      referenced::Pointer<ResultType> result;
      Function function;
    };
    referenced::Pointer<Result> result;
  };
}
