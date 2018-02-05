#pragma once

#include <type_traits>
#include <atomic>

#include "function_traits.h"

namespace rabid {

  namespace referenced {
    template < typename Base, typename Deleter = std::default_delete<Base> >
    class Object {
     public:
      friend void acquire( Base * base )
      { 
        if( base )
        {
          base->references.fetch_add( 1, std::memory_order_relaxed );
        }
      }
      friend void release( Base * base )
      {
        if( base && 1 == base->references.fetch_add( -1, std::memory_order_relaxed ) )
        { 
          Deleter{}( base );
        }
      }
     protected:
      std::atomic<size_t> references{ 0 };
    };

    template < typename Base >
    class Pointer {
     public:
      void usurp( Base * other )
      {
        release( object );
        object = other;
      }

      Base * leak()
      {
        auto result = object;
        object = nullptr;
        return result;
      }

      Pointer( Base * const other )
      {
        acquire( other );
        object = other;
      }

      Pointer() = default;

      Pointer( const Pointer & other )
      : object( other.object )
      { acquire( object ); }

      Pointer( Pointer && other )
      : object( other.object )
      { other.object = nullptr; }

      Pointer & operator = ( Base * const other )
      {
        acquire( other );
        release( object );
        object = other;
        return *this;
      }

      Pointer & operator = ( const Pointer & other )
      {
        acquire( other.object );
        auto tmp = other.object;
        release( object );
        object = tmp;
        return *this;
      }

      Pointer & operator = ( Pointer && other )
      {
        auto tmp = other.object;
        other.object = nullptr;
        release( object );
        object = tmp;
        return *this;
      }

      operator Base * () const { return object; }

      Base * get() const { return object; }
      Base * operator ->() const { return object; }
      Base & operator * () const { return *object; }
      operator bool() const { return object != nullptr; }

      ~Pointer() { release( object ); }
     protected:
      Base * object = nullptr;
    };
  }
}
