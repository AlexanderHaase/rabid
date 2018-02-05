project( 'c++ rabid', 'cpp', 
	version : '0.0.1', 
	license : 'BSD 3-clause' )

# TODO: add compiler version tests
#
cpp_flags = [ '-std=c++14',
	'-g',
	'-Wall',
	'-Wextra',
	'-Werror',
	'--pedantic',
	'-Wdouble-promotion',
	'-Wshadow',
	'-Wformat=2',
  '-flto', # Does not play well with clang
	'-Ofast'
  #'-O0'
  ]

cpp_lint_flags = [
	'-Wlogical-op',
	'-Wdouble-promotion',
	'-Wnull-dereference',
	'-Wduplicated-cond',
	'-Wduplicated-branches',
	'-Wrestrict'
  ]

cpp_other_flags = [
	'-Wuseless-cast'
  ]

cpp_compiler = meson.get_compiler('cpp')

foreach flag : cpp_lint_flags
  if cpp_compiler.has_argument( flag, name : flag )
    cpp_flags += [ flag ]
  endif
endforeach

foreach flag : cpp_other_flags
  if cpp_compiler.compiles(
    'struct Base { Base( int const & arg ); int value; }; struct Derived : Base { using Base::Base; }; static Derived test{0}; Base::Base( int const & arg ) : value( arg ) {} int main() { return 0; }',
      args : cpp_flags + [ flag ],
      name : flag )
    cpp_flags += [ flag ]
  endif
endforeach

base_includes = include_directories( '../', 'include' ) 
base_dependencies = dependency( 'threads' )

subdir( 'test' )

# TODO: Example program
#
comparison = executable( 'comparison', 'comparison.cpp', 
	include_directories : base_includes,
  dependencies: base_dependencies,
	cpp_args : cpp_flags )

