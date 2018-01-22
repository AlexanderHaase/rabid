#pragma once

#include "function_traits.h"

namespace rabid {

  namespace detail {

    /// Provides captured values to enable continuations and futures.
    ///
    /// Capture provides the following uniform interface:
    ///   - construct( ... ) construct the captured type from arguments.
    ///   - capture( func, args... ) capture the result of the expression.
    ///   - destruct() call the captured object's destructor.
    ///   - value() access the captured value by reference.
    ///
    /// A void specialization is provided for capturing void results.
    ///
    template <typename Type>
    class Captured {
     public:
      template < typename ...Args >
      void construct( Args && ...args )
      {
        new (&storage) Type{ std::forward<Args>( args )... };
      }

      template < typename Function, typename ... Args >
      void capture( Function && function, Args && ... args )
      {
        new (&storage) Type{ function( std::forward<Args>( args )... ) };
      }

      Type & value() { return *reinterpret_cast<Type*>( &storage ); }
      const Type & value() const { return *reinterpret_cast<Type*>( &storage ); }

      void destruct() { value().~Type(); }
     protected:
      std::aligned_storage_t< sizeof(Type), alignof(Type)> storage;
    };

    /// Specialization for void--omits value() method.
    ///
    /// Provides uniform semantics regardless of captured type.
    ///
    template <>
    class Captured<void> {
     public:
      template < typename ...Args >
      void construct( Args && ...args )
      {}

      template < typename Function, typename ... Args >
      void capture( Function && function, Args && ... args )
      {
        static_assert( std::is_same<typename function_traits<Function>::return_type, void>::value, "Attempt to capture non-void expression" );
        function( std::forward<Args>( args )... );
      }

      void destruct() {}
    };

    /// Evaluate and capture the result of an expression(continuation style).
    ///
    /// Specialization for functions taking one argument.
    ///
    template <typename Function,
      typename ReturnType,
      typename ArgType,
      typename Traits = function_traits<Function> >
    auto apply( Function && function, Captured<ReturnType> & result, Captured<ArgType> & arg )
      -> std::enable_if_t<Traits::nargs==1>
    {
      result.capture( std::forward<Function>( function ), arg.value() );
    }

    /// Evaluate and capture the result of an expression(continuation style).
    ///
    /// Specialization for functions taking zero arguments.
    ///
    template <typename Function,
      typename ReturnType,
      typename Traits = function_traits<Function> >
    auto apply( Function && function, Captured<ReturnType> & result, Captured<void> & )
      -> std::enable_if_t<Traits::nargs==0>
    {
      result.capture( std::forward<Function>( function ) );
    }
  }
}
