#pragma once
#include <array>
#include <atomic>
#include <type_traits>

namespace rabid {

  template < typename ... >
  using void_t = void;

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

  template < typename Src, typename Dst, typename = void_t<> >
  struct valid_static_cast : std::false_type {};

  template < typename Src, typename Dst >
  struct valid_static_cast<Src,Dst,void_t<decltype( static_cast<Dst>( std::declval<Src>() ) )>> : std::true_type {};

  template < typename Type, typename Result = typename std::underlying_type<Type>::type >
  Result underlying_cast( const Type & type ) { return static_cast<Result>( type ); }

  template < typename Type, int TagBits = Log2<alignof(Type)>::value >
  class TaggedPointer {
   public:
    static_assert( TagBits > 0, "Pointer Tagging requires at least one bit!" );

    static constexpr uintptr_t mask = uintptr_t(uintptr_t(1) << TagBits) - uintptr_t(1);

    TaggedPointer() = default;

    constexpr explicit TaggedPointer( Type * value_arg, uintptr_t tag_arg = 0 )
    : value( reinterpret_cast<uintptr_t>( value_arg ) | tag_arg )
    {}

    template < typename OtherType, size_t OtherBits,
      typename = std::enable_if_t<(OtherBits <= TagBits)>,
      typename = std::enable_if_t<(valid_static_cast<OtherType*,Type*>::value)> >
    constexpr TaggedPointer( const TaggedPointer<OtherType,OtherBits> & other )
    : TaggedPointer( static_cast<Type*>( other.get() ), other.tag() )
    {}

    template < typename OtherType, size_t OtherBits,
      typename = std::enable_if_t<(OtherBits <= TagBits)>,
      typename = std::enable_if_t<(valid_static_cast<OtherType*,Type*>::value)> >
    constexpr operator TaggedPointer<OtherType,OtherBits>() const { return TaggedPointer<OtherType,OtherBits>{ get(), tag() }; }

    void set( Type * value_arg, uintptr_t tag_arg = 0 ) { value = reinterpret_cast<uintptr_t>( value_arg ) | tag_arg; }

    constexpr Type * get() const { return reinterpret_cast<Type*>(value & ~mask); }

    template < typename TagType = uintptr_t >
    constexpr TagType tag() const { return static_cast<TagType>( value & mask ); }

    template < typename TagType = uintptr_t >
    void tag( TagType tag_arg ) { value = reinterpret_cast<uintptr_t>( get() ) | static_cast<uintptr_t>( tag_arg ); }

    constexpr Type * operator -> () const { return get(); }
    constexpr Type & operator * () const { return *get(); }

    TaggedPointer & operator = ( nullptr_t ) { value = reinterpret_cast<uintptr_t>( nullptr ); return *this; }

    template < typename OtherType, size_t OtherBits,
      typename = std::enable_if_t<(OtherBits <= TagBits)>,
      typename = std::enable_if_t<(valid_static_cast<OtherType*,Type*>::value)> >
    TaggedPointer & operator = ( const TaggedPointer<OtherType,OtherBits> & other )
    {
      set( static_cast<Type*>( other.get() ), other.tag() );
      return * this;
    }

    friend bool operator == ( const TaggedPointer & a, const Type & b ) { return a.get() == b; }
    friend bool operator == ( const Type & b, const TaggedPointer & a ) { return a.get() == b; }
    friend bool operator == ( const TaggedPointer & a, const TaggedPointer & b ) { return a.value == b.value; }

    friend bool operator != ( const TaggedPointer & a, const Type & b ) { return !(a == b); }
    friend bool operator != ( const Type & b, const TaggedPointer & a ) { return !(a == b); }
    friend bool operator != ( const TaggedPointer & a, const TaggedPointer & b ) { return !(a == b); }

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

      template < typename Prepare >
      void insert( const LinkPointer & link, Prepare && prepare )
      {
        insert( link, link, std::forward<Prepare>( prepare ) );
      }

      template < typename Prepare >
      void insert( const LinkPointer & first, const LinkPointer & tail, Prepare && prepare )
      {
        for(;;)
        {
          auto prior = head.load( std::memory_order_relaxed );
          tail->at( Dimension ) = prepare( prior );
          if( head.compare_exchange_strong( prior, first, std::memory_order_release ) )
          {
            break;
          }
        }
      }

     protected:
      std::atomic<LinkPointer> head{ LinkPointer{nullptr} };
    };
  }
}
