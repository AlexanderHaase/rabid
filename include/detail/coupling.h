#pragma once

#include "container.h"

namespace rabid {

  namespace detail {

    // Current draft lock-free futures/continuations operate with two
    // allocations per function: a result node that captures the function's
    // return value and functions as a sequence point for continuation and
    // splitting, and an expression node that links the function to it's result
    // and may be attached to a result node to be evaluated in due time.
    //
    // The interfaces are well suited to each task: both objects use 
    // reference counting for lifecycle management. The result node leverages
    // uses templating to appropriately destruct the result value if it's ever
    // constructed. Expression nodes use virtual destruction since they erase
    // the captured function's type.
    //
    // In reality, the envisioned use case has more nuance: moving towards
    // integration, the futures/continuation system needs to capture where to
    // evaluate the expression and dispatch it there when the dependent value
    // becomes ready. That closely integrates parallelism with continuations:
    // not only can continuations split, but by prefacing each follow on
    // function with a context, split functions can be evaluated in parallel.
    //
    // Moreover, maintaining a separation of function and result introduces
    // unnecissary inefficiency: Since they always exist in pairs, it doubles
    // reference counting, allocation/initialization, and destruction overhead.
    // Disjoint objects only offer two potential advantages: First, each
    // allocation is smaller, potentially offering better size-class behavior
    // for allocators(requires cache/allocator tooling to fully capitalize).
    // Second, it allows for the function lifecycle to end before it's result.
    // However, it's unlikely that would be a practical issue due to the close
    // coupling of results becoming ready and results being fully consumed.
    //
    // In summary, there's good motivation to explore a joint solution: while
    // both interfaces are neccissary and provide different type erasures,
    // there is not an obvious reason why they can't share a single storage
    // location.
    //
    // Some re-architecting is required to dispatch dependant functions for
    // concurrent evaluation. Currently futures form a simply directed tree
    // flowing from function to result to zero or more functions:
    //
    //   expression<Y> -> result<T>
    //                       |-> expression<T> -> result<A> -> ...
    //                       |-> expression<T> -> result<B> -> ...
    //                       *-> expression<T> -> result<C> -> ...
    //
    // Concurrent execution requires expresions to capture a reference to
    // their arguments when dispatched. When the result becomes ready, it
    // provides a reference of itself to the dependant expression object,
    // then moves it's reference to the dependant expression to the
    // execution context.
    //
    // Structure prior to evaluation of first expression:
    // 
    //     expression[thread A]<Y> -> result<T>
    //                                   |->expression[thread B]<T> -> ...
    //                                   |->expression[thread C]<T> -> ...
    //                                   *->expression[thread D]<T> -> ...
    //
    // After evaluation, dependant expressions are prepared for dispatch:
    //
    //   Thread A:
    //
    //     expression[thread A]<Y> -> result<T>
    //                                  ^^^
    //                                  |||
    //                                  ||*-expression[thread B]<T> -> ...
    //                                  |*--expression[thread C]<T> -> ...
    //                                  *---expression[thread D]<T> -> ...
    //
    // Then dispatched and evaluated (potentially concurrently):
    //
    //                                result<T>
    //                                  ^^^
    //                       Thread B:  |||
    //                                  ||*-expression[thread B]<T> -> ...
    //                       Thread C:  ||
    //                                  |*--expression[thread C]<T> -> ...
    //                       Thread D:  |
    //                                  *---expression[thread D]<T> -> ...
    //
    //
    // Returning to the joint object, coupling the allocation translates to
    // providing two different views of the same object: An expression
    // accepting <Y> and a future result <T>. From a composition perspective,
    // there are three interfaces:
    //
    // 1) Parent container providing:
    //    - Reference count lifecycle management.
    //    - Virtual destructor, encapsulates both destroying captured
    //      function and captured result.
    //
    // 2) Expression interface providing:
    //    - Pointer for lock-free list of expressions.
    //    - Pointer for result object.
    //    - Virtual call to evaluate captured function.
    //
    // 3) Result interface providing:
    //    - Result value
    //    - Head for lock-free list of expressions.
    //
    // Type implications:
    //    - The parent container know the expression type <Y> and result
    //      type <T>. If it provides a uniform storage location for untyped
    //      pointers, the parent container can recover the type.
    //    - The expression virtual interface has uniform void type.
    //    - If the result object can be instantiated at a uniform offset,
    //      it's location can be statically deduced from the parent container.
    //
    // With that in mind, it seems possible to prepare a base class that only
    // requires a single pointer to join both expressions.
    //

    template < class From, class To >
    constexpr ptrdiff_t alignment_thunk()
    {
      if( alignof(To) > alignof(From) )
      {
        return alignof(To) - alignof(From);
      }
      else
      {
        return 0;
      }
    }

    class Coupling : public referenced::Object<Coupling> {
     public:
      /// Destructor, implementations clears result<T> and function<Y>
      ///
      virtual ~Coupling() = default;

      /// Evaluation, implementations chain from argument to result.
      ///
      virtual void evaluate() = 0;

      /// Chain a coupling after this one.
      ///
      void chain( const referenced::Pointer<Coupling> & coupling )
      {
        acquire( coupling );
        for(;;)
        {
          Coupling * prior = pending.load( std::memory_order_relaxed );
          if( prior == sentinel() )
          {
            release( coupling );
            coupling->next = this;
            coupling->evaluate();
            break;
          }
          coupling->next.usurp( prior );
          if( pending.compare_exchange_strong( prior, coupling, std::memory_order_relaxed ) )
          {
            break;
          }
          else
          {
            coupling->next.leak();
          }
        }
      }

      /// Uniform location access for container.
      ///
      template < typename Value >
      Container<Value> & value()
      {
        const auto unaligned = reinterpret_cast<ptrdiff_t>( this + 1 );
        return *reinterpret_cast<Container<Value>*>( unaligned + alignment_thunk<Coupling,Value>() );
      }

     protected:
      /// Head of linked list of dependant expressions
      ///
      std::atomic<Coupling*> pending{ nullptr };

      /// Dual use pointer:
      ///  - Element for participating in linked lists.
      ///  - Points to argument of evaluation.
      ///
      referenced::Pointer<Coupling> next;

      /// Sentinel value for linked list
      ///
      Coupling * sentinel() { return this; }

      /// Function for evaluating all pending couplings.
      ///
      void complete()
      {
        referenced::Pointer<Coupling> waiting;
        waiting.usurp( pending.exchange( sentinel(), std::memory_order_acquire ) );
        while( waiting )
        {
          auto next = std::move( waiting->next );
          waiting->next = this;
          waiting->evaluate();
          waiting = std::move( next );
        }
      }
    };

    template < typename Function, typename Arg, typename Result >
    class Statement final : public Coupling {
     public:
      virtual ~Statement()
      {
        Coupling * value = pending.load( std::memory_order_relaxed );
        if( value == sentinel() )
        {
          container.destruct();
        }
        else
        {
          release( value );
        }
      }

      virtual void evaluate( void ) override
      {
        apply( function, container, next->value<Arg>() );
        complete();
      }

      template < typename ...Args >
      Statement( Args && ...args )
      : function( std::forward<Args>( args )... )
      {}

     protected:
      Container<Result> container;
      Function function;
    };
    
    template < typename Result >
    class Placeholder final : public Coupling {
     public:
      virtual ~Placeholder()
      {
        Coupling * value = pending.load( std::memory_order_relaxed );
        if( value == sentinel() )
        {
          container.destruct();
        }
        else
        {
          release( value );
        }
      }

      virtual void evaluate( void ) override
      {
        complete();
      }

     protected:
      Container<Result> container;
    };

    template < typename Value >
    class Future2 {
     public:
      template < typename Function >
      auto then( Function && function )
        -> Future2<typename function_traits<Function>::return_type>
      {
        using Result = typename function_traits<Function>::return_type;
        referenced::Pointer<Coupling> result{ new Statement<Function,Value,Result>{ std::forward<Function>( function ) } };
        value->chain( result );
        return result;
      }

      Future2( referenced::Pointer<Coupling> && coupling )
      : value( std::move( coupling ) )
      {}

     protected:
      referenced::Pointer<Coupling> value;
    };

    template < typename Value >
    class Promise2 {
     public:
      template < typename Function >
      auto then( Function && function )
        -> Future2<typename function_traits<Function>::return_type>
      {
        using Result = typename function_traits<Function>::return_type;
        referenced::Pointer<Coupling> result{ new Statement<Function,Value,Result>{ std::forward<Function>( function ) } };
        value->chain( result );
        return result;
      }

      template < typename ...Args >
      void complete( Args && ... args )
      {
        value->template value<Value>().construct( std::forward<Args>( args )... );
        value->evaluate();
      }

      Promise2()
      : value( new Placeholder<Value>{} )
      {}

     protected:
      referenced::Pointer<Placeholder<Value>> value;
    };
  }
}
