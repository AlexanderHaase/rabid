// Copyright (c) 2016, Alexander Haase
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#pragma once
#include <tuple>
#include <type_traits>
#include <cstddef>

namespace rabid {

  /// Tests if a type provides a SINGLE operator().
	///
	/// usage: is_functor<T>::value // true/false
	///
	/// If more than one operator() is provided, deduction will fail with a
	/// compile-time error.
	///
	/// @tparam T type to test.
	///
	template < typename T >
	struct is_functor
	{
		template < typename U >
		static auto test( std::nullptr_t ) -> decltype( &U::operator() );

		template < typename U>
		static auto test( ... ) -> std::false_type;

		static constexpr bool value = ! std::is_same< decltype( test<T>( nullptr ) ), std::false_type >::value;
	};
	
	/// Mix-in structure describing arguments and return type.
	///
	/// Provides argument types via an internal template.
	///
	/// @tparam R result type
	/// @tparam Args... parameter pack of argument types.
	///
	template < typename R, typename ... Args >
	struct arg_traits
	{
		using return_type = R;

		/// Number of arguments in function signature excluding 'this'.
		///
		static constexpr size_t nargs = sizeof...( Args );

		/// Template based access to argument types.
		///
		/// @tparam Index of argument to access.
		///
		template < size_t Index >
		struct args
		{
			static_assert( Index < sizeof...(Args), "Argument index out of range!" );
			using type = typename std::tuple_element<Index,std::tuple<Args...>>::type;
		};
	};

	/// Function trait detection, provided by specializations.
	///
	/// Specializations exist for:
	///
	///   - function pointers: function_traits< decltype( &someFunction ) >
	///   - member functions: function_traits< decltype( &SomeClass::someFunction) >
	///   - single functor types: function_traits< SomeFunctor >
	///
	/// Explicitly not supported:
	///
	///   - Functors providing multiple operator() definitions are 
	///     ambiguous and not deducible.
	///   - Variadic functions/members are not deducible by nature.
	///
	/// While usage with fully specified signatures is
	/// allowed, it makes little sense:
	///
	/// static_assert( std::is_same< Ret, function_traits< Ret (*)( Arg1, Arg2 ) >::return_type >::value );
	///
	/// @tparam type of function/callable object to evaluate.
	/// @tparam isFunctor boolean if T provides operator() (deduced).
	///
	template < typename T, bool isFunctor = is_functor<T>::value >
	struct function_traits_impl;

	/// Specialization for mutable member functions(member pointer to function).
	///
	/// Usage: function_traits< decltype( &SomeClass::someMethod ) >
	///
	/// @tparam R return type of function(deduced).
	/// @tparam Base class type containing function(deduced).
	/// @tparam Args parameter pack of function arguments(deduced).
	///
	template < typename R, typename Base, typename ...Args >
	struct function_traits_impl< R (Base::*)(Args...), false > : arg_traits< R, Args... >
	{
		using class_type = Base;
		using type = R (Base::*)(Args...);
    using signature = R (Args...);
	};

	/// Specialization for const member functions(member pointer to function).
	///
	/// Usage: function_traits< decltype( &SomeClass::someMethod ) >
	///
	/// @tparam R return type of function(deduced).
	/// @tparam Base class type containing function(deduced).
	/// @tparam Args parameter pack of function arguments(deduced).
	///
  template < typename R, typename Base, typename ...Args >
	struct function_traits_impl< R (Base::*)(Args...) const, false > : arg_traits< R, Args... >
	{
		using class_type = Base;
		using type = R (Base::*)(Args...) const;
    using signature = R (Args...);
	};

	/// Specialization for volatile member functions(member pointer to function).
	///
	/// Usage: function_traits< decltype( &SomeClass::someMethod ) >
	///
	/// @tparam R return type of function(deduced).
	/// @tparam Base class type containing function(deduced).
	/// @tparam Args parameter pack of function arguments(deduced).
	///
  template < typename R, typename Base, typename ...Args >
	struct function_traits_impl< R (Base::*)(Args...) volatile, false > : arg_traits< R, Args... >
	{
    using class_type = Base;
		using type = R (Base::*)(Args...) volatile;
    using signature = R (Args...);
	};

	/// Specialization for volatile member functions(member pointer to function).
	///
	/// Usage: function_traits< decltype( &SomeClass::someMethod ) >
	///
	/// @tparam R return type of function(deduced).
	/// @tparam Base class type containing function(deduced).
	/// @tparam Args parameter pack of function arguments(deduced).
	///
  template < typename R, typename Base, typename ...Args >
	struct function_traits_impl< R (Base::*)(Args...) const volatile, false > : arg_traits< R, Args... >
	{
    using class_type = Base;
		using type = R (Base::*)(Args...) const volatile;
    using signature = R (Args...);
	};

	/// Specialization for function pointers.
	///
	/// Usage: function_traits< decltype( &someFunction ) >
	///
	/// @tparam R return type of function(deduced).
	/// @tparam Args parameter pack of function arguments(deduced).
	///
	template < typename R, typename ...Args >
	struct function_traits_impl< R (*)(Args...), false > : arg_traits< R, Args... >
	{
		using type = R (*)(Args...);
    using signature = R (Args...);
	};

	/// Specialization for functors: Proxy to method pointer for operator().
	///
	/// Usage: function_traits< FunctorType >
	///
	/// @tparam T functor type.
	///
	template < typename T >
	struct function_traits_impl< T, true > : function_traits_impl< decltype( &T::operator() ), false > {};

	/// Function trait detection, provided by specializations.
	///
	/// Specializations exist for:
	///
	///   - function pointers: function_traits< decltype( &someFunction ) >
	///   - member functions: function_traits< decltype( &SomeClass::someFunction) >
	///   - single functor types: function_traits< SomeFunctor >
	///
	/// Explicitly not supported:
	///
	///   - Functors providing multiple operator() definitions are 
	///     ambiguous and not deducible.
	///   - Variadic functions/members are not deducible by nature.
	///
	/// While usage with fully specified signatures is
	/// allowed, it makes little sense:
	///
	/// static_assert( std::is_same< Ret, function_traits< Ret (*)( Arg1, Arg2 ) >::return_type >::value );
	///
	/// @tparam type of function/callable object to evaluate.
	/// @tparam isFunctor boolean if T provides operator() (deduced).
	///
	template < typename T >
	using function_traits = function_traits_impl< std::decay_t<T>, is_functor<std::decay_t<T>>::value >;

}
