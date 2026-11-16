// --onset-prime, --onset-attack, --bands

#include "Harness.h"
#include "Signals.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace estest
{
namespace
{
/// The band analyser's window at fs: the power of two nearest 21.3 ms.
int bandWindow( int fs )
{
	const double want = 0.0213 * fs;
	int          n    = 1;
	while( n < (int)want )
		n <<= 1;
	if( (double)n / want > std::sqrt( 2.0 ) )
		n >>= 1;
	return std::max( 256, n );
}

struct OnsetLog
{
	struct Hit
	{
		int    band;
		double time;      ///< audio time of the onset (Band::lastOnsetTime)
		double strength;
	};
	std::vector< Hit > hits;
	uint32_t           last[ earshot::kMaxBands ] = {};

	void take( const Frame& f )
	{
		for( int b = 0; b < f.bandCount; ++b )
		{
			const uint32_t c = f.bands[ b ].onsetCount;
			// Several onsets between two frames report only the latest time;
			// count them all, stamped with it.
			for( uint32_t k = last[ b ]; k < c; ++k )
				hits.push_back( { b, f.bands[ b ].lastOnsetTime, f.bands[ b ].onsetStrength } );
			last[ b ] = c;
		}
	}
	int count( int band, double from, double to ) const
	{
		int n = 0;
		for( const Hit& h : hits )
			if( ( band < 0 || h.band == band ) && h.time >= from && h.time <= to )
				++n;
		return n;
	}
	double first( int band, double from, double to ) const
	{
		for( const Hit& h : hits )
			if( h.band == band && h.time >= from && h.time <= to )
				return h.time;
		return -1.0;
	}
	std::string bandsFiring( double from, double to ) const
	{
		std::string s;
		for( int b = 0; b < earshot::kMaxBands; ++b )
			if( count( b, from, to ) > 0 )
				s += format( "%s%d", s.empty() ? "" : ",", b );
		return s.empty() ? "none" : s;
	}
};
} // namespace

//---------------------------------------------------------------------------
// Frame one. The fleet's bug: a detector that starts from zero reads every
// bin as risen from silence on its first frame, and if its floor then snaps
// to that flux it is deaf until the floor decays (pattern v0.1.0 measured
// 1.5 s). The check: a dense mix from sample 0 must fire nothing before the
// first real hit, the first real hit at 250 ms must fire, and the same hit at
// 3 s (a settled detector) must fire too -- the last is the control that
// says the hit is detectable at all.
//
// Latency bound: an onset is reported at the end of the first hop whose
// window sees the attack, so within one window plus one hop (26.7 ms at
// 48 kHz) of it; that is the bound, from the analyser's geometry.
//---------------------------------------------------------------------------

void checkOnsetPrime( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const int         n    = bandWindow( run.fs );
	const double      lat  = ( n + n / 4 ) / (double)run.fs;

	Signal s = silence( run.fs, 3.5 );
	addNoise( s, 0.03, 11 );
	addSine( s, 55.0, 0.15 );
	for( double hz : { 220.0, 277.18, 329.63 } )
		addSine( s, hz, 0.1, hz );
	addKick( s, 0.25, 0.8 );
	addKick( s, 3.0, 0.8 );

	Driver   d( run, Config(), mut );
	OnsetLog log;
	d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) { log.take( f ); } );

	const int    early = log.count( -1, 0.0, 0.2499 );
	const double hit1  = log.first( 0, 0.25, 0.25 + lat );
	const double hit2  = log.first( 0, 3.0, 3.0 + lat );
	rep.add( "onset.prime.quiet", cell, early == 0, "a dense mix from sample 0: %d onsets before the first hit (must be 0; bands %s)",
			 early, log.bandsFiring( 0.0, 0.2499 ).c_str() );
	rep.add( "onset.prime.hears", cell, hit1 >= 0.0, "first kick at 250 ms: %s (limit %.1f ms)",
			 hit1 >= 0.0 ? format( "heard after %.1f ms", ( hit1 - 0.25 ) * 1000.0 ).c_str() : "MISSED", lat * 1000.0 );
	rep.add( "onset.prime.steady", cell, hit2 >= 0.0, "control: the same kick at 3 s: %s",
			 hit2 >= 0.0 ? format( "heard after %.1f ms", ( hit2 - 3.0 ) * 1000.0 ).c_str() : "MISSED" );
	if( hit1 >= 0.0 )
		rep.figure( "onset latency, primed kick (ms)", ( hit1 - 0.25 ) * 1000.0 );

	//-- Steady material fires nothing -----------------------------------------
	{
		struct Mix
		{
			const char* name;
			std::function< void( Signal& ) > make;
		};
		const Mix mixes[] = {
			{ "pad+noise #1", []( Signal& x ) {
				 addNoise( x, 0.03, 21 );
				 addSine( x, 55.0, 0.15 );
				 for( double hz : { 220.0, 277.18, 329.63 } )
					 addSine( x, hz, 0.1, hz );
			 } },
			{ "pad+noise #2", []( Signal& x ) {
				 addNoise( x, 0.03, 23 );
				 addSine( x, 55.0, 0.15 );
				 for( double hz : { 220.0, 277.18, 329.63 } )
					 addSine( x, hz, 0.1, hz );
			 } },
			{ "pad+noise #3", []( Signal& x ) {
				 addNoise( x, 0.03, 25 );
				 addSine( x, 55.0, 0.15 );
				 for( double hz : { 220.0, 277.18, 329.63 } )
					 addSine( x, hz, 0.1, hz );
			 } },
			{ "noise -20 dBFS", []( Signal& x ) { addNoise( x, 0.1, 22 ); } },
			{ "bass 41 Hz", []( Signal& x ) { addSine( x, 41.2, 0.4 ); } },
			{ "sung vibrato", []( Signal& x ) {
				 // 6 harmonics of 220 Hz, +-50 cents at 5.5 Hz: the case the
				 // maximum filter exists for (Boeck & Widmer 2013)
				 double ph = 0.0;
				 for( size_t i = 0; i < x.frames(); ++i )
				 {
					 const double t = (double)i / x.fs;
					 const double f = 220.0 * std::pow( 2.0, 50.0 / 1200.0 * std::sin( 2.0 * kPi * 5.5 * t ) );
					 double       v = 0.0;
					 for( int h = 1; h <= 6; ++h )
						 v += 0.25 / h * std::sin( h * ph );
					 x.x[ i ] += (float)v;
					 ph += 2.0 * kPi * f / x.fs;
				 }
			 } },
			{ "organ chord", []( Signal& x ) {
				 for( double f0 : { 130.81, 164.81, 196.0, 261.63 } )
					 for( int h = 1; h <= 6; ++h )
						 addSine( x, f0 * h, 0.06 / h, f0 + h );
			 } },
		};
		// Tonal and mixed material must fire nothing. White noise is the
		// detector's worst case: the lowest band holds three bins, its flux is
		// a sum of three squared differences of chi-square variables, and the
		// tail of that reaches past mean + 12 MAD about once in a few thousand
		// hops. The limit is one per 12 s (a flash every 12 s on a noise floor),
		// recorded as a known limit in AGENTS.md, not a target.
		std::string detail;
		int         tonal = 0, noise = 0;
		for( const Mix& m : mixes )
		{
			Signal x = silence( run.fs, 12.0 );
			m.make( x );
			Driver   dd( run, Config(), mut );
			OnsetLog lg;
			dd.play( widen( x, 2 ), [ & ]( const Frame& f, double ) { lg.take( f ); } );
			const int c = lg.count( -1, 0.0, 12.0 );
			( std::string( m.name ).rfind( "noise ", 0 ) == 0 ? noise : tonal ) += c;
			detail += format( " %s:%d[%s]", m.name, c, lg.bandsFiring( 0.0, 12.0 ).c_str() );
		}
		rep.figure( "onset false onsets on 12 s white noise", noise );
		const int total = tonal;
		rep.add( "onset.steady", cell, total == 0 && noise <= 1, "12 s of steady material, onsets per mix (tonal 0, white noise at most 1):%s", detail.c_str() );
	}
}

//---------------------------------------------------------------------------
// Attacks. pattern's broadband lesson: a hard attack's first milliseconds
// are a click across the spectrum, and a 150 Hz burst lit all eight of its
// channels. A click is real, though: an attack of length T spreads a tone
// over the main lobe of its envelope, about +-2/T (a 1 ms rise reaches
// 2 kHz), and the first hop that sees it sees it through one hop of window
// (+-2/hop), on top of the analysis window's own main lobe (+-2 bins).
// Beyond the sum of those three half-widths there are only sidelobes, more
// than 30 dB down, and that is what the masking gate must remove. So:
//
// onset.attack.hard  -- tone bursts with a 1 ms rise on digital silence
//   (log flux on a silent band is where splatter is loudest): the tone's own
//   band fires within one window plus one hop, and no band beyond the reach
//   fires. Negative control: no gate.
// onset.attack.release -- the same bursts end with a 50 ms release: nothing
//   fires. Negative control: flux that counts falls as well as rises.
// onset.attack.cut -- bursts that stop dead (1 ms release): the cut clicks
//   like an attack, its splatter rising in bands the note never touched.
//   Nothing beyond the reach may fire. (An ending can fire within its
//   reach, as an attack does: a dead stop is a transient. AGENTS.md.)
// onset.attack.once -- with the refractory at 0, one onset per attack.
//   Negative control: fire on every hop over the threshold.
// onset.attack.soft -- 30 and 80 ms raised-cosine rises: one onset each, in
//   its own band, nothing beyond the reach. Negative control: pattern's
//   linear magnitude flux on the audio path.
//---------------------------------------------------------------------------

void checkOnsetAttack( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell  = run.name();
	const int         n     = bandWindow( run.fs );
	const double      lat   = ( n + n / 4 ) / (double)run.fs;
	const double      hopS  = ( n / 4 ) / (double)run.fs;
	const double      binHz = (double)run.fs / n;
	const double      hz[ 4 ] = { 90.0, 300.0, 1500.0, 8000.0 };   // one per default band
	const Config      def;

	auto reach = [ & ]( double f, double rise, int band ) {
		const double r  = 2.0 / rise + 2.0 / hopS + 2.0 * binHz;
		const double lo = def.bandEdges[ band ], hi = def.bandEdges[ band + 1 ];
		return hi > f - r && lo < f + r;
	};

	auto play = [ & ]( const Config& cfg, double attack, double release, OnsetLog& log, std::vector< double >& starts,
					   std::vector< double >& ends ) {
		Signal s = silence( run.fs, 0.4 + 4 * 0.8 );
		starts.clear();
		ends.clear();
		for( int b = 0; b < 4; ++b )
		{
			const double t0 = 0.4 + 0.8 * b;
			addBurst( s, hz[ b ], 0.5, t0, t0 + 0.35, attack, release );
			starts.push_back( t0 );
			ends.push_back( t0 + 0.35 );
		}
		Driver d( run, cfg, mut );
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) { log.take( f ); } );
	};

	//-- Hard attacks, and releases ---------------------------------------
	{
		const double          rise = 0.001;
		OnsetLog              log;
		std::vector< double > st, en;
		play( def, rise, 0.05, log, st, en );
		int         missed = 0, beyond = 0;
		double      worstLat = 0.0;
		std::string detail;
		for( int b = 0; b < 4; ++b )
		{
			const double t = log.first( b, st[ b ] - 0.002, st[ b ] + lat );
			if( t < 0.0 )
				++missed;
			else
				worstLat = std::max( worstLat, t - st[ b ] );
			for( int o = 0; o < 4; ++o )
				if( !reach( hz[ b ], rise, o ) && log.count( o, st[ b ] - 0.005, st[ b ] + 0.05 ) > 0 )
					++beyond;
			detail += format( " %.0fHz:[%s]", hz[ b ], log.bandsFiring( st[ b ] - 0.005, st[ b ] + 0.05 ).c_str() );
		}
		rep.add( "onset.attack.hard", cell, missed == 0 && beyond == 0,
				 "1 ms attacks on silence: %d missed, %d beyond the click's reach; bands lit:%s; worst latency %.1f ms (limit %.1f)",
				 missed, beyond, detail.c_str(), worstLat * 1000.0, lat * 1000.0 );
		rep.figure( "onset latency, hard attack (ms)", worstLat * 1000.0 );

		int         atEnd = 0;
		std::string ends;
		for( int b = 0; b < 4; ++b )
		{
			atEnd += log.count( -1, en[ b ] - 0.06, en[ b ] + 0.05 );
			ends += format( " %.0fHz:[%s]", hz[ b ], log.bandsFiring( en[ b ] - 0.06, en[ b ] + 0.05 ).c_str() );
		}
		rep.add( "onset.attack.release", cell, atEnd == 0, "50 ms releases: %d onsets (must be 0); bands lit:%s", atEnd,
				 ends.c_str() );
	}

	//-- A cut: a note that stops dead clicks too, and an ending is no onset --
	{
		const double          cut = 0.001;
		OnsetLog              log;
		std::vector< double > st, en;
		play( def, 0.05, cut, log, st, en );
		int         beyond = 0;
		std::string detail;
		for( int b = 0; b < 4; ++b )
		{
			for( int o = 0; o < 4; ++o )
				if( !reach( hz[ b ], cut, o ) && log.count( o, en[ b ] - 0.005, en[ b ] + 0.05 ) > 0 )
					++beyond;
			detail += format( " %.0fHz:[%s]", hz[ b ], log.bandsFiring( en[ b ] - 0.005, en[ b ] + 0.05 ).c_str() );
		}
		rep.add( "onset.attack.cut", cell, beyond == 0, "1 ms cuts: %d onsets beyond the click's reach (must be 0); bands lit:%s",
				 beyond, detail.c_str() );
	}

	//-- One onset per attack, even with no refractory ----------------------
	{
		Config cfg;
		cfg.onsetRefractory = 0.0f;
		OnsetLog              log;
		std::vector< double > st, en;
		play( cfg, 0.001, 0.05, log, st, en );
		int         wrong = 0;
		std::string counts;
		for( int b = 0; b < 4; ++b )
		{
			const int c = log.count( b, st[ b ] - 0.002, st[ b ] + 0.3 );
			wrong += c != 1 ? 1 : 0;
			counts += format( " %d", c );
		}
		rep.add( "onset.attack.once", cell, wrong == 0, "1 ms attacks, refractory 0: onsets in the attacked band per burst:%s (each must be 1)",
				 counts.c_str() );
	}

	//-- Soft attacks --------------------------------------------------------
	{
		int         bad = 0;
		std::string detail;
		double      worstLat = 0.0;
		for( double rise : { 0.03, 0.08 } )
		{
			OnsetLog              log;
			std::vector< double > st, en;
			play( def, rise, 0.05, log, st, en );
			for( int b = 0; b < 4; ++b )
			{
				const double to = st[ b ] + rise + lat;
				const int    c  = log.count( b, st[ b ], to );
				const double t  = log.first( b, st[ b ], to );
				int          far = 0;
				for( int o = 0; o < 4; ++o )
					if( o != b && !reach( hz[ b ], rise, o ) )
						far += log.count( o, st[ b ], to );
				if( c != 1 || far != 0 )
					++bad;
				if( t >= 0.0 )
					worstLat = std::max( worstLat, t - st[ b ] );
				detail += format( " %.0fms/%.0fHz:%d[%s]", rise * 1000, hz[ b ], c, log.bandsFiring( st[ b ], to ).c_str() );
			}
		}
		rep.add( "onset.attack.soft", cell, bad == 0, "30 and 80 ms rises, one onset in its own band, none beyond reach:%s",
				 detail.c_str() );
		rep.figure( "onset latency, soft attack from rise start (ms)", worstLat * 1000.0 );
	}

	//-- A swell is not an attack -------------------------------------------
	{
		Signal s = silence( run.fs, 2.0 );
		addNoise( s, 1e-4, 6 );
		Rng rng( 8 );
		for( size_t i = 0; i < s.frames(); ++i )
		{
			const double t  = (double)i / run.fs;
			const double db = t < 0.2 ? -80.0 : std::min( -10.0, -80.0 + 70.0 * ( t - 0.2 ) / 1.5 );
			s.x[ i ] += (float)( std::pow( 10.0, db / 20.0 ) * rng.gauss() );
		}
		Driver   d( run, Config(), mut );
		OnsetLog log;
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) { log.take( f ); } );
		const int c = log.count( -1, 0.0, 2.0 );
		rep.add( "onset.attack.swell", cell, c == 0, "white noise swelling 70 dB over 1.5 s: %d onsets (must be 0; bands %s)", c,
				 log.bandsFiring( 0.0, 2.0 ).c_str() );
	}

	//-- A clap is broadband and lights what it reaches --------------------
	{
		Signal s = silence( run.fs, 1.5 );
		addNoise( s, 1e-4, 9 );
		Rng rng( 10 );
		addClap( s, 0.5, 0.5, rng );
		addClap( s, 1.0, 0.5, rng );
		Driver   d( run, Config(), mut );
		OnsetLog log;
		d.play( widen( s, 2 ), [ & ]( const Frame& f, double ) { log.take( f ); } );
		int lit = 0;
		for( int b = 1; b < 4; ++b )
			lit += log.count( b, 0.495, 0.5 + lat ) > 0 && log.count( b, 0.995, 1.0 + lat ) > 0 ? 1 : 0;
		rep.add( "onset.attack.broadband", cell, lit == 3, "claps (700 Hz-5 kHz noise): bands lit [%s] and [%s]; bands 1-3 must all fire",
				 log.bandsFiring( 0.495, 0.5 + lat ).c_str(), log.bandsFiring( 0.995, 1.0 + lat ).c_str() );
	}
}

//---------------------------------------------------------------------------
// Band levels and envelopes.
//
// bands.level: a sine on an exact bin centre puts all its windowed energy in
// three bins (Hann's transform is zero at every other integer offset), so a
// band containing those three bins must read the sine's amplitude to float
// precision; 0.01 dB is the tolerance. The same for a sine between bins at
// least ten bins from the band's edges, where Hann leaks less than 1e-6 of
// the energy outside.
// bands.noise: white noise of variance s^2 puts s^2 sum(w^2) in every bin on
// average, so a band of c bins must read E[level^2] = 4 c s^2 / N. The
// tolerance is statistical: four standard deviations of the mean over the
// frames, each frame's estimate having about c / 1.5 degrees of freedom
// (Hann's equivalent noise bandwidth is 1.5 bins), widened by 1.3 for the
// overlap between frames.
// bands.envelope: the attack and release one-poles, timed on frames. The
// bound is the time constant plus the window (the level itself takes a
// window to change) plus one video frame (when we look). Negative control:
// the envelope falls at the attack rate.
//---------------------------------------------------------------------------

void checkBands( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell  = run.name();
	const int         n     = bandWindow( run.fs );
	const double      binHz = (double)run.fs / n;
	Config            cfg;

	//-- Level of a sine ----------------------------------------------------
	{
		double      worst = 0.0;
		std::string detail;
		for( int b = 0; b < cfg.bandCount; ++b )
		{
			const int lo = (int)std::ceil( cfg.bandEdges[ b ] / binHz );
			const int hi = std::min( n / 2, (int)std::ceil( cfg.bandEdges[ b + 1 ] / binHz ) );   // [lo, hi)
			for( int variant = 0; variant < 2; ++variant )
			{
				double f;
				if( variant == 0 )
					f = std::round( std::sqrt( (double)lo * ( hi - 1 ) ) ) * binHz;   // on a bin, mid-band
				else
				{
					if( hi - lo < 24 )
						continue;   // no room for 10 bins each side
					f = ( 0.5 * ( lo + hi ) + 0.37 ) * binHz;   // between bins
				}
				Signal s = silence( run.fs, 0.3 );
				addSine( s, f, 0.5, 0.25 );
				Driver d( run, cfg, mut );
				double maxErr = 0.0;
				d.play( widen( s, 2 ), [ & ]( const Frame& fr, double vt ) {
					if( vt < 0.1 )
						return;
					maxErr = std::max( maxErr, std::fabs( fr.bands[ b ].db - 20.0 * std::log10( 0.5 ) ) );
				} );
				worst = std::max( worst, maxErr );
				detail += format( " %.0fHz:%.4f", f, maxErr );
			}
		}
		rep.add( "bands.level", cell, worst <= 0.01, "sines on and between bins, every band: worst %.4f dB from the amplitude (limit 0.01);%s",
				 worst, detail.c_str() );
		rep.figure( "bands.level worst dB", worst );
	}

	//-- Level of white noise ------------------------------------------------
	{
		const double sigma = 0.1;
		Signal       s     = silence( run.fs, 3.0 );
		addNoise( s, sigma, 12 );
		Driver d( run, cfg, mut );
		double sum[ earshot::kMaxBands ] = {};
		int    frames = 0;
		d.play( widen( s, 2 ), [ & ]( const Frame& fr, double vt ) {
			if( vt < 0.1 )
				return;
			++frames;
			for( int b = 0; b < fr.bandCount; ++b )
				sum[ b ] += (double)fr.bands[ b ].level * fr.bands[ b ].level;
		} );
		bool        ok = true;
		std::string detail;
		double      worstSigmas = 0.0;
		for( int b = 0; b < cfg.bandCount; ++b )
		{
			const int    lo     = (int)std::ceil( cfg.bandEdges[ b ] / binHz );
			const int    hi     = std::min( n / 2, (int)std::ceil( cfg.bandEdges[ b + 1 ] / binHz ) );
			const int    c      = hi - lo;
			const double expect = 4.0 * c * sigma * sigma / n;
			const double got    = sum[ b ] / std::max( 1, frames );
			const double relSd  = 1.3 * std::sqrt( 1.5 / ( (double)c * frames ) );
			const double tolDb  = 10.0 * std::log10( 1.0 + 4.0 * relSd );
			const double errDb  = 10.0 * std::log10( got / expect );
			ok = ok && std::fabs( errDb ) <= tolDb;
			worstSigmas = std::max( worstSigmas, std::fabs( got / expect - 1.0 ) / relSd );
			detail += format( " b%d %+.3f dB (limit %.3f)", b, errDb, tolDb );
		}
		rep.add( "bands.noise", cell, ok, "white noise, mean level^2 against 4 c s^2 / N:%s", detail.c_str() );
		rep.figure( "bands.noise worst error in standard deviations", worstSigmas );
	}

	//-- Envelope timing ------------------------------------------------------
	{
		const double amp = 0.5, on = 0.5, off = 1.0;
		Signal       s   = silence( run.fs, 1.6 );
		addNoise( s, 1e-4, 13 );
		addBurst( s, 1500.0, amp, on, off, 0.001, 0.001 );
		Driver d( run, cfg, mut );
		double up = -1.0, down = -1.0;
		d.play( widen( s, 2 ), [ & ]( const Frame& fr, double ) {
			const double t = d.fedSeconds();
			const double e = fr.bands[ 2 ].envelope;
			if( up < 0.0 && t > on && e >= ( 1.0 - std::exp( -1.0 ) ) * amp )
				up = t - on;
			if( down < 0.0 && t > off && e <= amp * std::exp( -1.0 ) )
				down = t - off;
		} );
		const double win   = (double)n / run.fs;
		const double frame = 1.0 / run.fps + (double)run.block / run.fs;
		const double upMax = cfg.envelopeAttack + win + frame;
		const double dnLo  = cfg.envelopeRelease, dnHi = cfg.envelopeRelease + win + frame;
		const bool   ok    = up >= 0.0 && up <= upMax && down >= dnLo && down <= dnHi;
		rep.add( "bands.envelope", cell, ok, "63%% rise after %.1f ms (limit %.1f); fall to 1/e after %.1f ms (limits %.1f-%.1f)",
				 up * 1000.0, upMax * 1000.0, down * 1000.0, dnLo * 1000.0, dnHi * 1000.0 );
	}
}

} // namespace estest
