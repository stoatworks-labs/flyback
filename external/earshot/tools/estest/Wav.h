#pragma once

// Minimal RIFF WAVE: reads PCM 16/24/32-bit and IEEE float 32/64 (any channel
// count, WAVE_FORMAT_EXTENSIBLE included); writes float 32.

#include "Harness.h"

#include <string>

namespace estest
{

bool readWav( const std::string& path, Signal& out, std::string& error );
bool writeWav( const std::string& path, const Signal& in, std::string& error );

} // namespace estest
