cmake_minimum_required( VERSION 3.16 )

# earshot: the shared live-audio front end for the Stoatworks FFGL plugins.
# One target, `earshot` (alias `earshot::earshot`). A plugin pulls it in with
#
#   add_subdirectory( external/earshot )
#   target_link_libraries( MyPlugin PRIVATE earshot::earshot )
#
# and nothing else: no install step, no find_package, no generated files.

set( EARSHOT_TOP_LEVEL OFF )
if( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR )
	set( EARSHOT_TOP_LEVEL ON )
	# Universal by default only when earshot is the project. As a subdirectory
	# the plugin's own CMAKE_OSX_ARCHITECTURES decides, as it must.
	if( APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES )
		set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" )
	endif()
	if( APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET )
		set( CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS deployment target" )
	endif()
	if( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
		set( CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE )
	endif()
endif()

project( earshot VERSION 0.1.0 LANGUAGES C CXX )

option( EARSHOT_CAPTURE     "Build the miniaudio device backend (off: feed() and the host FFT only)" ON )
option( EARSHOT_BUILD_TOOLS "Build estest and the consumer smoke test" ${EARSHOT_TOP_LEVEL} )

find_package( Threads REQUIRED )

add_library( earshot STATIC
	src/Capture.cpp
	src/Engine.cpp
	src/Fft.cpp
	src/HostFft.cpp
	src/Onsets.cpp
	src/Partials.cpp
	src/Pitch.cpp
	src/Stereo.cpp
	src/Tap.cpp
	src/Tempo.cpp
	src/ThirdOctave.cpp
)
add_library( earshot::earshot ALIAS earshot )

target_include_directories( earshot
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src )
target_compile_features( earshot PUBLIC cxx_std_17 )
set_target_properties( earshot PROPERTIES
	CXX_EXTENSIONS OFF
	POSITION_INDEPENDENT_CODE ON   # it ends up inside a plugin bundle or DLL
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON )
target_link_libraries( earshot PUBLIC Threads::Threads )

if( MSVC )
	target_compile_options( earshot PRIVATE /utf-8 /W4 /permissive- )
	target_compile_definitions( earshot PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS )
else()
	target_compile_options( earshot PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter )
endif()

if( EARSHOT_CAPTURE )
	target_sources( earshot PRIVATE src/Miniaudio.cpp src/MiniaudioImpl.cpp )
	target_include_directories( earshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio )
	target_compile_definitions( earshot PRIVATE EARSHOT_CAPTURE=1 )
	# miniaudio is vendored as is; its warnings are not ours to fix.
	if( MSVC )
		set_source_files_properties( src/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "/W0" )
	else()
		set_source_files_properties( src/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-w" )
	endif()
	if( APPLE )
		# miniaudio links CoreAudio at run time on most platforms, but on Apple
		# it needs these at link time. PUBLIC, because a static library's
		# frameworks must reach the plugin's own link line.
		target_link_libraries( earshot PUBLIC
			"-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" )
	elseif( UNIX )
		target_link_libraries( earshot PUBLIC ${CMAKE_DL_LIBS} m )
	endif()
else()
	target_sources( earshot PRIVATE src/NoCapture.cpp )
endif()

if( EARSHOT_BUILD_TOOLS )
	add_subdirectory( tools/estest )
endif()
