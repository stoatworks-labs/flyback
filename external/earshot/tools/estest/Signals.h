#pragma once

// Synthetic signals for estest. Everything is deterministic: the noise comes
// from a fixed-seed splitmix64 through Box-Muller, not <random>, whose
// distributions differ between standard libraries (a check that passes on
// libc++ and fails on MSVC's library is not a check of earshot).

#include "Harness.h"

#include <cstdint>
#include <string>
#include <vector>

namespace estest
{

constexpr double kPi = 3.14159265358979323846;

struct Rng
{
	explicit Rng( uint64_t seed ) : s( seed ) {}
	uint64_t next();
	double   uniform();   ///< [0, 1)
	double   gauss();     ///< zero mean, unit variance
	uint64_t s;
	bool     haveSpare = false;
	double   spare     = 0.0;
};

Signal silence( int fs, double seconds, int channels = 1 );
/// Add a sine to every channel (or one, if channel >= 0) between start and end.
void addSine( Signal& s, double hz, double amp, double phase = 0.0, int channel = -1, double start = 0.0,
			  double end = 1e30 );
void addNoise( Signal& s, double rms, uint64_t seed, int channel = -1, double start = 0.0, double end = 1e30 );
/// Mono to n channels, the same samples on each.
Signal widen( const Signal& mono, int channels );
/// A harmonic tone: amps[k] is harmonic k + 1.
Signal harmonicTone( int fs, double seconds, double f0, const std::vector< double >& amps );
/// A sine with sinusoidal vibrato of +-cents at rateHz. instHz (if not null)
/// receives the instantaneous frequency of every sample.
Signal vibrato( int fs, double seconds, double hz, double cents, double rateHz, double amp,
				std::vector< double >* instHz );

/// A tone burst at `hz` from start to end. `attack` = 0 starts it with a step
/// (cosine phase: the worst-case click); > 0 a raised-cosine rise of that
/// length. `release` likewise at the end.
void addBurst( Signal& s, double hz, double amp, double start, double end, double attack, double release );

//-- Drums ------------------------------------------------------------------

void addKick( Signal& s, double t, double amp );
void addSnare( Signal& s, double t, double amp, Rng& rng );
void addClap( Signal& s, double t, double amp, Rng& rng );
void addHat( Signal& s, double t, double amp, Rng& rng );

struct Groove
{
	std::string name;
	double      bpm = 120;
	int         stepsPerBeat = 4;   ///< 4: sixteenths; 3: triplets
	std::string kick, snare, clap, hat;   ///< one bar, 'x' = hit, '.' = rest
	double      kickAmp = 0.8, snareAmp = 0.3, clapAmp = 0.25, hatAmp = 0.08;
	double      expected = 0.0;   ///< the notated tempo the check expects
};

const std::vector< Groove >& grooves();
Signal render( const Groove& g, int fs, double seconds, uint64_t seed = 1 );

} // namespace estest
