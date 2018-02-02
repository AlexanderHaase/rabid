#pragma once

#include "container.h"
#include "../referenced.h"

namespace rabid {

  namespace detail {

    // Current draft of lock-free futures/continuations operate with two
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
    //  Thread A:
    //     expression[thread A]<Y> -> result<T>
    //                                   |->expression[thread B]<T> -> ...
    //                                   |->expression[thread C]<T> -> ...
    //                                   *->expression[thread D]<T> -> ...
    //
    // After evaluation, dependant expressions are prepared for dispatch:
    //
    //   Thread A:
    //     expression[thread A]<Y> -> result<T>
    //                                  ^^^
    //                                  |||
    //          ((reference to b)) ==>  ||*-expression[thread B]<T> -> ...
    //          ((reference to c)) ==>  |*--expression[thread C]<T> -> ...
    //          ((reference to d)) ==>  *---expression[thread D]<T> -> ...
    //
    // Then dispatched and evaluated (potentially concurrently):
    //
    //                                result<T>
    //                                  ^^^
    //   Thread B:                      |||
    //          ((reference to b)) ==>  ||*-expression[thread B]<T> -> ...
    //                                  ||
    //   Thread C:                      ||
    //          ((reference to c)) ==>  |*--expression[thread C]<T> -> ...
    //                                  |
    //   Thread D:                      |
    //          ((reference to d)) ==>  *---expression[thread D]<T> -> ...
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
    namespace expression {

      /// Get the offset of the first member of a subclass.
      ///
      /// Glancing at the C++17 standard, 13.1.3 says that memory layout with
      /// inheritance is per composition. Note that structs/classes/unions
      /// by default inherit the largest alignment of their members and are
      /// sized as a multiple of their alignment for array packing purposes.
      /// That means that subclasses may not use padding at the end of the base
      /// class for their members, but instead use memory after the class.
      ///
      /// This asks the compiler to generate a pointer to the first member of
      /// a subclass. Since the compiler will synthesize the same offset
      /// for subclasses with identical first member types, any subclass may be
      /// used to obtain the offset. Since the pointer points to an instance of
      /// the member type, it is not undefined behavior to use it.
      ///
      /// Note: in multiple inheritance, the order of composition is not
      /// specifed by the standard.
      ///
      template < class Base, class Member >
      class first_subclass_member {
       public:
        static constexpr Member * cast( Base * base )
        {
          return &static_cast<Prototype*>( base )->member;
        }
       private:
        struct Prototype : Base { Member member; };
      };

      struct ImmediateDispatch
      {
        template < typename T >
        friend void dispatch( T && t )
        {
          t->evaluate();
        }
      };

      template < typename Dispatch >
      class Expression : public referenced::Object<Expression<Dispatch>>, public Dispatch {
       public:
        /// Destructor, implementations clears result<T> and function<Y>
        ///
        virtual ~Expression() = default;

        /// Evaluation, implementations chain from argument to result.
        ///
        virtual void evaluate() = 0;

        /// Chain a coupling after this one.
        ///
        void chain( referenced::Pointer<Expression> && expression )
        {
          for(;;)
          {
            Expression * prior = pending.load( std::memory_order_relaxed );
            if( prior == sentinel() )
            {
              expression->variable = this;
              dispatch( expression );
              break;
            }
            expression->variable.usurp( prior );
            if( pending.compare_exchange_strong( prior, expression, std::memory_order_relaxed ) )
            {
              expression.leak();
              break;
            }
            else
            {
              expression->variable.leak();
            }
          }
        }

        void chain( const referenced::Pointer<Expression> & expression )
        {
          chain( referenced::Pointer<Expression>{ expression } );
        }

        /// Uniform location access for container.
        ///
        template < typename Value >
        Container<Value> & value()
        {
          return *first_subclass_member<Expression,Container<Value>>::cast( this );
        }

       protected:
        /// Head of linked list of dependant expressions
        ///
        std::atomic<Expression*> pending{ nullptr };

        /// Dual use pointer:
        ///  - Element for participating in linked lists.
        ///  - Points to argument of evaluation.
        ///
        referenced::Pointer<Expression> variable;

        /// Sentinel value for linked list
        ///
        Expression * sentinel() { return this; }

        /// Function for evaluating all pending couplings.
        ///
        void complete()
        {
          referenced::Pointer<Expression> waiting;
          waiting.usurp( pending.exchange( sentinel(), std::memory_order_acquire ) );
          while( waiting )
          {
            auto next = std::move( waiting->variable );
            waiting->variable = this;
            dispatch( std::move( waiting ) );
            waiting = std::move( next );
          }
        }
      };

      template < typename Dispatch, typename Function, typename Arg, typename Result >
      class Continuation final : public Expression<Dispatch> {
       public:
        virtual ~Continuation()
        {
          Super * remainder = pending.load( std::memory_order_relaxed );
          if( remainder == sentinel() )
          {
            container.destruct();
          }
          else
          {
            release( remainder );
          }
        }

        virtual void evaluate( void ) override
        {
          apply( function, container, variable->template value<Arg>() );
          complete();
        }

        template < typename ...Args >
        Continuation( Args && ...args )
        : function( std::forward<Args>( args )... )
        {}

       protected:
        using Super = Expression<Dispatch>;
        using Super::pending;
        using Super::sentinel;
        using Super::variable;
        using Super::complete;

        Container<Result> container;
        Function function;
      };
      
      template < typename Dispatch, typename Result >
      class Argument final : public Expression<Dispatch> {
       public:
        virtual ~Argument()
        {
          Super * remainder = pending.load( std::memory_order_relaxed );
          if( remainder == sentinel() )
          {
            container.destruct();
          }
          else
          {
            release( remainder );
          }
        }

        virtual void evaluate( void ) override
        {
          complete();
        }

       protected:
        using Super = Expression<Dispatch>;
        using Super::pending;
        using Super::sentinel;
        using Super::complete;
        Container<Result> container;
      };
    }
  }
}
