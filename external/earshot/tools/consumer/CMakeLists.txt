cmake_minimum_required( VERSION 3.16 )

# The consumer smoke test: earshot pulled in exactly as a plugin would, by
# add_subdirectory, into a MODULE (a plugin bundle / DLL), plus a little
# host that loads the module at run time and asks it for a pitch. verify.sh
# builds it universal and checks the module exports nothing of earshot's.

project( earshot_consumer LANGUAGES C CXX )

if( NOT EARSHOT_DIR )
	set( EARSHOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.. )
endif()
add_subdirectory( ${EARSHOT_DIR} earshot )

add_library( FakePlugin MODULE plugin.cpp )
target_link_libraries( FakePlugin PRIVATE earshot::earshot )
set_target_properties( FakePlugin PROPERTIES
	CXX_STANDARD 17
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON )

add_executable( consumer-host host.cpp )
target_link_libraries( consumer-host PRIVATE ${CMAKE_DL_LIBS} )
target_compile_definitions( consumer-host PRIVATE
	PLUGIN_PATH="$<TARGET_FILE:FakePlugin>" )
