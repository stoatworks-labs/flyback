// miniaudio is compiled here and only here (spasis's arrangement). It is a
// four-megabyte single header; keeping it out of every other translation unit
// is the point. Compiled as C++ because that is the form spasis has shipped on
// macOS and Windows. Warnings are switched off for this file in CMakeLists.txt.
#include "MiniaudioConfig.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
