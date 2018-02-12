
#include <limits>
#include <unordered_map>
#include <iostream>
#include <chrono>

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

        base = ::mmap( nullptr, length, PROT_READ, MAP_SHARED, fd, off_t(offset) );
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

  size_t size() const { return size_t(end - begin); }

  size_t bucket( size_t concurrency ) const { return (size_t(*begin) + size()) % concurrency; }

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

struct Freq {
  size_t count = 0;
};


template <typename CharT>
auto freq_with_executor( const MappedFile & file,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;
  const auto stride = file.size<CharT>() / jobs;

  using FreqMap = std::unordered_map<Token<CharT>,Freq>;
  const auto map = std::make_unique<FreqMap[]>( concurrency );

  using rabid::detail::Join;

  struct Job {
    Token<CharT> token;
    Tokenizer<CharT> tokenizer;
    FreqMap * buckets;
    Join & join;

    void operator() ()
    {
      buckets[ Exec::current() ][ token ].count += 1;
      if( !tokenizer.empty() )
      {
        token = tokenizer.next();
        Exec::async( token.bucket( Exec::concurrency() ), Job{*this} );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    Tokenizer<CharT> tokenizer{ file.array<CharT>() + job * stride, stride };
    executor.inject( job % concurrency, Job{ tokenizer.next(), tokenizer, map.get(), join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

template <typename CharT>
auto freq_with_executor2( const MappedFile & file,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  using Exec = rabid::Executor<rabid::interconnect::Direct, rabid::execution::ThreadModel >;
  Exec executor{ concurrency };

  const auto jobs = concurrency * jobs_multiplier;
  const auto stride = file.size<CharT>() / jobs;

  using FreqMap = std::unordered_map<Token<CharT>,Freq>;
  const auto map = std::make_unique<FreqMap[]>( concurrency );

  using rabid::detail::Join;

  struct Job {
    Token<CharT> token;
    Tokenizer<CharT> tokenizer;
    FreqMap * buckets;
    Join & join;

    void operator() ()
    {
      buckets[ Exec::current() ][ token ].count += 1;
      if( !tokenizer.empty() )
      {
        token = tokenizer.next();
        Exec::defer( token.bucket( Exec::concurrency() ) );
      }
      else
      {
        join.notify();
      }
    }
  };

  Join join{ ssize_t(jobs) };
  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < jobs; ++job )
  {
    Tokenizer<CharT> tokenizer{ file.array<CharT>() + job * stride, stride };
    executor.inject( job % concurrency, Job{ tokenizer.next(), tokenizer, map.get(), join } );
  }

  join.wait();
  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

template <typename CharT>
class Bucket {
 public:
  template < typename Function >
  void apply( const Token<CharT> & token, Function && function )
  {
    std::unique_lock<std::mutex> lock( mutex );
    function( map[ token ] );
  }
 protected:
  std::mutex mutex;
  std::unordered_map<Token<CharT>,Freq> map;
};

template <typename CharT>
auto freq_with_threads( const MappedFile & file,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  std::vector<std::thread> threads;
  threads.reserve( concurrency );

  const auto buckets = std::make_unique<Bucket<CharT>[]>( concurrency );

  struct State {
    const MappedFile & file;
    const std::unique_ptr<Bucket<CharT>[]> buckets;
    const size_t jobs_multiplier;
    const size_t concurrency;
    const size_t stride;
  };

  const auto jobs = jobs_multiplier * concurrency;

  State state{ file,
    std::make_unique<Bucket<CharT>[]>( concurrency ),
    jobs_multiplier,
    concurrency,
    file.size<CharT>() / jobs };

  struct Job {
    size_t index;
    State & state;

    void operator()()
    {
      for( size_t job = 0; job < state.jobs_multiplier; ++job )
      {
        size_t offset = (index * state.jobs_multiplier + job) * state.stride;
        Tokenizer<CharT> tokenizer{ state.file.template array<CharT>() + offset, state.stride };
        while( !tokenizer.empty() )
        {
          auto token = tokenizer.next();
          auto bucket = token.bucket( state.concurrency );
          state.buckets[ bucket ].apply( token, []( Freq & freq ) { freq.count += 1; } );
        }
      }
    }
  };

  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < concurrency; ++job )
  {
    threads.emplace_back( Job{ job, state } );
  }

  for( auto & thread : threads )
  {
    thread.join();
  }

  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

template <typename CharT>
auto freq_with_threads2( const MappedFile & file,
  size_t jobs_multiplier = 1,
  size_t concurrency = std::thread::hardware_concurrency() )
  -> std::chrono::steady_clock::duration
{
  std::vector<std::thread> threads;
  threads.reserve( concurrency );

  const auto buckets = std::make_unique<Bucket<CharT>[]>( concurrency );

  struct State {
    const MappedFile & file;
    const std::unique_ptr<Bucket<CharT>[]> buckets;
    const size_t jobs_multiplier;
    const size_t concurrency;
    const size_t stride;
  };

  const auto jobs = jobs_multiplier * concurrency;

  State state{ file,
    std::make_unique<Bucket<CharT>[]>( concurrency ),
    jobs_multiplier,
    concurrency,
    file.size<CharT>() / jobs };

  struct Job {
    size_t index;
    State & state;

    void operator()()
    {
      for( size_t job = 0; job < state.jobs_multiplier; ++job )
      {
        size_t offset = (index * state.jobs_multiplier + job) * state.stride;
        Tokenizer<CharT> tokenizer{ state.file.template array<CharT>() + offset, state.stride };
        while( !tokenizer.empty() )
        {
          auto token = tokenizer.next();
          state.buckets[ index ].apply( token, []( Freq & freq ) { freq.count += 1; } );
        }
      }
    }
  };

  const auto begin = std::chrono::steady_clock::now();

  for( size_t job = 0; job < concurrency; ++job )
  {
    threads.emplace_back( Job{ job, state } );
  }

  for( auto & thread : threads )
  {
    thread.join();
  }

  const auto end = std::chrono::steady_clock::now();
  return end - begin;
}

int main( int argc, char ** argv )
{
  MappedFile file{ argv[ 1 ] };

  std::cout << "Warmed up: " << file.warm() << std::endl;

  const size_t concurrency = ( argc > 3 ? strtoul( argv[ 3 ], nullptr, 10 ) : std::thread::hardware_concurrency() );
  const size_t job_multipler = ( argc > 2 ? strtoul( argv[ 2 ], nullptr, 10 ) : concurrency * concurrency );

  {
    const auto duration = freq_with_executor<char>( file, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  {
    const auto duration = freq_with_executor2<char>( file, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  {
    const auto duration = freq_with_threads<char>( file, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }
  {
    const auto duration = freq_with_threads2<char>( file, job_multipler, concurrency );
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>( duration ).count() << " usec" << std::endl;
  }

  return 0;
}
