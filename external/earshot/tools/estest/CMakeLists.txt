# estest: the headless harness. Not part of the library a plugin links.
add_executable( estest
	main.cpp
	Harness.cpp
	Signals.cpp
	Wav.cpp
	CheckPitch.cpp
	CheckOnsets.cpp
	CheckThirds.cpp
	CheckTempo.cpp
	CheckStereoTap.cpp
	CheckCapture.cpp
)
# The harness reaches into src/ for the pieces it tests directly (the FFT, the
# ring, the filter bank's coefficients); a plugin only ever sees include/.
target_include_directories( estest PRIVATE ${PROJECT_SOURCE_DIR}/src )
target_link_libraries( estest PRIVATE earshot::earshot )
set_target_properties( estest PROPERTIES CXX_EXTENSIONS OFF )
if( MSVC )
	target_compile_options( estest PRIVATE /utf-8 /W3 )
	target_compile_definitions( estest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS )
else()
	target_compile_options( estest PRIVATE -Wall -Wextra -Wno-unused-parameter )
endif()
