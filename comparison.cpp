
#include <limits>
#include <unordered_map>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "include/Executor.h"

using namespace rabid;

class MappedFile {
 public:
  MappedFile() = default;

  template < typename ...Args>
  MappedFile( Args && ...args )
  {
    open( std::forward<Args>( args )... );
  }

  ~MappedFile() { close(); }

  MappedFile( const MappedFile & ) = delete;
  MappedFile( MappedFile && other )
  : base( other.base )
  , bytes( other.bytes )
  {
    other.base = nullptr;
    other.bytes = 0;
  }

  MappedFile & operator = ( const MappedFile & ) = delete;
  MappedFile & operator = ( MappedFile && other )
  {
    close();
    base = other.base;
    bytes = other.bytes;
    other.base = nullptr;
    other.bytes = 0;
    return *this;
  }

  bool open( const std::string & path, size_t offset = 0, size_t length = std::numeric_limits<size_t>::max() )
  {
    int fd = ::open( path.c_str(), O_RDONLY );
    if( fd >= 0 )
    {
      bool result = open( fd, offset, length );
      ::close( fd ); 
      return result;
    }
    return false;
  }

  bool open( const int fd, size_t offset = 0, size_t length = std::numeric_limits<size_t>::max() )
  {
      struct stat stat;
      if( 0 == ::fstat( fd, &stat ) )
      {
        offset = std::min( offset, size_t(stat.st_size) );
        length = std::min( length, size_t(stat.st_size) );

        close();

        base = ::mmap( nullptr, length, PROT_READ, MAP_SHARED, fd, offset );
        if( base == MAP_FAILED )
        {
          base = nullptr;
        }
        else
        {
          bytes = length;
          return true;
        }
      }
      return false;
  }

  void close()
  {
    if( base )
    {
      ::munmap( base, bytes );
      base = nullptr;
      bytes = 0;
    }
  }

  size_t warm() const
  {
    size_t total;
    for( size_t index = 0; index < size<uint8_t>(); ++index )
    {
      total += array<uint8_t>()[ index ];
    }
    return total;
  }

  bool empty() const { return base == nullptr || bytes == 0; }

  template < typename T >
  size_t size() const { return bytes/sizeof(T); }

  template < typename T >
  const T * array() const { return reinterpret_cast<const T*>( base ); }

 protected:
  void * base = nullptr;
  size_t bytes = 0;
};

template < typename CharT, typename Traits = std::char_traits<CharT> >
struct Token {
  const CharT * begin;
  const CharT * end;

  size_t size() const { return end - begin; }

  friend bool operator == ( const Token & a, const Token & b )
  {
    return a.size() != b.size() || Traits::compare( a.begin, b.begin, a.size() ) == 0;
  } 

  friend std::ostream & operator << ( std::ostream & stream, const Token & token )
  {
    if( token.begin != token.end )
    {
      stream.write( token.begin, token.end - token.begin );
    }
    return stream;
  }
};


template < typename CharT, typename Traits = std::char_traits<CharT> >
class Tokenizer {
 public:

  bool empty() const { return begin == end; }

  Token<CharT> next()
  {
    Token<CharT> result{};

    result.begin = begin;

    while( begin != end && !std::isspace(Traits::to_int_type(*begin)) )
    {
      begin += 1;
    }
    result.end = begin;

    while( begin != end && std::isspace(Traits::to_int_type(*begin)) )
    {
      begin += 1;
    }

    return result;
  }

  Tokenizer( const CharT * array, size_t size )
  : begin( array )
  , end( array + size )
  {
    while( begin != end && std::isspace(Traits::to_int_type(*begin)) )
    {
      begin += 1;
    }
  }
 protected:
  const CharT * begin;
  const CharT * const end;
};

namespace std {

  template < typename T >
  size_t hash_combine( size_t seed, const T & value )
  {
    return (seed << 1) ^ hash<T>{}( value );
  }

  template < typename CharT >
  struct hash<Token<CharT>>
  {
    size_t operator() ( const Token<CharT> & token ) const
    {
      size_t seed = 0;
      for( auto current = token.begin; current != token.end; ++current )
      {
        seed = hash_combine( seed, *current );
      }
      return seed;
    }
  };
}

int main( int, char ** argv )
{
  MappedFile file{ argv[ 1 ] };

  file.warm();

  size_t count = 0;
  struct Freq {
    size_t count = 0;
  };
  Tokenizer<char> tokenizer( file.array<char>(), file.size<char>() );
  std::unordered_map<Token<char>,Freq> counts;
  while( !tokenizer.empty() )
  {
    const auto token = tokenizer.next();
    counts[ token ].count += 1;
    count += 1;
  }

  for( auto & pair : counts )
  {
    std::cout << "'" << pair.first << "': " << pair.second.count << std::endl;
  }

  std::cout << file.size<uint8_t>() << " " << count << std::endl;
  return 0;
}
