#pragma once
#include "intrusive.h"
#include <algorithm>
#include <memory>

namespace rabid {

  template < size_t Amount >
  class Padding {
    std::uint8_t fill[ Amount ];
  };

  template <>
  class Padding<0> {};
  
  template < typename Type, size_t Alignment = std::min( size_t{128}, alignof( Type ) ) >
  struct alignas(Alignment) CacheAligned : public Type, private Padding< sizeof(Type) - Alignment > {};

  namespace interconnect {

    using Message = intrusive::Link< tagged_pointer_bits<3>::type >;

    using Buffer = CacheAligned< intrusive::Exchange<Message> >;

    using Batch = intrusive::List<Message>;

    class Connection {
     public:
      template < typename Prepare >
      void send( const Message::PointerType & first, const Message::PointerType & last, Prepare && prepare ) const
      {
        remote.insert( first, last, std::forward<Prepare>( prepare ) );
      }

      template < typename Prepare >
      void send( const Message::PointerType & message, Prepare && prepare ) const { remote.insert( message, std::forward<Prepare>( prepare ) ); }

      Batch receive( const Message::PointerType & message ) const { return local.clear( message ); }      
      
      constexpr Connection reverse() const { return Connection{ local, remote }; }

      constexpr Connection( Buffer & outbound, Buffer & inbound )
      : remote( outbound )
      , local( inbound )
      {}

     protected:
      Buffer & remote;
      Buffer & local;
    };

    template < typename AddressMap >
    class Node : protected AddressMap {
     public:
      const Connection & route( size_t index ) const { return connections[ AddressMap::operator()( index ) ]; }

      template < typename ...Args >
      Node( std::vector<Connection> connections_arg, Args && ... args )
      : AddressMap( std::forward<Args>( args )... )
      , connections( std::move( connections_arg ) )
      {}

      const std::vector<Connection> & all() const { return connections; }

     protected:
      std::vector<Connection> connections;
    };

    struct Identity {
      template < typename Type >
      const Type & operator() ( const Type & value ) const { return value; }
    };

    class Direct {
     public:
      using NodeType = Node<Identity>;
      const NodeType & node( size_t index ) const { return nodes[ index ]; }

      Direct( size_t count )
      : buffers( std::make_unique<Buffer[]>( (count -1 ) * count + count ) )
      {
        nodes.reserve( count );
        for( size_t node_index = 0; node_index < count; ++node_index )
        {
          std::vector<Connection> connections;
          connections.reserve( count );

          for( size_t index = 0; index < count; ++index )
          {
            connections.emplace_back(
              buffer_for_edge( node_index, index ),
              buffer_for_edge( index, node_index ) );
          }
          nodes.emplace_back( std::move( connections ) );
        }
      }
      
     protected:
      Buffer & buffer_for_edge( size_t src, size_t dst ) const
      {
        if( src == dst )
        {
          // There are SUM(1,...,num_nodes-1) buffer pairs and num_nodes
          // loopback buffers. Loopback buffers are placed after the buffer
          // pairs.
          //
          // SUM(1,...,num_nodes-1) * 2 + num_nodes
          //  == (num_nodes-1)*num_nodes/2 * 2 + num_nodes
          //  == num_nodes * num_nodes
          //
          const auto base = (nodes.capacity() - 2)*(nodes.capacity()-1);
          return buffers[ base + src ];
        }
        else
        {
          const auto low = std::min( src, dst );
          const auto high = std::max( src, dst );
          const auto offset = src > dst;

          // Buffer pairs are addressed as follows:
          //   - Low address selects row.
          //   - Each row has L * num_nodes - SUM(1,...,L) - L buffer pairs:
          //     - Each row has L + 1 fewer pairs than the prior:
          //       - L for the reflexive linking
          //       - 1 for the loopback buffer.
          //     - This can be shortened to: L * num_nodes - (L+3)*L/2 - 1
          //   - High address indexes the row.
          //   - (-1) offset for zero-based addressing.
          //
          const auto pair = low * nodes.capacity() - (( low + 3 ) * low )/ 2 - 1 + high;
          return buffers[ pair * 2 + offset ];
        }
      }

      std::vector<NodeType> nodes;
      std::unique_ptr<Buffer[]> buffers;
    };
  }
}
