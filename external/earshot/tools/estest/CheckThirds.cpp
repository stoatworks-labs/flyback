// --thirds: the third-octave bank against ISO 266 and IEC 61260 class 1.

#include "Harness.h"
#include "Signals.h"

#include "ThirdOctave.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace estest
{
namespace
{
using earshot::ThirdOctaveBank;
using cd = std::complex< double >;

const double kG = std::pow( 10.0, 0.3 );   // IEC 61260-1 base-ten octave ratio

/// |H| of a band's cascade at frequency f (Hz), from its coefficients.
double magnitude( const ThirdOctaveBank::BandFilter& b, double f, double fs )
{
	const cd z1 = std::polar( 1.0, -2.0 * kPi * f / fs );
	const cd z2 = z1 * z1;
	cd       h  = 1.0;
	for( int i = 0; i < b.sections; ++i )
	{
		const auto& s = b.sec[ i ];
		h *= ( s.b0 + s.b1 * z1 + s.b2 * z2 ) / ( 1.0 + s.a1 * z1 + s.a2 * z2 );
	}
	return std::abs( h );
}

/*
	IEC 61260 class 1 limits on relative attenuation (dB re the attenuation at
	the exact mid-band frequency), for an octave filter at normalised frequency
	Omega = G^x. Source: IEC 61260:1995, table 1, as reproduced in IS 6964
	(Bureau of Indian Standards), the freely readable copy of that table.
	IEC 61260-1:2014 restates the limits; its text is not freely available and
	was not consulted, so this harness claims the 1995 class 1 mask, no more.

	A fractional-octave filter (1/b octave) uses the same limits at the mapped
	frequencies Omega_b = 1 + ((G^(1/2b) - 1) / (G^(1/2) - 1)) (Omega - 1)
	above mid-band and 1/Omega_b below (the 1995 standard's breakpoint mapping).
	Between breakpoints the limits are interpolated linearly in lg Omega: the
	standard tabulates breakpoints, and straight lines on a log-frequency
	axis between them is this harness's reading, stated as an assumption.
*/
struct Breakpoint
{
	double x;      ///< Omega = G^x for the octave filter
	double minDb;  ///< attenuation at least
	double maxDb;  ///< attenuation at most (infinity outside the band)
};

const double kInf = std::numeric_limits< double >::infinity();

// The edge (x = 1/2) is a step: just inside it the minimum is -0.3, at it +2.0.
const Breakpoint kInside[] = {
	{ 0.0, -0.3, 0.3 }, { 0.125, -0.3, 0.4 }, { 0.25, -0.3, 0.6 }, { 0.375, -0.3, 1.3 }, { 0.5, -0.3, 5.0 },
};
const Breakpoint kOutside[] = {
	{ 0.5, 2.0, kInf }, { 1.0, 17.5, kInf }, { 2.0, 42.0, kInf }, { 3.0, 61.0, kInf }, { 4.0, 70.0, kInf },
};

double mapped( double x, int b )
{
	const double ratio = ( std::pow( kG, 1.0 / ( 2.0 * b ) ) - 1.0 ) / ( std::pow( kG, 0.5 ) - 1.0 );
	return 1.0 + ratio * ( std::pow( kG, x ) - 1.0 );
}

/// The class 1 limits at normalised frequency omega (f / fm) for 1/b octave.
void mask( double omega, int b, double& minDb, double& maxDb )
{
	const double w    = omega >= 1.0 ? omega : 1.0 / omega;
	const double edge = mapped( 0.5, b );
	auto interp = [ & ]( const Breakpoint* p, int n ) {
		for( int i = 0; i + 1 < n; ++i )
		{
			const double a = mapped( p[ i ].x, b ), c = mapped( p[ i + 1 ].x, b );
			if( w <= c )
			{
				const double t = ( std::log10( w ) - std::log10( a ) ) / ( std::log10( c ) - std::log10( a ) );
				minDb          = p[ i ].minDb + t * ( p[ i + 1 ].minDb - p[ i ].minDb );
				maxDb          = std::isinf( p[ i ].maxDb ) ? kInf : p[ i ].maxDb + t * ( p[ i + 1 ].maxDb - p[ i ].maxDb );
				return;
			}
		}
		minDb = p[ n - 1 ].minDb;
		maxDb = p[ n - 1 ].maxDb;
	};
	if( w < edge * ( 1.0 - 1e-12 ) )
		interp( kInside, 5 );
	else if( w <= edge * ( 1.0 + 1e-12 ) )
	{
		minDb = 2.0;
		maxDb = 5.0;
	}
	else
		interp( kOutside, 5 );
}

/// ISO 266 R10 labels, 20 Hz .. 20 kHz.
const float kNominal[ 31 ] = { 20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
							   800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };

} // namespace

/*
	thirds.centre -- the bank's bands are IEC 61260-1's base-ten mid-bands,
	fm = 1000 * 10^(x/10), to 1e-9 relative (double arithmetic), labelled with
	ISO 266's R10 numbers, and as many as fit under 0.95 of Nyquist (30 at
	44.1 kHz, 31 at 48 kHz). Read from the Frame, so through the engine.
	Negative control: G = 2 (base two), which moves 20 Hz by 1.3 %.

	thirds.edge -- the -3.01 dB points of each digital filter land on its IEC
	band edges fm G^(+-1/6). A Butterworth band-pass is exactly 1/sqrt(2) of
	its centre gain at its analog edges, and the bilinear transform is exact at
	pre-warped frequencies, so the only error is rounding: limit 1e-6 dB.
	Negative control: no pre-warp (the 20 kHz band's edges move by 15 %).

	thirds.mask -- each band's analytic response, evaluated from its biquad
	coefficients on a 4000-point log grid from Omega = 1/6 to 6 (plus every
	breakpoint), inside the class 1 limits above, below Nyquist. Reports the
	smallest margin to either limit and the attenuation at the band edges.
	Negative controls: a second-order prototype (skirts too shallow), and
	third order everywhere (the top bands' warped skirts miss the lower
	minimum by 7-11 dB).

	thirds.sines -- end to end: sines through the engine, every band's Fast
	level after 3 s against 20 lg(A |H(f)|) from the same coefficients, for
	bands predicted above -80 dBFS. Tolerance: the 125 ms one-pole leaves a
	ripple at 2f of relative size 1 / sqrt(1 + (4 pi f tau)^2) on the mean
	square, so 10 lg(1 + r) dB, plus 0.005 dB for float input. Why 3 s: the
	sine's switch-on rings the 25 Hz band at about -30 dB, and the Fast
	weighting forgets at only 34.7 dB a second, so at 1.5 s that ring still
	read 0.8 dB into a -78 dB skirt reading; at 3 s it is 100 dB down. This is what
	proves the skirts are the skirts of the running filters, not just of a
	design function. Negative control: the sections run in float.

	thirds.ballistics -- a 1 kHz sine for 2 s, then digital silence. The Fast
	level's decay, fitted from 50 to 400 ms after the cut, must be
	10 lg(e) / 0.125 s = 34.74 dB/s within 0.1 dB/s: IEC 61672-1 defines
	time weighting F by that 125 ms time constant, and the per-sample
	one-pole is an exact exponential, so the bound only has to cover the
	least-squares fit on float levels (the 1 kHz band's own ring is over
	100 dB down by 50 ms). The peak hold must stay flat for thirdHoldSeconds
	after its last peak (within one frame plus one block) and then fall at
	thirdFallDbPerSecond within 0.1 dB/s. Negative controls: time weighting S
	(1 s), and a hold that falls at once.
*/
void checkThirds( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const double      fs   = run.fs;

	ThirdOctaveBank bank;
	bank.setRate( run.fs, mut );

	//-- thirds.centre (through the engine) ---------------------------------
	{
		Driver d( run, Config(), mut );
		Signal s = silence( run.fs, 0.1, 2 );
		d.play( s, []( const Frame&, double ) {} );
		const Frame& f       = d.engine().frame();
		const int    want    = run.fs >= 48000 ? 31 : 30;
		double       worstRel = 0.0;
		int          badLabel = 0, badFloat = 0;
		for( int i = 0; i < f.thirdCount; ++i )
		{
			const double exact = 1000.0 * std::pow( 10.0, ( ThirdOctaveBank::kFirstX + i ) / 10.0 );
			// The Frame carries float; the bank's own double is checked too.
			worstRel = std::max( worstRel, std::fabs( bank.band( i ).exactHz / exact - 1.0 ) );
			badFloat += std::fabs( f.thirds[ i ].exactHz / exact - 1.0 ) > 1e-7 ? 1 : 0;   // float rounding is 6e-8
			if( i < 31 && f.thirds[ i ].nominalHz != kNominal[ i ] )
				++badLabel;
		}
		const bool ok = f.thirdCount == want && bank.bandCount() == want && worstRel < 1e-9 && badLabel == 0 && badFloat == 0;
		rep.add( "thirds.centre", cell, ok, "%d bands (want %d), mid-bands off 1000*10^(x/10) by %.2g relative (limit 1e-9), %d wrong labels, %d frame values off by more than float rounding",
				 f.thirdCount, want, worstRel, badLabel, badFloat );
	}

	//-- thirds.edge ---------------------------------------------------------
	{
		double worst = 0.0;
		int    worstBand = 0;
		for( int i = 0; i < bank.bandCount(); ++i )
		{
			const auto&  b  = bank.band( i );
			// The digital centre the design normalises at.
			const double tl = std::tan( kPi * b.loHz / fs ), th = std::tan( kPi * b.hiHz / fs );
			const double fc = std::atan( std::sqrt( tl * th ) ) * fs / kPi;
			const double h0 = magnitude( b, fc, fs );
			for( double fe : { b.loHz, b.hiHz } )
			{
				const double e = std::fabs( 20.0 * std::log10( h0 / magnitude( b, fe, fs ) ) - 10.0 * std::log10( 2.0 ) );
				if( e > worst )
				{
					worst     = e;
					worstBand = i;
				}
			}
		}
		rep.add( "thirds.edge", cell, worst < 1e-6, "attenuation at the band edges re centre, off 3.0103 dB by at most %.2g dB (limit 1e-6; worst %g Hz band)",
				 worst, bank.band( worstBand ).nominalHz );
		rep.figure( "thirds edge error re 3.0103 dB (dB)", worst );
	}

	//-- thirds.mask ---------------------------------------------------------
	{
		double worstMargin = kInf, worstOmega = 1.0, worstLimit = 0.0, worstA = 0.0;
		int    worstBand   = 0;
		double edgeMin = kInf, edgeMax = -kInf;
		double skirtMargin = kInf, skirtOmega = 1.0;
		int    skirtBand = 0, points = 0, raised = 0;
		for( int i = 0; i < bank.bandCount(); ++i )
		{
			const auto&  b    = bank.band( i );
			const double href = magnitude( b, b.exactHz, fs );
			raised += b.sections > 3 ? 1 : 0;
			const double edge = mapped( 0.5, 3 );
			std::vector< double > omegas;
			const int    n = 4000;
			for( int k = 0; k <= n; ++k )
				omegas.push_back( std::pow( 6.0, -1.0 + 2.0 * k / n ) );
			for( const Breakpoint& p : kInside )
				omegas.push_back( mapped( p.x, 3 ) ), omegas.push_back( 1.0 / mapped( p.x, 3 ) );
			for( const Breakpoint& p : kOutside )
				omegas.push_back( mapped( p.x, 3 ) ), omegas.push_back( 1.0 / mapped( p.x, 3 ) );
			for( double om : omegas )
			{
				const double f = om * b.exactHz;
				if( f >= 0.5 * fs * 0.999 )
					continue;   // the bilinear transform's zero at Nyquist: beyond it there is nothing to judge
				double lo, hi;
				mask( om, 3, lo, hi );
				const double a = 20.0 * std::log10( href / std::max( 1e-300, magnitude( b, f, fs ) ) );
				const double m = std::min( a - lo, hi - a );
				++points;
				const double w = om >= 1.0 ? om : 1.0 / om;
				if( w > edge * ( 1.0 + 1e-12 ) && m < skirtMargin )
				{
					skirtMargin = m;
					skirtOmega  = om;
					skirtBand   = i;
				}
				if( m < worstMargin )
				{
					worstMargin = m;
					worstOmega  = om;
					worstBand   = i;
					worstA      = a;
					worstLimit  = ( a - lo < hi - a ) ? lo : hi;
				}
			}
			for( double fe : { b.loHz, b.hiHz } )
			{
				const double a = 20.0 * std::log10( href / magnitude( b, fe, fs ) );
				edgeMin        = std::min( edgeMin, a );
				edgeMax        = std::max( edgeMax, a );
			}
		}
		rep.add( "thirds.mask", cell, worstMargin >= 0.0,
				 "%d points on %d bands (%d above third order) inside IEC 61260 class 1: smallest margin %.3f dB (%g Hz band at Omega %.4f: %.3f dB against limit %.2f); "
				 "on the skirts %.2f dB (%g Hz band at Omega %.4f); edge attenuation %.4f..%.4f dB (class 1 allows 2.0..5.0)",
				 points, bank.bandCount(), raised, worstMargin, bank.band( worstBand ).nominalHz, worstOmega, worstA, worstLimit, skirtMargin,
				 bank.band( skirtBand ).nominalHz, skirtOmega, edgeMin, edgeMax );
		rep.figure( "thirds smallest margin on the skirts (dB)", skirtMargin, false );
		rep.figure( "thirds smallest margin to the class 1 mask (dB)", worstMargin, false );
		rep.figure( "thirds edge attenuation, highest (dB)", edgeMax );
		rep.figure( "thirds edge attenuation, lowest (dB)", edgeMin, false );
	}

	//-- thirds.sines (end to end) -------------------------------------------
	{
		const double amp   = 0.5;
		const double tau   = 0.125;
		const double freqs[] = { 25.0, 101.0, 1000.0, 1122.0 /* the 1000 Hz band's upper edge */, 9000.0 };
		double       worstExcess = -kInf, worstErr = 0.0;
		int          compared = 0;
		std::string  worstAt;
		for( double hz : freqs )
		{
			Signal s = silence( run.fs, 3.0 );
			addSine( s, hz, amp );
			Driver d( run, Config(), mut );
			d.play( widen( s, 2 ), []( const Frame&, double ) {} );
			const Frame& f = d.engine().frame();
			const double r = 1.0 / std::sqrt( 1.0 + std::pow( 4.0 * kPi * hz * tau, 2.0 ) );
			const double tol = 10.0 * std::log10( 1.0 + r ) + 0.005;
			for( int i = 0; i < f.thirdCount && i < bank.bandCount(); ++i )
			{
				const double want = 20.0 * std::log10( amp * magnitude( bank.band( i ), hz, fs ) );
				if( want < -80.0 )
					continue;
				++compared;
				const double err = std::fabs( f.thirds[ i ].db - want );
				if( err - tol > worstExcess )
				{
					worstExcess = err - tol;
					worstErr    = err;
					worstAt     = format( "%g Hz sine in the %g Hz band: %.3f dB, predicted %.3f, limit %.3f", hz, f.thirds[ i ].nominalHz,
										  f.thirds[ i ].db, want, tol );
				}
			}
		}
		rep.add( "thirds.sines", cell, worstExcess <= 0.0, "%d band readings against the coefficients' response; worst: %s",
				 compared, worstAt.c_str() );
		rep.figure( "thirds end-to-end error, worst (dB)", worstErr );
	}

	//-- thirds.ballistics ----------------------------------------------------
	{
		const double cut = 2.0;
		Signal       s   = silence( run.fs, 4.5 );
		addBurst( s, 1000.0, 0.5, 0.0, cut, 0.001, 0.0 );
		const Config cfg;
		Driver       d( run, cfg, mut );
		std::vector< double > ft, fdb, fhold;
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) {
			for( int i = 0; i < f.thirdCount; ++i )
				if( f.thirds[ i ].nominalHz == 1000.0f )
				{
					ft.push_back( d.fedSeconds() );
					fdb.push_back( f.thirds[ i ].db );
					fhold.push_back( f.thirds[ i ].holdDb );
				}
		} );
		auto slope = [ & ]( const std::vector< double >& y, double from, double to ) {
			double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
			for( size_t i = 0; i < ft.size(); ++i )
				if( ft[ i ] >= from && ft[ i ] <= to )
				{
					n += 1;
					sx += ft[ i ];
					sy += y[ i ];
					sxx += ft[ i ] * ft[ i ];
					sxy += ft[ i ] * y[ i ];
				}
			return n > 2 ? ( n * sxy - sx * sy ) / ( n * sxx - sx * sx ) : 0.0;
		};
		const double fastWant = 10.0 * std::log10( std::exp( 1.0 ) ) / 0.125;
		const double fast     = -slope( fdb, cut + 0.05, cut + 0.40 );
		// The hold: its last rise before the fall, and the first frame it fell.
		double peakAt = -1.0, fallAt = -1.0;
		for( size_t i = 1; i < ft.size(); ++i )
		{
			if( fhold[ i ] > fhold[ i - 1 ] || ( ft[ i ] < cut && fhold[ i ] == fdb[ i ] ) )
				peakAt = ft[ i ];
			if( ft[ i ] > cut && fallAt < 0.0 && fhold[ i ] < fhold[ i - 1 ] )
				fallAt = ft[ i ];
		}
		const double held     = fallAt - peakAt;
		const double slack    = 1.0 / run.fps + (double)run.block / run.fs;
		const double fall     = fallAt > 0.0 ? -slope( fhold, fallAt + 0.05, fallAt + 0.5 ) : 0.0;
		const bool   fastOk   = std::fabs( fast - fastWant ) <= 0.1;
		const bool   holdOk   = peakAt > 0.0 && fallAt > 0.0 && held >= cfg.thirdHoldSeconds - 1e-9 && held <= cfg.thirdHoldSeconds + slack + 1e-6;
		const bool   fallOk   = std::fabs( fall - cfg.thirdFallDbPerSecond ) <= 0.1;
		rep.add( "thirds.ballistics", cell, ft.size() > 100 && fastOk && holdOk && fallOk,
				 "1 kHz band after a cut: Fast decay %.3f dB/s (want %.3f +-0.1); hold %.3f s after its last peak (want %.3f..%.3f); then falls %.3f dB/s (want %.1f +-0.1)",
				 fast, fastWant, held, (double)cfg.thirdHoldSeconds, cfg.thirdHoldSeconds + slack, fall, (double)cfg.thirdFallDbPerSecond );
		rep.figure( "thirds Fast decay error (dB/s)", std::fabs( fast - fastWant ) );
	}
}

} // namespace estest
