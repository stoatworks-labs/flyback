#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

/**
	Small numeric helpers shared by the analysis modules.

	MSVC has no M_PI (atrac and pattern's first Windows CI runs broke on it),
	so the constant lives here, and every file includes <cmath> for itself.
*/
namespace earshot
{
namespace dsp
{

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

/// Below this every level reads as -200 dB, so a log never sees zero.
constexpr double kTiny = 1e-20;

inline double dbFromAmplitude( double a )
{
	return 20.0 * std::log10( std::max( a, kTiny ) );
}

inline double dbFromPower( double p )
{
	return 10.0 * std::log10( std::max( p, kTiny ) );
}

inline double clampd( double v, double lo, double hi )
{
	return std::min( hi, std::max( lo, v ) );
}

/// One-pole smoothing coefficient for a step of `dt` seconds and time constant
/// `tau`. A non-positive dt returns 0 -- the state does not move -- which is
/// the other half of the fleet's priming trap: dt = 0 must never snap a filter.
inline double onePoleAlpha( double dt, double tau )
{
	if( !( dt > 0.0 ) )
		return 0.0;
	if( !( tau > 0.0 ) )
		return 1.0;
	return 1.0 - std::exp( -dt / tau );
}

inline int nextPow2( int n )
{
	int p = 1;
	while( p < n )
		p <<= 1;
	return p;
}

/// Periodic Hann: the right form for analysing a continuous signal. The
/// symmetric form repeats its endpoint and biases every bin (spasis's note).
inline float hann( int i, int n )
{
	return (float)( 0.5 * ( 1.0 - std::cos( kTwoPi * (double)i / (double)n ) ) );
}

inline double midiFromHz( double hz )
{
	return hz > 0.0 ? 69.0 + 12.0 * std::log2( hz / 440.0 ) : 0.0;
}

inline double cents( double hz, double reference )
{
	return 1200.0 * std::log2( hz / reference );
}

} // namespace dsp
} // namespace earshot
