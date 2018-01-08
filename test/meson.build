test_includes = include_directories( '../Catch/single_include/' )
test_sources = files( 'main.cpp',
  'unit_test_executor.cpp',
   )

test_exe = executable( 'all_tests', test_sources,
	include_directories : [ base_includes, test_includes ],
	cpp_args : cpp_flags )

test( 'combined tests', test_exe )
