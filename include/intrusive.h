#pragma once
#include <array>
#include <atomic>

namespace rabid {

  template < typename Container, typename Member >
  constexpr uintptr_t offset_of( Member Container::* pointer )
  {
    return static_cast<uintptr_t>( &(static_cast<Container*>(0)->*pointer) );
  }

  template < typename Container, typename Member >
  constexpr Container & container_of( Member & member, Member Container::* relationship )
  {
    return *static_cast<Container*>( static_cast<uintptr_t>( &member ) - offset_of( relationship ) );
  }

  template < size_t Number >
  struct Log2 : std::integral_constant<std::size_t, Log2<(Number >> 1)>::value + 1 > {};
 

  template <>
  struct Log2<1> : std::integral_constant<std::size_t, 0> {};

  template < typename Type, int TagBits = Log2<alignof(Type)>::value >
  class TaggedPointer {
   public:
    static_assert( TagBits > 0, "Pointer Tagging requires at least one bit!" );

    static constexpr uintptr_t mask = uintptr_t(uintptr_t(1) << TagBits) - uintptr_t(1);

    constexpr explicit TaggedPointer( Type * value_arg, uintptr_t tag_arg = 0 )
    : value( static_cast<uintptr_t>( value_arg ) | tag_arg )
    {}

    void set( Type * value_arg, uintptr_t tag_arg = 0 ) { value =  static_cast<uintptr_t>( value_arg ) | tag_arg; }

    constexpr Type * get() const { return static_cast<Type*>(value & ~mask); }

    constexpr uintptr_t tag() const { return value & mask; }
    void tag( uintptr_t tag_arg ) { value = static_cast<uintptr_t>( get() ) | tag_arg; }

    constexpr Type * operator -> () const { return get(); }
    constexpr Type & operator * () const { return *get(); }

    TaggedPointer & operator = ( Type * value_arg )
    {
      set( value_arg );
      return * this;
    }

   protected:
    uintptr_t value;
  };

  template < typename Type >
  struct add_tagged_pointer { using type = TaggedPointer<Type>; };

  template < typename Type >
  using add_tagged_pointer_t = typename add_tagged_pointer<Type>::type;


  template < size_t TagBits >
  struct tagged_pointer_bits {
    template < typename Type >
    using type = TaggedPointer<Type,TagBits>;
  };

  namespace intrusive {

    template< size_t Connectivity, template< typename > class Pointer = std::add_pointer_t >
    class Link {
     public:
      using PointerType = Pointer<Link>;

      template < typename Container >
      Container & container( Link Container::* pointer )
      {
        return container_of( this, pointer );
      }

      template < typename Container >
      Container & container( Link Container::* pointer ) const
      {
        return container_of( this, pointer );
      }

      Link* & at( size_t index )
      {
        return links[ index ];
      }

      Link* at( size_t index ) const
      {
        return links[ index ];
      }

     protected:
      std::array<PointerType, Connectivity> links;
    };

    template < typename Link, size_t Dimension = 0>
    class List {
     public:
      using LinkType = Link;
      using LinkPointer = typename LinkType::PointerType;

      void insert( const LinkPointer & link )
      {
        insert( link, link );
      }

      void insert( const LinkPointer & first, const LinkPointer & last )
      {
        last->at( Dimension ) = head;
        head = first;
      }

      LinkPointer remove()
      {
        const auto result = head;
        head = result->at( Dimension );
        return result;
      }

      LinkPointer begin() const { return head; }

      bool empty() { return head == nullptr; }
      void clear() { head = nullptr; }

      List() = default;
      explicit List( const LinkPointer & head_arg )
      : head( head_arg )
      {}

      List( List && other )
      : head( std::move( other.head ) )
      { other.head = nullptr; }

      List( const List & ) = delete;

      List & operator = ( List && other )
      {
        head = std::move( other.head );
        other.head = nullptr;
        return *this;
      }

      List & operator = ( const List & ) = delete;

     protected:
      LinkPointer head = nullptr;
    };

    template < typename Link, size_t Dimension = 0>
    class Exchange {
     public:
      using LinkType = Link;
      using LinkPointer = typename LinkType::PointerType;

      List<Link,Dimension> clear( const LinkPointer & value = LinkPointer{ nullptr } )
      {
        return List<Link,Dimension>{ head.exchange( value, std::memory_order_acq_rel ) };
      }

      void insert( const LinkPointer & link )
      {
        insert( link, link );
      }

      void insert( const LinkPointer & first, const LinkPointer & tail )
      {
        for(;;)
        {
          auto prior = head.load( std::memory_order_relaxed );
          tail->at( Dimension ) = prior;
          if( head.compare_exchange_strong( prior, first, std::memory_order_release ) )
          {
            break;
          }
        }
      }

     protected:
      std::atomic<LinkPointer> head{ {nullptr} };
    };
  }
}
