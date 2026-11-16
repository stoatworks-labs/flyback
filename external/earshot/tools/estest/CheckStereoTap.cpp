// --stereo, --tap

#include "Harness.h"
#include "Signals.h"

#include "Stereo.h"

#include <algorithm>
#include <cmath>

namespace estest
{

using earshot::Stereo;

//---------------------------------------------------------------------------
// Stereo. The meter reads the newest window of the pair (2048 samples at both
// rates). Tones are put on an integer number of cycles in that window, so the
// mean square of a sine is exactly A^2 / 2 wherever the window falls, and the
// deterministic claims can be tight:
//
// stereo.correlation -- identical sides +1, inverted -1, sine against cosine
//   0, each within 1e-5 (float accumulation over 2048 samples, done in
//   double: the residue is the float input's rounding). Independent noise
//   on each side: |r| <= 4 / sqrt(N) (the sample correlation of independent
//   white noise has standard deviation 1 / sqrt(N)). Negative control:
//   sum(LR) / N, not normalised.
// stereo.balance -- left at 0.5, right at 0.25: +6.0206 dB within 0.001 dB,
//   rmsL and rmsR within 1e-5 of A / sqrt 2. Negative control: sides swapped.
// stereo.width -- mono 0, anti-phase 1, hard pan 0.5 (1e-5); independent
//   noise 0.5 within 4 / sqrt(N) (width = (1 - r) / 2 for equal levels).
//   Negative control: side over mid energy.
// stereo.pair -- four channels, pairFirst = 2: the meter reads channels 2
//   and 3 (distinct tones on 0..3), so the balance and levels are 2's and
//   3's. Negative control: sides swapped.
// stereo.mono -- a one-channel stream: mono set, correlation 1, width 0,
//   balance 0. Negative controls: not normalised; metered as left with a
//   silent right.
//---------------------------------------------------------------------------

namespace
{
Stereo meterAfter( const Run& run, const Signal& s, const Mutations& mut, const Config& cfg = Config() )
{
	Driver d( run, cfg, mut );
	d.play( s, []( const Frame&, double ) {} );
	return d.engine().frame().stereo;
}

Signal pair( int fs, double seconds, double hzL, double ampL, double phaseL, double hzR, double ampR, double phaseR )
{
	Signal s = silence( fs, seconds, 2 );
	if( ampL != 0.0 )
		addSine( s, hzL, ampL, phaseL, 0 );
	if( ampR != 0.0 )
		addSine( s, hzR, ampR, phaseR, 1 );
	return s;
}
} // namespace

void checkStereo( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const int         n    = earshot::StereoMeter::windowFor( run.fs );
	const double      f    = run.fs * 20.0 / n;   // 20 whole cycles in the window
	const double      stat = 4.0 / std::sqrt( (double)n );
	const double      pi2  = 0.5 * kPi;

	//-- correlation ---------------------------------------------------------
	{
		const Stereo same = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.5, 0.0 ), mut );
		const Stereo inv  = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.5, kPi ), mut );
		const Stereo quad = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.5, pi2 ), mut );
		Signal       nz   = silence( run.fs, 0.5, 2 );
		addNoise( nz, 0.2, 41, 0 );
		addNoise( nz, 0.2, 42, 1 );
		const Stereo ind = meterAfter( run, nz, mut );
		const bool   ok  = std::fabs( same.correlation - 1.0 ) < 1e-5 && std::fabs( inv.correlation + 1.0 ) < 1e-5 &&
						std::fabs( quad.correlation ) < 1e-5 && std::fabs( ind.correlation ) <= stat;
		rep.add( "stereo.correlation", cell, ok,
				 "same %+.6f, inverted %+.6f, quadrature %+.6f (each within 1e-5 of +1, -1, 0); independent noise %+.4f (limit +-%.4f)",
				 same.correlation, inv.correlation, quad.correlation, ind.correlation, stat );
	}

	//-- balance and levels --------------------------------------------------
	{
		const Stereo s  = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.25, 0.3 ), mut );
		const double bw = 20.0 * std::log10( 2.0 );
		const bool   ok = std::fabs( s.balanceDb - bw ) < 1e-3 && std::fabs( s.rmsL - 0.5 / std::sqrt( 2.0 ) ) < 1e-5 &&
						std::fabs( s.rmsR - 0.25 / std::sqrt( 2.0 ) ) < 1e-5;
		rep.add( "stereo.balance", cell, ok, "left 0.5, right 0.25: balance %+.4f dB (want %+.4f), rms %.6f / %.6f (want %.6f / %.6f)",
				 s.balanceDb, bw, s.rmsL, s.rmsR, 0.5 / std::sqrt( 2.0 ), 0.25 / std::sqrt( 2.0 ) );
	}

	//-- width ---------------------------------------------------------------
	{
		const Stereo mono = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.5, 0.0 ), mut );
		const Stereo anti = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.5, kPi ), mut );
		const Stereo pan  = meterAfter( run, pair( run.fs, 0.5, f, 0.5, 0.0, f, 0.0, 0.0 ), mut );
		Signal       nz   = silence( run.fs, 0.5, 2 );
		addNoise( nz, 0.2, 43, 0 );
		addNoise( nz, 0.2, 44, 1 );
		const Stereo ind = meterAfter( run, nz, mut );
		const bool   ok  = std::fabs( mono.width ) < 1e-5 && std::fabs( anti.width - 1.0 ) < 1e-5 && std::fabs( pan.width - 0.5 ) < 1e-5 &&
						std::fabs( ind.width - 0.5 ) <= stat;
		rep.add( "stereo.width", cell, ok, "mono %.6f, anti-phase %.6f, hard pan %.6f (each within 1e-5 of 0, 1, 0.5); independent noise %.4f (limit 0.5 +- %.4f)",
				 mono.width, anti.width, pan.width, ind.width, stat );
	}

	//-- the pair out of four channels ----------------------------------------
	{
		Signal s = silence( run.fs, 0.5, 4 );
		const double amps[ 4 ] = { 0.4, 0.3, 0.2, 0.05 };
		for( int c = 0; c < 4; ++c )
			addSine( s, f * ( 1 + c ), amps[ c ], 0.0, c );
		Config cfg;
		cfg.maxChannels = 4;
		cfg.pairFirst   = 2;
		const Stereo st = meterAfter( run, s, mut, cfg );
		const double bw = 20.0 * std::log10( amps[ 2 ] / amps[ 3 ] );
		const bool   ok = std::fabs( st.balanceDb - bw ) < 1e-3 && std::fabs( st.rmsL - amps[ 2 ] / std::sqrt( 2.0 ) ) < 1e-5 &&
						std::fabs( st.rmsR - amps[ 3 ] / std::sqrt( 2.0 ) ) < 1e-5;
		rep.add( "stereo.pair", cell, ok, "4 channels, pairFirst 2: rms %.5f / %.5f (want %.5f / %.5f), balance %+.3f dB (want %+.3f)",
				 st.rmsL, st.rmsR, amps[ 2 ] / std::sqrt( 2.0 ), amps[ 3 ] / std::sqrt( 2.0 ), st.balanceDb, bw );
	}

	//-- a mono stream -------------------------------------------------------
	{
		Signal s = silence( run.fs, 0.5, 1 );
		addSine( s, f, 0.5 );
		const Stereo st = meterAfter( run, s, mut );
		const bool   ok = st.mono && std::fabs( st.correlation - 1.0 ) < 1e-5 && std::fabs( st.width ) < 1e-5 && std::fabs( st.balanceDb ) < 1e-4;
		rep.add( "stereo.mono", cell, ok, "one channel: mono %s, correlation %+.6f, width %.6f, balance %+.5f dB", st.mono ? "yes" : "NO",
				 st.correlation, st.width, st.balanceDb );
	}
}

//---------------------------------------------------------------------------
// Tap. The stream at Config::tapRate (8 kHz), read after every update.
//
// tap.contiguous -- a 440 Hz sine (left) and a 610 Hz cosine (right) through
//   the engine; the whole collected stream must equal A sin(2 pi f k / R)
//   sample for sample from k = 0 (after the first kernel width), within
//   2e-4 A: the Kaiser kernel's passband ripple is 10^(-81/20) = 9e-5
//   (Kaiser's A = beta / 0.1102 + 8.7 dB at beta 8), the 512-point table's
//   linear interpolation adds under 1e-5, and float the rest. A sample
//   dropped or repeated anywhere is a phase step of 2 pi f / R = 0.35 rad,
//   an error of order 0.3 A. The count must be every sample the fed audio
//   allows (within one kernel half-width). Negative control: hand out the
//   newest window on each read instead of the stream.
// tap.alias -- sines at 0.55, 0.7 and 1.3 tapRate (above the tap's Nyquist):
//   what reaches the tap is at least 75 dB down (the 81 dB stopband less
//   6 dB for the table and float). Negative control: no anti-alias filter.
// tap.passband -- 100, 1000 and 2000 Hz: gain within 0.005 dB of unity
//   (ripple 9e-5 is 0.0008 dB; 2 kHz is 0.25 R, inside the passband, which
//   ends near 0.3 R for this kernel). Negative control: the cutoff at a
//   quarter of the tap rate.
//---------------------------------------------------------------------------

namespace
{
struct TapRun
{
	std::vector< float > l, r;
	double               fed = 0.0;
	int                  rate = 0;
};

TapRun tapOf( const Run& run, const Signal& s, const Mutations& mut )
{
	Driver d( run, Config(), mut );
	d.collectTap = true;
	d.play( s, []( const Frame&, double ) {} );
	TapRun t;
	t.l    = d.tapL;
	t.r    = d.tapR;
	t.fed  = d.fedSeconds();
	t.rate = d.engine().tapRate();
	return t;
}

double rmsFrom( const std::vector< float >& x, size_t from )
{
	double acc = 0.0;
	size_t n   = 0;
	for( size_t i = from; i < x.size(); ++i, ++n )
		acc += (double)x[ i ] * x[ i ];
	return n > 0 ? std::sqrt( acc / (double)n ) : 0.0;
}
} // namespace

void checkTap( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const double      A    = 0.5;

	//-- contiguous ----------------------------------------------------------
	{
		Signal s = silence( run.fs, 2.0, 2 );
		addSine( s, 440.0, A, 0.0, 0 );
		addSine( s, 610.0, A, 0.5 * kPi, 1 );
		const TapRun t    = tapOf( run, s, mut );
		const double R    = t.rate;
		const size_t skip = (size_t)std::ceil( 2.0 * 10.0 / ( 2.0 * 0.4 ) ) + 2;   // a kernel width at the tap rate (25 samples)
		double       worst = 0.0;
		size_t       worstAt = 0;
		for( size_t k = skip; k < t.l.size(); ++k )
		{
			const double el = std::fabs( t.l[ k ] - A * std::sin( 2.0 * kPi * 440.0 * (double)k / R ) );
			const double er = std::fabs( t.r[ k ] - A * std::sin( 2.0 * kPi * 610.0 * (double)k / R + 0.5 * kPi ) );
			if( std::max( el, er ) > worst )
			{
				worst   = std::max( el, er );
				worstAt = k;
			}
		}
		const double expect = t.fed * R;                 // samples the audio covers
		const double slack  = 10.0 / ( 2.0 * 0.4 ) + 2;  // the kernel's look-ahead, at the tap rate
		const bool   countOk = (double)t.l.size() <= expect + 1.0 && (double)t.l.size() >= expect - slack;
		rep.add( "tap.contiguous", cell, worst <= 2e-4 * A && countOk && t.l.size() == t.r.size(),
				 "%zu samples at %d Hz from %.3f s of audio (want %.0f..%.0f); worst error %.2e (limit %.1e) at k = %zu", t.l.size(), t.rate,
				 t.fed, expect - slack, expect, worst, 2e-4 * A, worstAt );
		rep.figure( "tap worst sample error re amplitude", worst / A );
	}

	//-- alias ---------------------------------------------------------------
	{
		double      worstDb = -400.0;
		std::string detail;
		for( double ratio : { 0.55, 0.7, 1.3 } )
		{
			const double hz = ratio * 8000.0;
			if( hz >= 0.45 * run.fs )
				continue;
			Signal s = silence( run.fs, 1.0, 2 );
			addSine( s, hz, A );
			const TapRun t  = tapOf( run, s, mut );
			const double db = 20.0 * std::log10( std::max( 1e-20, rmsFrom( t.l, 200 ) ) / ( A / std::sqrt( 2.0 ) ) );
			worstDb         = std::max( worstDb, db );
			detail += format( " %.0f Hz: %.1f dB;", hz, db );
		}
		rep.add( "tap.alias", cell, worstDb <= -75.0, "above the tap's Nyquist, what reaches it (limit -75 dB):%s", detail.c_str() );
		rep.figure( "tap alias rejection, worst (dB)", worstDb );
	}

	//-- passband ------------------------------------------------------------
	{
		double      worst = 0.0;
		std::string detail;
		for( double hz : { 100.0, 1000.0, 2000.0 } )
		{
			// A whole number of cycles in the measured stretch of the tap.
			Signal s = silence( run.fs, 1.5, 2 );
			addSine( s, hz, A );
			const TapRun t = tapOf( run, s, mut );
			const size_t from = 1000, cycles = (size_t)( ( t.l.size() - from ) * hz / t.rate );
			const size_t len  = (size_t)std::llround( cycles * t.rate / hz );
			std::vector< float > seg( t.l.begin() + (long)from, t.l.begin() + (long)( from + len ) );
			const double db = 20.0 * std::log10( rmsFrom( seg, 0 ) / ( A / std::sqrt( 2.0 ) ) );
			worst           = std::max( worst, std::fabs( db ) );
			detail += format( " %.0f Hz: %+.4f dB;", hz, db );
		}
		rep.add( "tap.passband", cell, worst <= 0.005, "gain (limit +-0.005 dB):%s", detail.c_str() );
	}
}

} // namespace estest
