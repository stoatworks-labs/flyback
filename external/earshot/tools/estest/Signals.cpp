#include "Signals.h"

#include <algorithm>
#include <cmath>

namespace estest
{

uint64_t Rng::next()
{
	uint64_t z = ( s += 0x9E3779B97F4A7C15ull );
	z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	return z ^ ( z >> 31 );
}

double Rng::uniform()
{
	return (double)( next() >> 11 ) * ( 1.0 / 9007199254740992.0 );
}

double Rng::gauss()
{
	if( haveSpare )
	{
		haveSpare = false;
		return spare;
	}
	double u = 0.0;
	while( u <= 1e-300 )
		u = uniform();
	const double v = uniform();
	const double r = std::sqrt( -2.0 * std::log( u ) );
	spare          = r * std::sin( 2.0 * kPi * v );
	haveSpare      = true;
	return r * std::cos( 2.0 * kPi * v );
}

Signal silence( int fs, double seconds, int channels )
{
	Signal s;
	s.fs       = fs;
	s.channels = channels;
	s.x.assign( (size_t)std::llround( seconds * fs ) * (size_t)channels, 0.0f );
	return s;
}

namespace
{
void span( const Signal& s, double start, double end, size_t& a, size_t& b )
{
	const size_t n = s.frames();
	a = (size_t)std::max( 0.0, std::ceil( start * s.fs ) );
	b = (size_t)std::min( (double)n, std::ceil( std::min( end, 1e12 ) * s.fs ) );
	a = std::min( a, n );
	b = std::max( a, b );
}
} // namespace

void addSine( Signal& s, double hz, double amp, double phase, int channel, double start, double end )
{
	size_t a, b;
	span( s, start, end, a, b );
	const double w = 2.0 * kPi * hz / s.fs;
	for( size_t i = a; i < b; ++i )
	{
		const float v = (float)( amp * std::sin( w * (double)i + phase ) );
		for( int c = 0; c < s.channels; ++c )
			if( channel < 0 || channel == c )
				s.x[ i * (size_t)s.channels + (size_t)c ] += v;
	}
}

void addNoise( Signal& s, double rms, uint64_t seed, int channel, double start, double end )
{
	Rng    rng( seed );
	size_t a, b;
	span( s, start, end, a, b );
	for( size_t i = a; i < b; ++i )
		for( int c = 0; c < s.channels; ++c )
			if( channel < 0 || channel == c )
				s.x[ i * (size_t)s.channels + (size_t)c ] += (float)( rms * rng.gauss() );
}

Signal widen( const Signal& mono, int channels )
{
	Signal s;
	s.fs       = mono.fs;
	s.channels = channels;
	s.x.resize( mono.frames() * (size_t)channels );
	for( size_t i = 0; i < mono.frames(); ++i )
		for( int c = 0; c < channels; ++c )
			s.x[ i * (size_t)channels + (size_t)c ] = mono.x[ i * (size_t)mono.channels ];
	return s;
}

Signal harmonicTone( int fs, double seconds, double f0, const std::vector< double >& amps )
{
	Signal s = silence( fs, seconds );
	for( size_t k = 0; k < amps.size(); ++k )
		if( amps[ k ] != 0.0 && f0 * (double)( k + 1 ) < 0.45 * fs )
			addSine( s, f0 * (double)( k + 1 ), amps[ k ], 0.7 * (double)k );
	return s;
}

Signal vibrato( int fs, double seconds, double hz, double cents, double rateHz, double amp, std::vector< double >* instHz )
{
	Signal s = silence( fs, seconds );
	if( instHz != nullptr )
		instHz->resize( s.frames() );
	double phase = 0.0;
	for( size_t i = 0; i < s.frames(); ++i )
	{
		const double t = (double)i / fs;
		const double f = hz * std::pow( 2.0, cents / 1200.0 * std::sin( 2.0 * kPi * rateHz * t ) );
		if( instHz != nullptr )
			( *instHz )[ i ] = f;
		s.x[ i ] = (float)( amp * std::sin( phase ) );
		phase += 2.0 * kPi * f / fs;
	}
	return s;
}

void addBurst( Signal& s, double hz, double amp, double start, double end, double attack, double release )
{
	size_t a, b;
	span( s, start, end, a, b );
	const double w = 2.0 * kPi * hz / s.fs;
	for( size_t i = a; i < b; ++i )
	{
		const double t   = (double)i / s.fs - start;
		const double toE = end - (double)i / s.fs;
		double       g   = 1.0;
		if( attack > 0.0 && t < attack )
			g *= 0.5 - 0.5 * std::cos( kPi * t / attack );
		if( release > 0.0 && toE < release )
			g *= 0.5 - 0.5 * std::cos( kPi * toE / release );
		// cosine phase: a step from zero to the peak at the start
		const float v = (float)( amp * g * std::cos( w * ( (double)i - (double)a ) ) );
		for( int c = 0; c < s.channels; ++c )
			s.x[ i * (size_t)s.channels + (size_t)c ] += v;
	}
}

//---------------------------------------------------------------------------

namespace
{
void addMono( Signal& s, size_t i, double v )
{
	if( i >= s.frames() )
		return;
	for( int c = 0; c < s.channels; ++c )
		s.x[ i * (size_t)s.channels + (size_t)c ] += (float)v;
}

/// One-pole high-pass then low-pass: crude, fixed, deterministic band-limiting.
struct Shaper
{
	double hpA = 0, lpA = 0, hpX = 0, hpY = 0, lpY = 0;
	Shaper( double fs, double hp, double lp )
	{
		hpA = std::exp( -2.0 * kPi * hp / fs );
		lpA = std::exp( -2.0 * kPi * lp / fs );
	}
	double operator()( double x )
	{
		hpY = hpA * ( hpY + x - hpX );
		hpX = x;
		lpY = lpY * lpA + hpY * ( 1.0 - lpA );
		return lpY;
	}
};
} // namespace

void addKick( Signal& s, double t, double amp )
{
	const size_t a     = (size_t)std::llround( t * s.fs );
	const size_t n     = (size_t)( 0.45 * s.fs );
	double       phase = 0.0;
	for( size_t k = 0; k < n; ++k )
	{
		const double tt  = (double)k / s.fs;
		const double f   = 48.0 + 110.0 * std::exp( -tt / 0.025 );
		const double env = std::exp( -tt / 0.22 ) * std::min( 1.0, tt / 0.001 );
		addMono( s, a + k, amp * env * std::sin( phase ) );
		phase += 2.0 * kPi * f / s.fs;
	}
}

void addSnare( Signal& s, double t, double amp, Rng& rng )
{
	const size_t a = (size_t)std::llround( t * s.fs );
	const size_t n = (size_t)( 0.3 * s.fs );
	Shaper       sh( s.fs, 900.0, 7000.0 );
	for( size_t k = 0; k < n; ++k )
	{
		const double tt   = (double)k / s.fs;
		const double body = 0.5 * std::sin( 2.0 * kPi * 185.0 * tt ) * std::exp( -tt / 0.06 );
		const double rasp = 2.5 * sh( rng.gauss() ) * std::exp( -tt / 0.09 );
		addMono( s, a + k, amp * ( body + rasp ) * std::min( 1.0, tt / 0.0005 ) );
	}
}

void addClap( Signal& s, double t, double amp, Rng& rng )
{
	const size_t a = (size_t)std::llround( t * s.fs );
	const size_t n = (size_t)( 0.25 * s.fs );
	Shaper       sh( s.fs, 700.0, 5000.0 );
	for( size_t k = 0; k < n; ++k )
	{
		const double tt = (double)k / s.fs;
		// three quick slaps, then the room
		double env = std::exp( -tt / 0.08 );
		for( int j = 0; j < 3; ++j )
		{
			const double d = tt - 0.009 * j;
			if( d >= 0.0 && d < 0.009 )
				env = std::max( env, std::exp( -d / 0.003 ) );
		}
		addMono( s, a + k, amp * 3.0 * env * sh( rng.gauss() ) );
	}
}

void addHat( Signal& s, double t, double amp, Rng& rng )
{
	const size_t a = (size_t)std::llround( t * s.fs );
	const size_t n = (size_t)( 0.08 * s.fs );
	Shaper       sh( s.fs, 7000.0, 0.45 * s.fs );
	for( size_t k = 0; k < n; ++k )
	{
		const double tt = (double)k / s.fs;
		addMono( s, a + k, amp * 3.0 * sh( rng.gauss() ) * std::exp( -tt / 0.02 ) );
	}
}

const std::vector< Groove >& grooves()
{
	// One bar each. Levels are deliberately unequal: pattern's v0.1.0 read 62
	// on a four-on-the-floor whose kick dwarfed its clap, so the kick here is
	// 10 dB over the clap.
	static const std::vector< Groove > g = {
		{ "4otf-125", 125, 4, "x...x...x...x...", "", "....x.......x...", "..x...x...x...x.", 0.8, 0.3, 0.25, 0.08, 125 },
		{ "kick-128", 128, 4, "x...x...x...x...", "", "", "", 0.8, 0.3, 0.25, 0.08, 128 },
		{ "break-96", 96, 4, "x.........x.....", "....x.......x...", "", "x.x.x.x.x.x.x.x.", 0.8, 0.35, 0.25, 0.06, 96 },
		{ "dnb-174", 174, 4, "x.........x.....", "....x.......x...", "", "x.x.x.x.x.x.x.x.", 0.8, 0.35, 0.25, 0.06, 174 },
		{ "shuffle-110", 110, 3, "x.....x.....", "...x.....x..", "", "x.xx.xx.xx.x", 0.8, 0.3, 0.25, 0.07, 110 },
	};
	return g;
}

Signal render( const Groove& g, int fs, double seconds, uint64_t seed )
{
	Signal       s = silence( fs, seconds );
	Rng          rng( seed );
	const double step = 60.0 / g.bpm / g.stepsPerBeat;
	const size_t bar  = std::max( { g.kick.size(), g.snare.size(), g.clap.size(), g.hat.size(), (size_t)1 } );
	for( int i = 0;; ++i )
	{
		const double t = 0.05 + i * step;   // a little lead-in
		if( t >= seconds )
			break;
		const size_t at  = (size_t)i % bar;
		auto         hit = [ & ]( const std::string& p ) { return at < p.size() && p[ at ] == 'x'; };
		if( hit( g.kick ) )
			addKick( s, t, g.kickAmp );
		if( hit( g.snare ) )
			addSnare( s, t, g.snareAmp, rng );
		if( hit( g.clap ) )
			addClap( s, t, g.clapAmp, rng );
		if( hit( g.hat ) )
			addHat( s, t, g.hatAmp, rng );
	}
	return s;
}

} // namespace estest
