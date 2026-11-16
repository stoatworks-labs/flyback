// --fft, --pitch, --partials

#include "Harness.h"
#include "Signals.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>

namespace estest
{

//---------------------------------------------------------------------------
// The FFT against a direct DFT in double.
//
// Tolerance: a radix-2 FFT in float has an RMS relative error of order
// eps * log2(n) (Higham, "Accuracy and Stability of Numerical Algorithms",
// 2nd ed., section 24.1); eps = 6e-8, so at n = 4096 that is ~7e-7. The check
// allows 1e-5 relative to the transform's RMS, about 15x that, and the
// in-check control (the inverse used as the forward, i.e. the twiddles
// conjugated) misses by order 1. Negative control: the twiddle table built
// by a float running-angle recurrence (spasis measured it and rejected it).
//---------------------------------------------------------------------------

void checkFft( Report& rep, const Run&, const Mutations& mut )
{
	earshot::Fft fft;
	fft.init( 4096, mut.fftFloatTwiddles );
	Rng rng( 7 );
	for( int n : { 16, 256, 4096 } )
	{
		std::vector< std::complex< float > > x( (size_t)n ), y;
		for( auto& v : x )
			v = { (float)rng.gauss(), (float)rng.gauss() };
		std::vector< std::complex< double > > ref( (size_t)n );
		for( int k = 0; k < n; ++k )
		{
			std::complex< double > acc = 0.0;
			for( int j = 0; j < n; ++j )
			{
				const double a = -2.0 * kPi * (double)( (long long)k * j % n ) / n;
				acc += std::complex< double >( x[ (size_t)j ] ) * std::complex< double >( std::cos( a ), std::sin( a ) );
			}
			ref[ (size_t)k ] = acc;
		}
		auto relErr = [ & ]( const std::vector< std::complex< float > >& got ) {
			double e = 0.0, r = 0.0;
			for( int k = 0; k < n; ++k )
			{
				e += std::norm( std::complex< double >( got[ (size_t)k ] ) - ref[ (size_t)k ] );
				r += std::norm( ref[ (size_t)k ] );
			}
			return std::sqrt( e / r );
		};
		y = x;
		fft.forward( y.data(), n );
		const double fwd = relErr( y );
		// Round trip.
		fft.inverse( y.data(), n );
		double rt = 0.0, rr = 0.0;
		for( int k = 0; k < n; ++k )
		{
			rt += std::norm( std::complex< double >( y[ (size_t)k ] ) / (double)n - std::complex< double >( x[ (size_t)k ] ) );
			rr += std::norm( std::complex< double >( x[ (size_t)k ] ) );
		}
		rt = std::sqrt( rt / rr );
		// Negative control: the conjugate transform must miss.
		y = x;
		fft.inverse( y.data(), n );
		const double neg = relErr( y );
		const bool   ok  = fwd < 1e-5 && rt < 1e-5 && neg > 0.1;
		rep.add( "fft.dft", format( "n=%d", n ), ok, "forward %.2e, round trip %.2e (limit 1e-5); conjugated control %.2f (must exceed 0.1)",
				 fwd, rt, neg );
		rep.figure( "fft relative error", std::max( fwd, rt ) );
	}
}

//---------------------------------------------------------------------------
// Pitch
//---------------------------------------------------------------------------

namespace
{
struct PitchStats
{
	int    frames = 0, voiced = 0;
	double maxAbsCents = 0.0, sumCents = 0.0;
	double sumConf = 0.0;
	double lastHz = 0.0;
};

/// Play a mono signal (as a stereo pair) and gather pitch statistics on frames
/// at or after `from` seconds, measured against `expectHz`.
PitchStats playPitch( Driver& d, const Signal& mono, double from, double expectHz )
{
	PitchStats st;
	d.play( widen( mono, 2 ), [ & ]( const Frame& f, double vt ) {
		if( vt < from )
			return;
		++st.frames;
		if( !f.pitch.voiced )
			return;
		++st.voiced;
		const double c = centsBetween( f.pitch.hz, expectHz );
		st.maxAbsCents = std::max( st.maxAbsCents, std::fabs( c ) );
		st.sumCents += c;
		st.sumConf += f.pitch.confidence;
		st.lastHz = f.pitch.hz;
	} );
	return st;
}
} // namespace

// Tolerances, and where they come from:
//
// pitch.sine, pitch.mf -- 1 cent. MPM's parabolic interpolation of a cosine
//   peak has a closed-form bias: with the true peak at offset d from the
//   sample, the vertex lands at tan(w d) cot(w / 2) / 2 where w = 2 pi / P,
//   at most 0.016 w^2 samples. At the shortest period (2 kHz at 44.1 kHz,
//   P = 22) that is 0.0013 samples, 0.1 cent. 1 cent is ten times that, the
//   headroom being for the windowed NSDF not being exactly a cosine; it is
//   also a tuner display's resolution.
// pitch.noise -- 5 cents: a pitch-to-colour mapping must not visibly wobble on
//   a held note, and 5 cents is the commonly cited just-noticeable difference
//   for pitch in the middle register.
//   The confidence must be S/(S+N) within 0.05, N being the noise inside the
//   pitch path's band (Pitch.h): the NSDF at the period is that ratio by
//   construction, so the 0.05 is the max-of-noise bias of picking the peak.
// pitch.residue -- 5 cents (the JND): tones of high harmonics only, whose
//   pitch is f0 though no partial is near it, and whose every partial lies
//   above pass two's band.
// pitch.octave, pitch.h2 -- 5 cents (the JND above). These exist to catch
//   octave errors, which miss by 1200; a sub-harmonic 26 dB down also nudges
//   the peak at T by a couple of cents, because the window does not hold a
//   whole number of its periods, so the pure-sine 1 cent does not apply.
// pitch.vibrato -- the estimate must lie within the instantaneous pitch's
//   range over the window that ends at the newest sample (plus 1 cent): MPM
//   describes its window, so this is the honest bound, and it is what fails
//   when the window lags (the latency control).

void checkPitch( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	Config            cfg;
	Driver            d( run, cfg, mut );

	//-- Pure sines across the range --------------------------------------
	{
		double worst = 0.0;
		int    unvoiced = 0, total = 0;
		std::string worstAt;
		for( double hz : { 40.0, 41.2, 55.0, 82.41, 110.0, 196.0, 261.63, 440.0, 466.16, 880.0, 1000.0, 1318.5, 1760.0,
						   1975.5, 2000.0 } )
		{
			Signal s = silence( run.fs, 0.40 );
			addSine( s, hz, 0.5, 0.3 + hz * 0.01 );
			const PitchStats st = playPitch( d, s, 0.15, hz );
			total += st.frames;
			unvoiced += st.frames - st.voiced;
			if( st.maxAbsCents > worst || st.voiced < st.frames )
			{
				worst   = std::max( worst, st.maxAbsCents );
				worstAt = format( "%.2f Hz", hz );
			}
		}
		rep.add( "pitch.sine", cell, unvoiced == 0 && worst <= 1.0, "15 sines 40 Hz-2 kHz: worst %.3f cents at %s (limit 1), %d of %d frames unvoiced",
				 worst, worstAt.c_str(), unvoiced, total );
		rep.figure( "pitch.sine worst |cents|", worst );
	}

	//-- Missing fundamental ----------------------------------------------
	{
		double worst = 0.0;
		int    bad = 0, total = 0;
		for( double f0 : { 50.0, 80.0, 100.0, 155.56, 220.0, 311.13 } )
		{
			const Signal     s  = harmonicTone( run.fs, 0.40, f0, { 0.0, 0.12, 0.12, 0.12, 0.12, 0.12 } );
			const PitchStats st = playPitch( d, s, 0.15, f0 );
			total += st.frames;
			bad += st.frames - st.voiced;
			worst = std::max( worst, st.voiced > 0 ? st.maxAbsCents : 1e9 );
		}
		rep.add( "pitch.mf", cell, bad == 0 && worst <= 1.0,
				 "harmonics 2-6 only, f0 50-311 Hz: worst %.3f cents from f0 (limit 1), %d of %d frames unvoiced", worst, bad,
				 total );
		rep.figure( "pitch.mf worst |cents|", worst );
	}

	//-- Residue pitch: only high harmonics -------------------------------
	{
		// High harmonics and nothing else: pass two cuts the band to 8 f0,
		// below every one of them, and must still agree with pass one.
		double worst = 0.0;
		int    bad = 0, total = 0;
		for( double f0 : { 100.0, 150.0, 200.0 } )
		{
			std::vector< double > amps( 12, 0.0 );
			for( int h = 9; h <= 12; ++h )
				amps[ (size_t)h - 1 ] = 0.1;
			if( f0 == 100.0 )   // harmonics 15-19: pass two's band (800 Hz) is two octaves short of them
			{
				amps.assign( 19, 0.0 );
				for( int h = 15; h <= 19; ++h )
					amps[ (size_t)h - 1 ] = 0.08;
			}
			const Signal s = harmonicTone( run.fs, 0.40, f0, amps );
			const PitchStats st = playPitch( d, s, 0.15, f0 );
			total += st.frames;
			bad += st.frames - st.voiced;
			worst = std::max( worst, st.voiced > 0 ? st.maxAbsCents : 1e9 );
		}
		rep.add( "pitch.residue", cell, bad == 0 && worst <= 5.0,
				 "harmonics 9-12 only (f0 150, 200 Hz) and 15-19 only (f0 100 Hz): worst %.3f cents from f0 (limit 5), %d of %d frames unvoiced", worst, bad, total );
	}

	//-- Octave traps ------------------------------------------------------
	{
		// A sub-harmonic 26 dB down: the signal really repeats at 2T, and the
		// NSDF peaks at exactly 1 there, 0.995 at T. MPM's first key maximum
		// within 0.9 picks T; a global maximum picks the octave below.
		double worst = 0.0;
		for( double f0 : { 110.0, 220.0, 440.0 } )
		{
			Signal s = silence( run.fs, 0.40 );
			addSine( s, f0, 0.5, 0.2 );
			addSine( s, f0 / 2.0, 0.025, 1.1 );
			const PitchStats st = playPitch( d, s, 0.15, f0 );
			worst = std::max( worst, st.voiced == st.frames ? st.maxAbsCents : 1e9 );
		}
		rep.add( "pitch.octave", cell, worst <= 5.0, "a sub-harmonic 26 dB down: worst %.2f cents from the main pitch (limit 5)", worst );
		rep.figure( "pitch.octave worst |cents|", worst );

		// A second harmonic 6 dB OVER the fundamental: NSDF at T/2 is
		// (4 - 1) / (4 + 1) = 0.6, under 0.9, so T still wins.
		worst = 0.0;
		for( double f0 : { 98.0, 196.0, 392.0 } )
		{
			const Signal     s  = harmonicTone( run.fs, 0.40, f0, { 0.2, 0.4 } );
			const PitchStats st = playPitch( d, s, 0.15, f0 );
			worst = std::max( worst, st.voiced == st.frames ? st.maxAbsCents : 1e9 );
		}
		rep.add( "pitch.h2", cell, worst <= 5.0, "second harmonic +6 dB over the fundamental: worst %.2f cents (limit 5)", worst );
	}

	//-- Vibrato -------------------------------------------------------------
	{
		int    inside = 0, total = 0;
		double worstOut = 0.0;
		for( double hz : { 220.0, 440.0 } )
		{
			std::vector< double > inst;
			const Signal          s = vibrato( run.fs, 1.2, hz, 50.0, 5.5, 0.5, &inst );
			const int             window = (int)std::ceil( 3.0 * run.fs / cfg.pitchMinHz );
			d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) {
				const double fed = d.fedSeconds();
				if( fed < 0.2 )
					return;
				const size_t end = (size_t)std::llround( fed * run.fs );
				double       lo = 1e9, hi = 0.0;
				for( size_t i = end > (size_t)window ? end - (size_t)window : 0; i < end && i < inst.size(); ++i )
				{
					lo = std::min( lo, inst[ i ] );
					hi = std::max( hi, inst[ i ] );
				}
				++total;
				if( !f.pitch.voiced )
				{
					worstOut = 1e9;
					return;
				}
				const double below = centsBetween( lo, f.pitch.hz );   // > 0 if pitch is under lo
				const double above = centsBetween( f.pitch.hz, hi );   // > 0 if pitch is over hi
				const double out   = std::max( below, above );
				if( out <= 1.0 )
					++inside;
				worstOut = std::max( worstOut, out );
			} );
		}
		rep.add( "pitch.vibrato", cell, inside == total && total > 0,
				 "+-50 cents at 5.5 Hz: %d of %d frames inside the window's instantaneous range (+1 cent); worst %.2f cents outside",
				 inside, total, std::max( 0.0, worstOut ) );
		rep.figure( "pitch.vibrato worst cents outside window range", std::max( 0.0, worstOut ) );
	}

	//-- Noise ---------------------------------------------------------------
	{
		double worst = 0.0, worstConf = 0.0;
		int    unvoiced = 0, total = 0;
		for( double snrDb : { 20.0, 10.0 } )
			for( double hz : { 110.0, 440.0, 1000.0 } )
			{
				Signal       s        = silence( run.fs, 0.6 );
				const double sigPower = 0.5 * 0.5 * 0.5;
				const double noiseRms = std::sqrt( sigPower / std::pow( 10.0, snrDb / 10.0 ) );
				addSine( s, hz, 0.5, 0.4 );
				addNoise( s, noiseRms, (uint64_t)( hz * 10 + snrDb ) );
				const PitchStats st = playPitch( d, s, 0.15, hz );
				total += st.frames;
				unvoiced += st.frames - st.voiced;
				worst = std::max( worst, st.maxAbsCents );
				rep.note( "%.0f dB SNR, %.0f Hz: worst %.2f cents, mean %.2f, %d/%d voiced", snrDb, hz, st.maxAbsCents,
						  st.voiced ? st.sumCents / st.voiced : 0.0, st.voiced, st.frames );
				// The NSDF sees the noise inside the pitch path's band: a 4th-order
				// Butterworth at 8 x the pitch (pass two) or 2.5 x pitchMaxHz,
				// whose equivalent noise bandwidth is fc (pi/8) / sin(pi/8) = 1.026 fc.
				const double fc1    = std::min( 2.5 * cfg.pitchMaxHz, 0.45 * run.fs );
				const double fc     = 8.0 * hz < 0.8 * fc1 ? 8.0 * hz : fc1;   // pass two's band (Pitch.h)
				const double inBand = noiseRms * noiseRms * 1.0262 * fc / ( 0.5 * run.fs );
				const double expect = sigPower / ( sigPower + inBand );
				if( st.voiced > 0 )
					worstConf = std::max( worstConf, std::fabs( st.sumConf / st.voiced - expect ) );
			}
		const bool ok = worst <= 5.0 && worstConf <= 0.05 && unvoiced == 0;
		rep.add( "pitch.noise", cell, ok, "sines in white noise at 20 and 10 dB SNR: worst %.2f cents (limit 5), confidence off S/(S+N) by %.3f (limit 0.05), %d of %d unvoiced",
				 worst, worstConf, unvoiced, total );
		rep.figure( "pitch.noise worst |cents|", worst );
		rep.figure( "pitch.noise worst confidence error", worstConf );
	}

	//-- Negative controls: things that are not a pitch ---------------------
	{
		int    voiced = 0, total = 0;
		Signal noise = silence( run.fs, 0.6 );
		addNoise( noise, 0.1, 99 );
		PitchStats st = playPitch( d, noise, 0.15, 440.0 );
		voiced += st.voiced;
		total += st.frames;
		Signal quiet = silence( run.fs, 0.4 );
		addSine( quiet, 440.0, std::pow( 10.0, -70.0 / 20.0 ) );   // under the -60 dB gate
		st = playPitch( d, quiet, 0.15, 440.0 );
		voiced += st.voiced;
		total += st.frames;
		st = playPitch( d, silence( run.fs, 0.4 ), 0.15, 440.0 );
		voiced += st.voiced;
		total += st.frames;
		rep.add( "pitch.unvoiced", cell, voiced <= total / 100, "white noise, a -70 dBFS sine under the gate, silence: %d of %d frames voiced (limit 1%%)",
				 voiced, total );
	}
}

//---------------------------------------------------------------------------
// Partials
//
// Tolerance: the bias of quadratic interpolation on a Hann window's log
// magnitude is a property of the window alone. It is computed here from the
// window's transform -- sample |W| at the three bins around a sine at every
// offset in [0, 0.5] bin, interpolate, compare -- and that worst case (plus
// 1e-4 bin and 0.002 dB for float arithmetic) is the tolerance. Partials
// hundreds of bins apart do not disturb each other measurably: Hann's
// sidelobes are below -100 dB that far out.
//---------------------------------------------------------------------------

namespace
{
struct QifftBias
{
	double freqBins = 0.0, ampDb = 0.0;
};

QifftBias hannQifftBias( int n )
{
	auto mag = [ n ]( double nu ) {
		// |sum_j w_j e^{-i 2 pi nu j / n}| for the periodic Hann window
		std::complex< double > acc = 0.0;
		for( int j = 0; j < n; ++j )
		{
			const double w = 0.5 - 0.5 * std::cos( 2.0 * kPi * j / n );
			const double a = -2.0 * kPi * nu * j / n;
			acc += w * std::complex< double >( std::cos( a ), std::sin( a ) );
		}
		return std::abs( acc );
	};
	const double peak = mag( 0.0 );
	QifftBias    b;
	for( int i = 0; i <= 50; ++i )
	{
		const double d  = 0.5 * i / 50.0;   // sine at bin 0 + d; samples at -1, 0, 1
		const double a  = std::log( mag( -1.0 - d ) );
		const double m  = std::log( mag( -d ) );
		const double c  = std::log( mag( 1.0 - d ) );
		const double dd = a - 2.0 * m + c;
		const double p  = 0.5 * ( a - c ) / dd;
		const double pk = m - 0.25 * ( a - c ) * p;
		b.freqBins = std::max( b.freqBins, std::fabs( p - d ) );
		b.ampDb    = std::max( b.ampDb, std::fabs( 20.0 / std::log( 10.0 ) * ( pk - std::log( peak ) ) ) );
	}
	return b;
}
} // namespace

void checkPartials( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	Driver            d( run, Config(), mut );
	// The window PartialTracker uses: the power of two nearest 170 ms.
	int n = 1;
	while( n < (int)( 0.17 * run.fs ) )
		n <<= 1;
	if( (double)n / ( 0.17 * run.fs ) > std::sqrt( 2.0 ) )
		n >>= 1;
	static std::map< int, QifftBias > cache;
	if( cache.find( n ) == cache.end() )
		cache[ n ] = hannQifftBias( n );
	const QifftBias bias  = cache[ n ];
	const double    binHz = (double)run.fs / n;
	const double    tolHz = ( bias.freqBins + 1e-4 ) * binHz;
	const double    tolDb = bias.ampDb + 0.002;

	{
		const double hz[ 3 ]  = { 440.0, 1234.5, 3000.7 };
		const double amp[ 3 ] = { 0.5, 0.25, 0.1 };
		Signal       s        = silence( run.fs, 0.6 );
		for( int i = 0; i < 3; ++i )
			addSine( s, hz[ i ], amp[ i ], 0.5 * i );
		double worstHz = 0.0, worstDb = 0.0;
		int    frames = 0, missing = 0;
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double vt ) {
			if( vt < 0.25 )
				return;
			++frames;
			for( int i = 0; i < 3; ++i )
			{
				// partials are sorted loudest first
				if( f.partialCount <= i )
				{
					++missing;
					continue;
				}
				worstHz = std::max( worstHz, std::fabs( f.partials[ i ].hz - hz[ i ] ) );
				worstDb = std::max( worstDb, std::fabs( f.partials[ i ].db - 20.0 * std::log10( amp[ i ] ) ) );
			}
		} );
		const bool ok = missing == 0 && worstHz <= tolHz && worstDb <= tolDb;
		rep.add( "partials.sines", cell, ok,
				 "3 sines: worst %.4f Hz (limit %.4f = Hann QIFFT bias %.4f bin), worst %.4f dB (limit %.4f), %d missing over %d frames",
				 worstHz, tolHz, bias.freqBins, worstDb, tolDb, missing, frames );
		rep.figure( "partials worst freq error (bins)", worstHz / binHz );
		rep.figure( "partials worst amplitude error (dB)", worstDb );
	}

	{
		// A harmonic tone labels its partials 1..6; an inharmonic one at 2.1 f0
		// (85 cents from the 2nd harmonic) must not be labelled.
		const double f0 = 196.0;
		Signal       s  = harmonicTone( run.fs, 0.6, f0, { 0.3, 0.15, 0.1, 0.075, 0.06, 0.05 } );
		addSine( s, 2.1 * f0, 0.04, 0.9 );
		int bad = 0, frames = 0, labelled = 0;
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double vt ) {
			if( vt < 0.25 )
				return;
			++frames;
			for( int i = 0; i < f.partialCount; ++i )
			{
				const earshot::Partial& p = f.partials[ i ];
				const double   ratio  = p.hz / f0;
				const bool     inharm = std::fabs( ratio - 2.1 ) < 0.02;
				const int      expect = inharm ? 0 : (int)std::lround( ratio );
				if( p.harmonic != expect )
					++bad;
				if( p.harmonic > 0 )
					++labelled;
			}
		} );
		rep.add( "partials.harmonic", cell, bad == 0 && labelled >= frames * 6, "harmonics 1-6 labelled, a partial at 2.1 f0 not: %d wrong labels over %d frames",
				 bad, frames );
	}
}

} // namespace estest
