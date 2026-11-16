//===========================================================================
// The singing coil (source/Singing.h; SPEC-flyback-singing.md).
//
// Included into main.cpp's anonymous namespace, after the Rig and before
// --negative. On the engine and the pitch input, no GL: --sing, --sing-clock,
// --sing-memory, --device. Through the real plugin and its pixels:
// --sing-picture, --off, --lamp, --resize-sing. And a report, not a check:
// --demo-music DIR.
//
// Every signal is fed through earshot's feed() ("as if a device opened"),
// sample-accurately in step with the frames; no check opens a real device
// (main() installs the scripted backend unless HVTEST_REAL_DEVICES=1).
//===========================================================================

/// earshot's pitch window: three periods of its lowest pitch, 40 Hz (earshot.h,
/// Config::pitchMinHz). A level or a pitch is the last 75 ms's.
constexpr double kWindowSeconds = 3.0 / 40.0;
/// Double's unit roundoff (2^-52: one ulp of 1).
constexpr double kEps = 2.220446049250313e-16;

float DefaultKey()
{
	return earshot::deviceKey( earshot::kDefaultDevice );
}

double Ulp( double x )
{
	return std::nextafter( std::fabs( x ), INFINITY ) - std::fabs( x );
}

double Cents( double a, double b )
{
	return a > 0.0 && b > 0.0 ? 1200.0 * std::log2( a / b ) : 1e9;
}

/// One frame of a sung run: the engine's step ( t0, t1 ] and what the pitch
/// input said for it.
struct SungFrame
{
	double t0 = 0, t1 = 0;
	sing::Note note;
};

struct SungRun
{
	std::vector< SungFrame > frames;
	std::vector< Event > events;
};

/// The coil singing `signal` the way ProcessOpenGL drives it, on the engine
/// alone: before frame i every sample up to i / fps (audio time 0 at the first
/// frame), the pitch input's Update, its rate into the settings, Advance. The
/// first Advance anchors the engine and steps nothing; frame i's rate is the
/// step ( t_(i-1), t_i ]'s, as in the plugin.
SungRun SingEngine( const Signal& signal, Settings s, int octaves, double gateDb, double fps, double origin, const Perturb& perturb )
{
	ScopedNoDevices quiet;// earshot hears only what is fed
	sing::PitchInput pitch;
	pitch.hooks |= perturb.pitchHooks;
	pitch.Apply( DefaultKey(), false );
	Engine engine;
	Frame frame;
	s.machine   = Machine::Tesla;
	s.sing      = true;
	s.singHooks = perturb.singHooks;
	if( perturb.singMemorySeconds >= 0.0 )
		s.memory = perturb.singMemorySeconds;
	SungRun run;
	const int n   = static_cast< int >( std::floor( signal.Seconds() * fps + 1e-9 ) );
	size_t cursor = 0;
	double last   = origin;
	for( int i = 0; i <= n; ++i )
	{
		const double now    = origin + i / fps;
		const size_t target = std::min( signal.Frames(), static_cast< size_t >( std::llround( i / fps * signal.rate ) ) );
		if( target > cursor )
		{
			pitch.Feed( &signal.samples[ cursor * static_cast< size_t >( signal.channels ) ], static_cast< int >( target - cursor ), signal.rate,
			            signal.channels );
			cursor = target;
		}
		SungFrame f;
		f.note     = pitch.Update( now, octaves, gateDb );
		s.singRate = f.note.rate;
		f.t0       = last;
		f.t1       = now;
		engine.Configure( s );
		engine.Advance( now, 1.0 / fps, frame );
		run.frames.push_back( f );
		last = now;
	}
	run.events = engine.Events();
	return run;
}

/// The integral of the frames' rates over ( a, b ], in cycles (bangs).
double RateIntegral( const SungRun& run, double a, double b, double scale = 1.0 )
{
	double cycles = 0.0;
	for( size_t i = 1; i < run.frames.size(); ++i )
	{
		const SungFrame& f = run.frames[ i ];
		const double lo    = std::max( a, f.t0 ), hi = std::min( b, f.t1 );
		if( hi > lo )
			cycles += scale * f.note.rate * ( hi - lo );
	}
	return cycles;
}

/// The fold law as a property, not the formula again: r = hz x 2^(octaves + k)
/// EXACTLY for a whole k, r inside [5, 1000], and a fold that went no further
/// than it had to (folded down: r above 500; folded up: r below 10).
bool FoldHolds( double r, double hz, int octaves, int* kOut = nullptr )
{
	if( !( r > 0.0 ) || !( hz > 0.0 ) )
		return false;
	int e          = 0;
	const double m = std::frexp( r / std::ldexp( hz, octaves ), &e );
	if( m != 0.5 )
		return false;// not a power of two
	const int k = e - 1;
	if( kOut )
		*kOut = k;
	if( r < sing::kRateLow || r > sing::kRateHigh )
		return false;
	if( k < 0 && !( r > 0.5 * sing::kRateHigh ) )
		return false;
	if( k > 0 && !( r < 2.0 * sing::kRateLow ) )
		return false;
	return true;
}

//===========================================================================
// --sing
//===========================================================================
int runSing( const Perturb& perturb )
{
	std::printf( "\n=== sing: the coil bangs at the detected pitch, folded into the interrupter's range; it rests, and sustains\n" );
	const double fps = 60.0;
	Settings base    = MachineSettings( Machine::Tesla );
	base.reach       = 0.25;// cheap growth: this counts bangs

	// 1. The rate: tones through earshot, at the pitch, folded, and an octave off.
	struct Case
	{
		double hz;
		int octaves;
	};
	for( const Case& c : { Case { 110.0, 0 }, Case { 440.0, 0 }, Case { 1318.51, 0 }, Case { 55.0, -3 }, Case { 220.0, 2 }, Case { 98.0, -4 } } )
	{
		const Signal tone = Tone( c.hz, -20.0, 3.0 );
		const SungRun run = SingEngine( tone, base, c.octaves, -60.0, fps, 0.0, perturb );
		size_t first      = 0;// the first sounding frame
		while( first < run.frames.size() && !( run.frames[ first ].note.rate > 0.0 ) )
			++first;
		if( first + 10 >= run.frames.size() )
		{
			Check( false, fmt( "%.2f Hz: the coil never sang", c.hz ) );
			continue;
		}
		// Counted from the first sounding step's start INCLUSIVE: nothing came
		// before it (a rest), so a bang at exactly a would be a cycle owed to
		// the silence -- the phase a rest must zero.
		const double a = run.frames[ first ].t0, b = run.frames.back().t1;
		int bangs      = 0, before = 0;
		for( const Event& e : run.events )
		{
			if( e.time >= a && e.time <= b )
				++bangs;
			if( e.time < a )
				++before;
		}
		const double expected = RateIntegral( run, a, b, perturb.singPitchScale );
		// A phase accumulator that starts at 0 (after a rest) and ends in
		// [0, 1) has crossed floor( integral ) whole cycles: the count is the
		// integral less a fraction of one, plus the sum's own rounding.
		const double rounding = 64.0 * kEps * expected;
		int voiced = 0, foldBad = 0, sounding = 0, k = 0, kSeen = 0;
		double worstCents = 0.0, rate = 0.0;
		for( size_t i = first; i < run.frames.size(); ++i )
		{
			const sing::Note& note = run.frames[ i ].note;
			if( note.voiced )
			{
				++voiced;
				worstCents = std::max( worstCents, std::fabs( Cents( note.hz, c.hz ) ) );
			}
			if( note.rate > 0.0 )
			{
				++sounding;
				rate = note.rate;
				if( !FoldHolds( note.rate, note.hz, c.octaves, &k ) )
					++foldBad;
				kSeen = k;
			}
		}
		const int span = static_cast< int >( run.frames.size() - first );
		Check( bangs > 0 && before == 0 && expected - bangs < 1.0 + rounding && bangs <= expected + rounding,
		       fmt( "%7.2f Hz, octave %+d: %d bangs in [%.3f, %.3f] s (%d before, in the window's warm-up); the detected pitch's rate "
		            "integrates to %.4f there (the count must be that less a fraction of one)",
		            c.hz, c.octaves, bangs, a, b, before, expected ) );
		const std::string folded = kSeen == 0 ? std::string() : fmt( ", folded %+d", kSeen );
		Check( foldBad == 0 && sounding == span && voiced >= span - 2 && worstCents <= 1.0,
		       fmt( "%7.2f Hz, octave %+d: %.4f BPS = the note x 2^(%+d%s) exactly on %d of %d frames, sounding on %d; voiced on %d; "
		            "worst %.3f cents off the tone (earshot's README: 1 cent for a sine)",
		            c.hz, c.octaves, rate, c.octaves, folded.c_str(), sounding - foldBad, span, sounding, voiced, worstCents ) );
	}

	// 2. Between any two bangs the rate integrates to exactly one cycle, the
	// phase accumulator's definition -- through a glide, where the rate is new
	// every frame. An exponential sweep 200 -> 400 Hz in 2 s, through earshot.
	{
		Signal glide;
		glide.rate      = 48000;
		const double f0 = 200.0, f1 = 400.0, T = 2.0;
		const size_t n  = static_cast< size_t >( T * glide.rate );
		glide.samples.resize( 2 * n );
		for( size_t i = 0; i < n; ++i )
		{
			// phase( t ) = 2 pi f0 T / ln( f1 / f0 ) ( ( f1 / f0 )^( t / T ) - 1 )
			const double t  = static_cast< double >( i ) / glide.rate;
			const double ph = 2.0 * kPi * f0 * T / std::log( f1 / f0 ) * ( std::exp( t * std::log( f1 / f0 ) / T ) - 1.0 );
			glide.samples[ 2 * i ] = glide.samples[ 2 * i + 1 ] = static_cast< float >( 0.1 * std::sin( ph ) );
		}
		const SungRun run = SingEngine( glide, base, 0, -60.0, fps, 0.0, perturb );
		double worst = 0.0, bound = 0.0, lastRate = 0.0;
		int intervals = 0, distinct = 0, outside = 0;
		for( const SungFrame& f : run.frames )
			if( f.note.rate > 0.0 && f.note.rate != lastRate )
			{
				++distinct;
				lastRate = f.note.rate;
			}
		for( size_t e = 1; e < run.events.size(); ++e )
		{
			const double t0 = run.events[ e - 1 ].time, t1 = run.events[ e ].time;
			const double cycles = RateIntegral( run, t0, t1, perturb.singPitchScale );
			// The arithmetic's bound: each step's phase sum rounds by an ulp of
			// what it holds (under 1 + 1000/24 cycles) and the product by one of
			// r dt; each bang time by an ulp of the clock, which r turns into
			// cycles; this integral's own sums the same again. Doubled.
			const double steps = ( t1 - t0 ) * fps + 2.0;
			const double b     = 2.0 * ( 2.0 * steps * ( 2.0 + 1000.0 / 24.0 ) * kEps + 2.0 * 1000.0 * Ulp( t1 ) );
			bound              = std::max( bound, b );
			worst              = std::max( worst, std::fabs( cycles - 1.0 ) );
			outside += std::fabs( cycles - 1.0 ) > b ? 1 : 0;
			++intervals;
		}
		Check( intervals > 400 && distinct > 60 && outside == 0,
		       fmt( "a glide 200 -> 400 Hz in 2 s (%d distinct rates): %d intervals between bangs each integrate the rate to 1 cycle, worst "
		            "%.1e off; %d outside their rounding bound (largest %.1e)",
		            distinct, intervals, worst, outside, bound ) );
	}

	// 3. Rests. A tone, then a second of quiet (noise at -90 dBFS: a rest in a
	// real room is not bit-exact zeros, and zeros are earshot's Silent), then
	// noise as loud as the tone, then another tone. In the quiet the coil
	// stops once earshot's window has cleared the tone (75 ms, and a frame);
	// in the noise after the rest it has no note to sustain, because the rest
	// forgot it; the second tone sings its own pitch.
	{
		const double rest = 1.0, noisy = 2.0, second = 2.5;
		Signal s          = Tone( 220.0, -20.0, rest );
		s.Then( Noise( -90.0, noisy - rest, 3u ) ).Then( Noise( -20.0, second - noisy, 5u ) ).Then( Tone( 330.0, -20.0, 1.0 ) );
		const SungRun run = SingEngine( s, base, 0, -60.0, fps, 0.0, perturb );
		const double stop = rest + kWindowSeconds + 1.0 / fps;
		int late = 0, oldNote = 0, noiseFrames = 0, after = 0, afterRight = 0;
		double lastLate = -1.0;
		for( const Event& e : run.events )
			if( e.time > stop && e.time <= noisy )
			{
				++late;
				lastLate = e.time;
			}
		for( const SungFrame& f : run.frames )
		{
			if( f.t0 >= noisy && f.t1 <= second )
			{
				++noiseFrames;
				if( f.note.rate > 0.0 && std::fabs( Cents( f.note.hz, 220.0 ) ) <= 1.0 )
					++oldNote;
			}
			if( f.t0 >= second + kWindowSeconds + 1.0 / fps )
			{
				++after;
				if( f.note.rate > 0.0 && std::fabs( Cents( f.note.hz, 330.0 ) ) <= 1.0 )
					++afterRight;
			}
		}
		const std::string lastText = late ? fmt( ", the last at %.3f s", lastLate ) : std::string();
		Check( late == 0, fmt( "a tone, then a rest at -90 dBFS: %d bangs after %.3f s (the 75 ms window and a frame past the tone's end)%s", late,
		                       stop, lastText.c_str() ) );
		Check( oldNote == 0 && after > 20 && afterRight == after,
		       fmt( "after the rest, noise as loud as the tone: %d of %d frames sound the forgotten 220 Hz; the next tone sounds 330 Hz, within "
		            "1 cent, on %d of its %d frames past the window",
		            oldNote, noiseFrames, afterRight, after ) );
	}

	// 4. The Gate: a tone at -70 dBFS rests under a -60 gate and sings under -80.
	{
		const Signal soft  = Tone( 220.0, -70.0, 1.5 );
		const SungRun shut = SingEngine( soft, base, 0, -60.0, fps, 0.0, perturb );
		const SungRun open = SingEngine( soft, base, 0, -80.0, fps, 0.0, perturb );
		int sounding = 0;
		for( const SungFrame& f : open.frames )
			if( f.note.rate > 0.0 )
				++sounding;
		const int frames = static_cast< int >( open.frames.size() );
		// The window's warm-up: no pitch until 75 ms of audio, a frame to see it.
		const int warm = static_cast< int >( std::ceil( kWindowSeconds * fps ) ) + 1;
		Check( shut.events.empty() && sounding >= frames - warm,
		       fmt( "a -70 dBFS tone: %zu bangs under a -60 dBFS gate; under -80 the coil sounds on %d of %d frames (all but the %d of the "
		            "window's warm-up)",
		            shut.events.size(), sounding, frames, warm ) );
	}

	// 5. Sustain: a tone and then noise as loud. The noise is above the gate and
	// unvoiced, so the coil holds the tone's note through it.
	{
		Signal s = Tone( 220.0, -20.0, 1.0 );
		s.Then( Noise( -20.0, 1.0, 7u ) );
		const SungRun run = SingEngine( s, base, 0, -60.0, fps, 0.0, perturb );
		int frames = 0, sounding = 0, voicedNoise = 0;
		double heldLo = 1e9, heldHi = 0.0;
		for( const SungFrame& f : run.frames )
		{
			if( f.t0 < 1.0 + kWindowSeconds + 1.0 / fps )
				continue;
			++frames;
			sounding += f.note.rate > 0.0 ? 1 : 0;
			voicedNoise += f.note.voiced ? 1 : 0;
			heldLo = std::min( heldLo, f.note.hz );
			heldHi = std::max( heldHi, f.note.hz );
		}
		// The note held is the last voiced frame's, whose window already had
		// noise in it: near the tone's, not exactly it (printed, not claimed).
		Check( frames > 50 && sounding == frames,
		       fmt( "a tone and then noise as loud: the coil sounds on %d of the %d noise frames past the window (sustained); the note held "
		            "%.2f..%.2f Hz (%+.1f cents from the tone: the last voiced frame, noise already in its window); earshot called %d of the "
		            "noise frames voiced",
		            sounding, frames, heldLo, heldHi, Cents( heldHi, 220.0 ), voicedNoise ) );
	}
	return Verdict();
}

//===========================================================================
// --sing-clock
//===========================================================================
/// The engine alone at a constant sung rate, stepped at the given times.
std::vector< double > SungTimes( double rate, const std::vector< double >& steps, int hooks )
{
	Settings s  = MachineSettings( Machine::Tesla );
	s.reach     = 0.05;
	s.sing      = true;
	s.singRate  = rate;
	s.singHooks = hooks;
	Engine engine;
	Frame frame;
	for( size_t i = 0; i < steps.size(); ++i )
	{
		engine.Configure( s );
		engine.Advance( steps[ i ], i ? steps[ i ] - steps[ i - 1 ] : 1.0 / 60.0, frame );
	}
	std::vector< double > t;
	for( const Event& e : engine.Events() )
		t.push_back( e.time );
	return t;
}

int runSingClock( const Perturb& perturb )
{
	std::printf( "\n=== sing-clock: the sung interrupter is the same at any frame rate, and at Resolume's half a million seconds\n" );
	// 1. Frame-rate independence: a constant rate stepped at 24, 30, 60 and 144
	// fps and at irregular steps (1/240 .. 1/20 s) over the same 2 s. Bang k
	// must be at k / r. The bound is the arithmetic's: each step rounds the
	// phase sum (it holds under r dt + 1 cycles) and the product r dt, so the
	// phase is off by at most 2 eps ( r T + steps ) cycles after T; a time adds
	// an ulp of the clock. All of it times four.
	for( double rate : { 261.6256, 997.3, 7.25 } )
	{
		const double T = 2.0;
		std::vector< std::pair< std::string, std::vector< double > > > plans;
		for( double fps : { 24.0, 30.0, 60.0, 144.0 } )
		{
			std::vector< double > steps;
			const int n = static_cast< int >( std::lround( T * fps ) );
			for( int i = 0; i <= n; ++i )
				steps.push_back( i / fps );
			plans.push_back( { fmt( "%.0f fps", fps ), steps } );
		}
		{
			std::vector< double > steps = { 0.0 };
			uint32_t x                  = 12345u;
			while( steps.back() < T - 0.06 )
			{
				x ^= x << 13;
				x ^= x >> 17;
				x ^= x << 5;
				steps.push_back( steps.back() + 1.0 / 240.0 + ( 1.0 / 20.0 - 1.0 / 240.0 ) * ( x / 4294967296.0 ) );
			}
			steps.push_back( T );
			plans.push_back( { fmt( "%zu irregular steps", steps.size() - 1 ), steps } );
		}
		const long expected = static_cast< long >( std::floor( rate * T ) );
		double worst = 0.0, widest = 0.0;
		std::string counts;
		bool allCounts = true, allTimes = true;
		for( const auto& plan : plans )
		{
			const std::vector< double > t = SungTimes( rate, plan.second, perturb.singHooks );
			const double steps            = static_cast< double >( plan.second.size() );
			const double bound            = 4.0 * ( 2.0 * kEps * ( rate * T + steps ) / rate + Ulp( T ) );
			widest                        = std::max( widest, bound );
			counts += fmt( " %s %zu;", plan.first.c_str(), t.size() );
			allCounts = allCounts && static_cast< long >( t.size() ) == expected;
			for( size_t k = 0; k < t.size(); ++k )
			{
				const double err = std::fabs( t[ k ] - static_cast< double >( k + 1 ) / rate );
				worst            = std::max( worst, err );
				allTimes         = allTimes && err <= bound;
			}
		}
		Check( allCounts && allTimes, fmt( "%8.4f BPS over 2 s: floor( r T ) = %ld bangs at every stepping (%s ); every bang at k / r, worst %.1e s "
		                                   "(bound %.1e)",
		                                   rate, expected, counts.c_str(), worst, widest ) );
	}

	// 2. The clock at Resolume's measured ~499 million ms, against zero: the
	// same intervals. A double resolves 5.8e-11 s at 499,000 s; the phase sees
	// only the step's length (Sterbenz: exact). A float clock there resolves
	// 0.031 s, two periods of a 64 Hz note.
	{
		const double rate = 440.0, T = 2.0;
		double worst = 0.0, bound = 0.0;
		size_t counted[ 2 ] = { 0, 0 };
		bool ok             = true;
		for( int run = 0; run < 2; ++run )
		{
			const double origin = run == 0 ? 0.0 : 499000.0;
			std::vector< double > steps;
			for( int i = 0; i <= static_cast< int >( T * 60.0 ); ++i )
				steps.push_back( origin + i / 60.0 );
			const std::vector< double > t = SungTimes( rate, steps, perturb.singHooks );
			counted[ run ]                = t.size();
			// Each time rounds to half an ulp of the clock; an interval, one.
			// The phase's own drift (as above) is 1e-17 s here. Four ulps.
			const double b = 4.0 * Ulp( origin + T ) + 4.0 * kEps * ( rate * T + static_cast< double >( steps.size() ) ) / rate;
			if( run == 1 )
				bound = b;
			for( size_t k = 1; k < t.size(); ++k )
			{
				const double err = std::fabs( ( t[ k ] - t[ k - 1 ] ) - 1.0 / rate );
				if( run == 1 )
					worst = std::max( worst, err );
				ok = ok && err <= b;
			}
		}
		Check( ok && counted[ 0 ] == counted[ 1 ] && counted[ 0 ] == static_cast< size_t >( std::floor( rate * T ) ),
		       fmt( "440 BPS from a clock at 499,000 s and at 0: %zu and %zu bangs; every interval 1/440 s, worst %.1e s off at 499,000 (bound "
		            "%.1e: four ulps of the clock)",
		            counted[ 1 ], counted[ 0 ], worst, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --sing-memory
//===========================================================================
int runSingMemory( const Perturb& perturb )
{
	std::printf( "\n=== sing-memory: sung streamers are the BPS coil's at the same rate -- long on high notes, short on low\n" );
	// Each note sung, then the BPS coil at the rate it sang, both at tau =
	// 20 ms, reach 0.5, no target: --tesla's memory claim with the sung rate in
	// place of BPS. Tolerances as --tesla's: 3 standard errors, each widened by
	// the square root of the bangs one memory spans.
	struct Row
	{
		double rate = 0, sung = 0, sungErr = 0, bps = 0, bpsErr = 0;
		int n = 0;
	};
	auto stats = []( const std::vector< Event >& events, double from, double tau, double rate, double& mean, double& err, int& n ) {
		double sum = 0, sq = 0;
		n          = 0;
		for( const Event& e : events )
		{
			if( e.time < from )
				continue;
			sum += e.length;
			sq += e.length * e.length;
			++n;
		}
		mean            = n ? sum / n : 0.0;
		const double sd = std::sqrt( std::max( 0.0, n ? sq / n - mean * mean : 0.0 ) );
		err             = n ? sd / std::sqrt( n / std::max( 1.0, tau * rate ) ) : 1e9;
	};
	std::vector< Row > rows;
	for( int octaves : { -4, -3, 0, 1, 2 } )
	{
		Settings s           = MachineSettings( Machine::Tesla );
		s.memory             = 0.020;
		s.reach              = 0.5;
		s.target             = 0;
		const double nominal = std::ldexp( 110.0, octaves );
		const double seconds = std::max( 4.0, 250.0 / nominal ) + 0.5;
		const SungRun run    = SingEngine( Tone( 110.0, -20.0, seconds ), s, octaves, -60.0, 60.0, 0.0, perturb );
		Row row;
		int counted = 0;
		for( const SungFrame& f : run.frames )
			if( f.note.rate > 0.0 )
			{
				row.rate += f.note.rate;
				++counted;
			}
		row.rate /= std::max( 1, counted );
		const double from = 0.5 + kWindowSeconds;// settled, as --tesla's 0.5 s, after the window's warm-up
		stats( run.events, from, 0.020, row.rate, row.sung, row.sungErr, row.n );
		Settings ref = s;
		ref.bps      = row.rate;
		Engine engine;
		Frame frame;
		RunEngine( engine, ref, seconds, frame );
		int nRef = 0;
		stats( engine.Events(), from, 0.020, row.rate, row.bps, row.bpsErr, nRef );
		rows.push_back( row );
		Check( row.n > 100 && std::fabs( row.sung - row.bps ) <= 3.0 * std::hypot( row.sungErr, row.bpsErr ),
		       fmt( "110 Hz at octave %+d sings %7.3f BPS: mean streamer %.3f m +- %.3f over %d bangs; the BPS coil at that rate: %.3f m +- "
		            "%.3f over %d",
		            octaves, row.rate, row.sung, row.sungErr, row.n, row.bps, row.bpsErr, nRef ) );
	}
	auto apart = [ & ]( size_t lo, size_t hi ) { return rows[ hi ].sung - rows[ lo ].sung > 3.0 * std::hypot( rows[ hi ].sungErr, rows[ lo ].sungErr ); };
	Check( std::fabs( rows[ 0 ].sung - rows[ 1 ].sung ) <= 3.0 * std::hypot( rows[ 0 ].sungErr, rows[ 1 ].sungErr ) && apart( 1, 2 ) && apart( 2, 3 )
	           && apart( 3, 4 ),
	       fmt( "the note's octave sets the streamer: flat while 1 / rate >> tau (%.3f m at %.2f BPS, %.3f at %.2f), then longer with every "
	            "octave by 3 SE (%.3f at %.0f, %.3f at %.0f, %.3f at %.0f)",
	            rows[ 0 ].sung, rows[ 0 ].rate, rows[ 1 ].sung, rows[ 1 ].rate, rows[ 2 ].sung, rows[ 2 ].rate, rows[ 3 ].sung, rows[ 3 ].rate,
	            rows[ 4 ].sung, rows[ 4 ].rate ) );
	return Verdict();
}

//===========================================================================
// --device
//===========================================================================
/// Update until `done`, or `limit` seconds of wall time (earshot opens on its
/// own thread). The limit only bounds a hang; it is not a measurement.
bool UpdateUntil( sing::PitchInput& p, double& t, std::function< bool( const sing::Note& ) > done, double limit = 5.0 )
{
	const auto start = std::chrono::steady_clock::now();
	for( ;; )
	{
		t += 1.0 / 60.0;
		const sing::Note n = p.Update( t, 0, -60.0 );
		if( done( n ) )
			return true;
		if( std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() > limit )
			return false;
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
}

void ScriptDevices( std::initializer_list< std::string > names )
{
	std::lock_guard< std::mutex > guard( g_script.lock );
	g_script.present = names;
}

int runDevice( const Perturb& perturb )
{
	std::printf( "\n=== device: nothing asked of the OS until the mode is on; then the menu by name, the lamp's states, Rescan\n" );
	ScriptDevices( { "Bench Mic", "Dante 1-2", g_script.silentName } );
	sing::PitchInput::SetDefaultHooksForTest( perturb.pitchHooks );
	const sing::Counters c0 = sing::CountersForTest();
	{
		// 1. Off: made, Off applied, Rescan pressed, a frame updated -- nothing
		// made, listed or selected.
		sing::PitchInput p;
		double t = 0.0;
		p.Apply( 0.0f, false );
		p.Apply( 0.0f, true );
		const sing::Note off                   = p.Update( t, 0, -60.0 );
		const sing::Counters c1                = sing::CountersForTest();
		const std::vector< sing::Slot >& slots = p.Slots();
		bool spares = slots.size() == static_cast< size_t >( sing::kDeviceElements );
		std::set< float > values;
		for( size_t i = 2; i < slots.size(); ++i )
			spares = spares && slots[ i ].label == "(none)" && slots[ i ].value < 0.0f && values.insert( slots[ i ].value ).second;
		Check( c1.engines == c0.engines && c1.lists == c0.lists && c1.selects == c0.selects && c1.feeds == c0.feeds && c1.updates == c0.updates
		           && !off.active && off.lamp == sing::Lamp::Off && !p.Started(),
		       fmt( "off: made, Off applied, Rescan pressed, a frame updated: %d engines, %d listings, %d selects, %d feeds, %d updates "
		            "(all must be 0)",
		            c1.engines - c0.engines, c1.lists - c0.lists, c1.selects - c0.selects, c1.feeds - c0.feeds, c1.updates - c0.updates ) );
		Check( spares && slots[ 0 ].value == 0.0f && slots[ 1 ].value == DefaultKey(),
		       fmt( "the menu before the mode is first on: %zu elements -- Off = 0, System default = deviceKey( \"(default)\" ), the rest distinct "
		            "negative spares; no device listed",
		            slots.size() ) );

		// 2. On, by a key a composition saved: the engine made, the devices listed
		// once, the device opened BY NAME.
		const float dante       = earshot::deviceKey( "Dante 1-2" );
		const bool menuChanged  = p.Apply( dante, false );
		const sing::Counters c2 = sing::CountersForTest();
		bool byName             = false;
		for( const sing::Slot& s : p.Slots() )
			if( s.name == "Dante 1-2" )
				byName = s.value == dante;
		const bool running = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Running; } );
		Check( menuChanged && c2.engines - c1.engines == 1 && c2.lists - c1.lists == 1 && c2.selects - c1.selects == 1 && p.Requested() == "Dante 1-2"
		           && byName && running,
		       fmt( "on with a saved key: %d engine, %d listing, %d select; '%s' requested, listed at deviceKey( its name ) (%s); Running (%s)",
		            c2.engines - c1.engines, c2.lists - c1.lists, c2.selects - c1.selects, p.Requested().c_str(), byName ? "yes" : "NO",
		            running ? "yes" : "NO" ) );
		p.Apply( dante, false );
		Check( sing::CountersForTest().selects == c2.selects, "the same value again selects nothing (a select would end a feed and reopen)" );

		// 3. The key earshot names devices by, written out for builds without it.
		bool keys = true;
		for( const char* name : { "(default)", "Bench Mic", "Dante 1-2", "MacBook Pro Microphone", "BlackHole 16ch", "" } )
			keys = keys && sing::KeyFor( name ) == earshot::deviceKey( name );
		Check( keys, "sing::KeyFor is earshot::deviceKey for six names (a build without earshot keys the same elements)" );

		// 4. Rescan finds a new device; one slow to open is Waiting (amber) first.
		{
			std::lock_guard< std::mutex > guard( g_script.lock );
			g_script.present.push_back( g_script.slowName );
		}
		const bool relisted = p.Apply( dante, true );
		const float slowKey = earshot::deviceKey( g_script.slowName.c_str() );
		bool listedSlow     = false;
		for( const sing::Slot& s : p.Slots() )
			listedSlow = listedSlow || ( s.name == g_script.slowName && s.value == slowKey );
		p.Apply( slowKey, false );
		// earshot's own thread opens it, so the frame that sees Opening is
		// whichever polls while it does: frames every 5 ms through its 0.3 s.
		const bool waiting     = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Waiting; } );
		const bool slowRunning = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Running; } );
		Check( relisted && listedSlow && waiting && slowRunning,
		       fmt( "Rescan lists a new device by name (%s); one %.1f s slow to open shows Waiting while it opens (%s), then Running (%s)",
		            listedSlow ? "yes" : "NO", g_script.openDelay, waiting ? "yes" : "NO", slowRunning ? "yes" : "NO" ) );

		// 5. A saved device that is not plugged in: Missing, never the default;
		// plugged in, Rescan opens it by name.
		const char* away = "Studio Interface";
		p.Apply( earshot::deviceKey( away ), false );
		const bool missing      = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Missing; } );
		const std::string asked = p.Requested();
		{
			std::lock_guard< std::mutex > guard( g_script.lock );
			g_script.present.push_back( away );
		}
		p.Apply( earshot::deviceKey( away ), true );
		const bool found = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Running; } );
		Check( missing && asked.rfind( "(missing device ", 0 ) == 0 && found && p.Requested() == away,
		       fmt( "'%s' saved but absent: Missing (%s), asking for '%s', never the default; plugged in, Rescan opens it by name (%s)", away,
		            missing ? "yes" : "NO", asked.c_str(), found ? "yes" : "NO" ) );

		// 6. A device delivering bit-exact zeros: Silent (red).
		p.Apply( earshot::deviceKey( g_script.silentName.c_str() ), false );
		const bool silent = UpdateUntil( p, t, []( const sing::Note& n ) { return n.lamp == sing::Lamp::Silent; } );
		Check( silent, fmt( "'%s' opens and delivers zeros: Silent after earshot's 3 s of them (%s)", g_script.silentName.c_str(), silent ? "yes" : "NO" ) );

		// 7. Off again: the device closed, the lamp gone, nothing listed.
		const int listsBefore = sing::CountersForTest().lists;
		p.Apply( 0.0f, false );
		p.Apply( 0.0f, true );
		t += 1.0 / 60.0;
		const sing::Note offAgain = p.Update( t, 0, -60.0 );
		Check( !offAgain.active && offAgain.lamp == sing::Lamp::Off && p.Requested().empty() && sing::CountersForTest().lists == listsBefore,
		       "Off again: the device closed (requested \"\"), no lamp, and Rescan while off lists nothing" );
	}
	{
		// 8. FLYBACK_NO_DEVICES: on, and still nothing listed or selected.
		ScopedNoDevices quiet;
		const sing::Counters before = sing::CountersForTest();
		sing::PitchInput q;
		q.Apply( DefaultKey(), false );
		q.Apply( DefaultKey(), true );
		double t                   = 0.0;
		const sing::Note waiting   = q.Update( t, 0, -60.0 );
		const sing::Counters after = sing::CountersForTest();
		Check( after.engines - before.engines == 1 && after.lists == before.lists && after.selects == before.selects && waiting.lamp == sing::Lamp::Waiting,
		       fmt( "FLYBACK_NO_DEVICES=1, the mode on and Rescan pressed: %d engine, %d listings, %d selects; the lamp Waiting (%s)",
		            after.engines - before.engines, after.lists - before.lists, after.selects - before.selects,
		            waiting.lamp == sing::Lamp::Waiting ? "yes" : "NO" ) );
	}
	{
		// 9. The lamp's colours: distinct by state; a blinking one unlit in its dark half.
		float run[ 3 ], sil[ 3 ], mis[ 3 ], misOff[ 3 ], wai[ 3 ], waiOff[ 3 ];
		LampPainter::Colour( sing::Lamp::Running, true, run );
		LampPainter::Colour( sing::Lamp::Silent, true, sil );
		LampPainter::Colour( sing::Lamp::Missing, true, mis );
		LampPainter::Colour( sing::Lamp::Missing, false, misOff );
		LampPainter::Colour( sing::Lamp::Waiting, true, wai );
		LampPainter::Colour( sing::Lamp::Waiting, false, waiOff );
		const bool green = run[ 1 ] > 2.0f * run[ 0 ] && run[ 1 ] > 2.0f * run[ 2 ];
		const bool red   = sil[ 0 ] > 5.0f * sil[ 1 ] && mis[ 0 ] == sil[ 0 ] && mis[ 1 ] == sil[ 1 ] && mis[ 2 ] == sil[ 2 ];
		const bool amber = wai[ 0 ] > wai[ 1 ] && wai[ 1 ] > 3.0f * wai[ 2 ];
		const bool dark  = misOff[ 0 ] == misOff[ 1 ] && misOff[ 1 ] == misOff[ 2 ] && waiOff[ 0 ] == misOff[ 0 ] && misOff[ 0 ] < 0.2f;
		Check( green && red && amber && dark, "the lamp's colours: green Running, red Silent, red blinking Missing, amber blinking Waiting, unlit grey "
		                                      "between blinks" );
	}
	sing::PitchInput::SetDefaultHooksForTest( 0 );
	ScriptDevices( { "Bench Mic", "Dante 1-2" } );
	return Verdict();
}

//===========================================================================
// --sing-picture
//===========================================================================
/// The emission buffer's light (before the glow), read back as floats: J.
double EmissionSum( Rig& rig )
{
	return rig.LightSum( nullptr, 0, true );
}

/// --light's bound on the emission's sum, relative (Shaders.h's closed form
/// summed at pixel centres; float stores), at the narrowest sigma.
double EmissionBound( Rig& rig )
{
	const double sigma = 0.8;
	const double phi45 = std::exp( -0.5 * 4.5 * 4.5 ) / std::sqrt( 2.0 * kPi );
	const int stores   = rig.plugin->RendererForTest().FloatStores();
	return 2.0 * 2.0 * std::exp( -2.0 * kPi * kPi * sigma * sigma ) + 4.5 * phi45 / ( 6.0 * sigma * sigma ) + 1.5e-7
	       + ( stores + 1 ) * std::ldexp( 1.0, -24 ) * 16.0;
}

/// A Tesla coil set up to be counted: no persistence, a full shutter, no
/// apparatus, nothing to strike, every streamer inside the frame.
bool SingRig( Rig& rig, const Size& size, bool over = false )
{
	if( !rig.Init( size.first, size.second, over ) )
		return false;
	rig.Set( PT_MACHINE, static_cast< float >( Machine::Tesla ) );
	rig.Set( PT_PERSISTENCE, 0.0f );
	rig.Set( PT_SHUTTER, 1.0f );
	rig.Set( PT_APPARATUS, 0.0f );
	rig.Set( PT_TARGET, 0.0f );
	rig.Set( PT_REACH, 0.2f );
	rig.Set( PT_SCALE, ParamFromRelative( 0.5, 4.0 ) );
	return true;
}

int runSingPicture( const Perturb& perturb )
{
	std::printf( "\n=== sing-picture: the picture holds one bang's light per period of the note (counted out of the pixels)\n" );
	ScopedNoDevices quiet;
	for( const Size& size : Rasters( { { 480, 270 }, { 960, 540 } } ) )
	{
		// One bang's light, out of the picture: Fire with BPS off and the mode off.
		double quantum = 0.0, bound = 0.0;
		{
			Rig rig;
			if( !SingRig( rig, size ) )
				return 1;
			rig.Set( PT_BPS, 0.0f );
			rig.Render( 3 );
			Engine& engine = rig.plugin->EngineForTest();
			engine.ClearEvents();
			rig.Press( PT_FIRE );
			rig.Render( 1 );
			int landed = 0;
			for( const Event& e : engine.Events() )
				landed += e.exposed ? 1 : 0;
			if( landed != 1 )
			{
				Check( false, fmt( "%dx%d: Fire landed %d bangs, not one", size.first, size.second, landed ) );
				continue;
			}
			quantum = EmissionSum( rig );
			bound   = EmissionBound( rig );
		}
		struct Case
		{
			double hz;
			int octaves;
		};
		for( const Case& c : { Case { 220.0, 0 }, Case { 1318.51, 0 }, Case { 110.0, 2 } } )
		{
			Rig rig;
			if( !SingRig( rig, size ) )
				return 1;
			rig.Set( PT_OCTAVE, sing::ParamFromOctave( c.octaves ) );
			rig.Set( PT_PITCH_SOURCE, DefaultKey() );
			const Signal tone = Tone( c.hz, -20.0, 3.0 );
			rig.audio.signal  = &tone;
			rig.Render( 12 );// the window's warm-up, and more
			Engine& engine = rig.plugin->EngineForTest();
			engine.ClearEvents();
			int pictureBangs = 0, frames = 0, unwhole = 0;
			double integral = 0.0, worst = 0.0;
			for( int i = 0; i < 90; ++i )
			{
				rig.Render( 1 );
				const double n     = EmissionSum( rig ) / quantum;
				const long counted = std::lround( n );
				const double off   = std::fabs( n - static_cast< double >( counted ) );
				// Each bang's light within --light's bound of the quantum, and the
				// quantum's own: twice the bound a bang.
				const double allowed = static_cast< double >( std::max( 1L, counted ) ) * 2.0 * bound;
				worst                = std::max( worst, off / static_cast< double >( std::max( 1L, counted ) ) );
				unwhole += off > allowed ? 1 : 0;
				pictureBangs += static_cast< int >( counted );
				integral += perturb.singPitchScale * rig.plugin->NoteForTest().rate / kFps;
				++frames;
			}
			int stateBangs = 0;
			for( const Event& e : engine.Events() )
				stateBangs += e.exposed ? 1 : 0;
			Check( unwhole == 0 && pictureBangs > 0 && std::fabs( pictureBangs - integral ) < 1.0 + 1e-9 * integral,
			       fmt( "%4dx%-4d %7.2f Hz %+d oct: the pixels hold %d bangs' light in %.1f s (every frame a whole number, worst %.1e of a "
			            "bang off, bound %.1e: --light's); the detected pitch's rate integrates to %.3f (within one: the interrupter's "
			            "phase); the engine logged %d",
			            size.first, size.second, c.hz, c.octaves, pictureBangs, frames / kFps, worst, 2.0 * bound, integral, stateBangs ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --off
//===========================================================================
/// What the engine handed the renderer, compared bit for bit: every segment,
/// every shape, the light and the count. (Segment and Shape are floats and an
/// int, no padding.)
bool SameFrame( const Frame& a, const Frame& b )
{
	return a.segments.size() == b.segments.size() && a.shapes.size() == b.shapes.size() && a.joules == b.joules && a.events == b.events
	       && a.sceneHeight == b.sceneHeight
	       && ( a.segments.empty() || std::memcmp( a.segments.data(), b.segments.data(), a.segments.size() * sizeof( Segment ) ) == 0 )
	       && ( a.shapes.empty() || std::memcmp( a.shapes.data(), b.shapes.data(), a.shapes.size() * sizeof( Shape ) ) == 0 );
}

/// Apple's software renderer does not reproduce its own output run to run
/// (Van de Graaff, 90 frames at 480x270: two different films in three runs of
/// one binary; the GPU gave one): a pixel comparison there says nothing.
bool ReproducibleRenderer()
{
	const GLubyte* name = glGetString( GL_RENDERER );
	return name == nullptr || std::strstr( reinterpret_cast< const char* >( name ), "Software" ) == nullptr;
}

int runOff( const Perturb& perturb )
{
	std::printf( "\n=== off: with Pitch Source Off every new control is inert, nothing reaches earshot, and the frame is the same\n" );
	const Size size = Rasters( { { 480, 270 } } ).front();
	sing::PitchInput::SetDefaultHooksForTest( perturb.pitchHooks );
	for( bool over : { false, true } )
	{
		const sing::Counters before = sing::CountersForTest();
		Rig a, b;
		if( !a.Init( size.first, size.second, over ) || !b.Init( size.first, size.second, over ) )
			return 1;
		const Signal tone = Tone( 220.0, -20.0, 3.0 );
		b.audio.signal    = &tone;// would be fed, if anything were listening
		int differing = 0, displays = 0, lamps = 0, frameDiffers = 0;
		for( int i = 0; i < 90; ++i )
		{
			b.Set( PT_OCTAVE, static_cast< float >( i % sing::kOctaveElements ) );
			b.Set( PT_GATE, ( i % 7 ) / 6.0f );
			if( i % 10 == 3 )
				b.Press( PT_RESCAN );
			if( perturb.singOn && i == 20 )
				b.Set( PT_PITCH_SOURCE, DefaultKey() );
			if( !a.Render( 1 ) || !b.Render( 1 ) )
				return 1;
			if( a.Output() != b.Output() )
				++differing;
			if( !SameFrame( a.plugin->LastFrameForTest(), b.plugin->LastFrameForTest() ) )
				++frameDiffers;
			lamps += b.plugin->LampForTest().drawn ? 1 : 0;
		}
		for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
			if( a.plugin->GetParamType( id ) != FF_TYPE_BUFFER && std::string( a.plugin->GetParameterDisplay( id ) ) != b.plugin->GetParameterDisplay( id ) )
				++displays;
		const sing::Counters after = sing::CountersForTest();
		const bool pixels          = ReproducibleRenderer();
		Check( frameDiffers == 0 && ( !pixels || differing == 0 ) && displays == 0 && lamps == 0,
		       fmt( "%s %dx%d, 90 frames: Octave through all seven, Gate swept, Rescan pressed, a tone ready to feed: the engine's frame "
		            "differs from the defaults' on %d, bit for bit; the pixels on %d%s; %d of the v0.1.0 controls display differently; a "
		            "lamp on %d",
		            over ? "effect" : "source", size.first, size.second, frameDiffers, differing,
		            pixels ? "" : " (not claimed: this renderer does not reproduce its own output)", displays, lamps ) );
		Check( after.engines == before.engines && after.lists == before.lists && after.selects == before.selects && after.feeds == before.feeds
		           && after.updates == before.updates && !b.plugin->PitchForTest().Started(),
		       fmt( "%s: %d earshot engines, %d device listings, %d selects, %d feeds, %d updates (CoreAudio is reached only through the "
		            "first three; all must be 0)",
		            over ? "effect" : "source", after.engines - before.engines, after.lists - before.lists, after.selects - before.selects,
		            after.feeds - before.feeds, after.updates - before.updates ) );
	}
	sing::PitchInput::SetDefaultHooksForTest( 0 );
	return Verdict();
}

//===========================================================================
// --lamp
//===========================================================================
int runLamp( const Perturb& perturb )
{
	std::printf( "\n=== lamp: the device's state in the corner while singing, in the colour the state names; the host's GL state kept\n" );
	ScriptDevices( { "Bench Mic", "Dante 1-2", g_script.silentName } );
	for( const Size& size : Rasters( { { 480, 270 }, { 1280, 720 } } ) )
		for( bool over : { false, true } )
		{
			Rig rig;
			if( !rig.Init( size.first, size.second, over ) )
				return 1;
			rig.Set( PT_MACHINE, static_cast< float >( Machine::Tesla ) );
			std::string line;
			auto near = [ & ]( sing::Lamp state, bool blinkOn, const char* what ) {
				float want[ 3 ];
				LampPainter::Colour( state, blinkOn, want );
				const LampPainter::Placement& p = rig.plugin->LampForTest();
				const Bytes out                 = rig.Output();
				const int x = static_cast< int >( std::floor( p.x ) ), y = static_cast< int >( std::floor( p.y ) );
				bool ok     = p.drawn && x >= 0 && y >= 0 && x < size.first && y < size.second;
				if( ok )
				{
					const unsigned char* q = &out[ ( static_cast< size_t >( y ) * size.first + x ) * 4 ];
					// The pixel whose centre is within 0.71 px of the disc's is fully
					// covered (coverage = radius - d + 0.5, radius >= 3), so it is the
					// colour itself, through an 8-bit store: half a step of rounding.
					// Allowed a whole step.
					for( int c = 0; c < 3; ++c )
						ok = ok && std::fabs( q[ c ] / 255.0f - want[ c ] ) <= 1.0f / 255.0f;
				}
				line += fmt( " %s %s;", what, ok ? "yes" : "NO" );
				return ok;
			};
			auto renderUntil = [ & ]( std::function< bool() > done ) {
				const auto start = std::chrono::steady_clock::now();
				while( !done() )
				{
					if( !rig.Render( 1 ) )
						return false;
					// A hang guard, not a measurement: Apple's software renderer
					// takes ~0.3 s a frame at 720p, and a blink is 15 frames.
					if( std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() > 60.0 )
						return false;
					std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
				}
				return true;// the frame just rendered drew the state just read
			};
			// Running: the system default, the scripted list's first device.
			rig.Set( PT_PITCH_SOURCE, DefaultKey() );
			const sing::Lamp expected = perturb.expectLamp >= 0 ? static_cast< sing::Lamp >( perturb.expectLamp ) : sing::Lamp::Running;
			bool ok = renderUntil( [ & ] { return rig.plugin->NoteForTest().lamp == sing::Lamp::Running; } ) && near( expected, true, "green Running" );
			// The host's option elements refilled: the devices by name, keyed by name.
			const unsigned elements = rig.plugin->GetNumParamElements( PT_PITCH_SOURCE );
			int keyed               = 0;
			for( unsigned e = 0; e < elements; ++e )
			{
				const FFMixed mixed = rig.plugin->GetParamElementDefault( PT_PITCH_SOURCE, e );
				float v             = 0.0f;
				std::memcpy( &v, &mixed.UIntValue, sizeof( v ) );
				const char* label      = rig.plugin->GetParamElementName( PT_PITCH_SOURCE, e );
				const std::string name = label ? label : "";
				if( name == "Bench Mic" || name == "Dante 1-2" || name == g_script.silentName )
					keyed += v == earshot::deviceKey( name.c_str() ) ? 1 : 0;
			}
			const bool menu = elements == static_cast< unsigned >( sing::kDeviceElements ) && keyed == 3;
			// Silent: red.
			rig.Set( PT_PITCH_SOURCE, earshot::deviceKey( g_script.silentName.c_str() ) );
			ok = renderUntil( [ & ] { return rig.plugin->NoteForTest().lamp == sing::Lamp::Silent; } ) && near( sing::Lamp::Silent, true, "red Silent" ) && ok;
			// Missing: red, blinking at 2 Hz on the host's clock.
			rig.Set( PT_PITCH_SOURCE, earshot::deviceKey( "Nowhere" ) );
			ok = renderUntil( [ & ] {
				     const sing::Note& n = rig.plugin->NoteForTest();
				     return n.lamp == sing::Lamp::Missing && n.blinkOn;
			     } ) && near( sing::Lamp::Missing, true, "red Missing" ) && ok;
			ok = renderUntil( [ & ] { return !rig.plugin->NoteForTest().blinkOn; } ) && near( sing::Lamp::Missing, false, "unlit between blinks" ) && ok;
			const LampPainter::Placement p = rig.plugin->LampForTest();
			const bool placed = p.radius >= 3.0f && p.x - 1.6f * p.radius >= 0.0f && p.y + 1.6f * p.radius <= static_cast< float >( size.second );
			Check( ok && menu && placed, fmt( "%s %4dx%-4d radius %.1f px at (%.1f, %.1f), bezel inside the frame (%s); the menu refilled by name (%s);%s",
			                                  over ? "effect" : "source", size.first, size.second, p.radius, p.x, p.y, placed ? "yes" : "NO",
			                                  menu ? "yes" : "NO", line.c_str() ) );

			// The GL state around a frame that drew the lamp: --state's list.
			GLuint hostArray = 0;
			glGenVertexArrays( 1, &hostArray );
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, size.first, size.second );
			glBindVertexArray( hostArray );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, size.first, size.second );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin->SetTime( rig.origin + rig.frame / kFps );
			++rig.frame;
			if( rig.plugin->ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0, srcA = 0, dstA = 0, arrayBuffer = 0;
			GLfloat clear[ 4 ] = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetIntegerv( GL_BLEND_SRC_ALPHA, &srcA );
			glGetIntegerv( GL_BLEND_DST_ALPHA, &dstA );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &arrayBuffer );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			// The host's blend, or (the negative) the lamp's own left behind.
			const GLint wantSrc  = perturb.expectLeak ? GL_ONE : GL_SRC_ALPHA;
			const GLint wantDstA = perturb.expectLeak ? GL_ONE_MINUS_SRC_ALPHA : GL_ZERO;
			std::string wrong;
			auto expect = [ & ]( bool good, const char* name ) {
				if( !good )
					wrong += std::string( " " ) + name;
			};
			expect( rig.plugin->LampForTest().drawn, "lamp-not-drawn" );
			expect( viewport[ 2 ] == size.first && viewport[ 3 ] == size.second, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == wantSrc && dst == GL_ONE_MINUS_SRC_ALPHA && srcA == GL_ONE && dstA == wantDstA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( arrayBuffer == 0, "buffers" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
			glDisable( GL_SCISSOR_TEST );
			glDisable( GL_BLEND );
			glBindVertexArray( 0 );
			glDeleteVertexArrays( 1, &hostArray );
			Check( wrong.empty(), fmt( "%s %4dx%-4d a frame that drew the lamp: viewport, vertex array, program, active unit, framebuffer, blend "
			                           "(colour and alpha), scissor, clear colour, buffers and eight texture units as the host left them (wrong:%s)",
			                           over ? "effect" : "source", size.first, size.second, wrong.empty() ? " none" : wrong.c_str() ) );
		}
	ScriptDevices( { "Bench Mic", "Dante 1-2" } );
	return Verdict();
}

//===========================================================================
// --resize-sing
//===========================================================================
int runResizeSing( const Perturb& perturb )
{
	std::printf( "\n=== resize-sing: a new raster and a lost GL context mid-song keep the pitch input and the interrupter's phase\n" );
	ScopedNoDevices quiet;
	const Size size = Rasters( { { 480, 270 } } ).front();
	const Size big  = { size.first * 4 / 3, size.second * 4 / 3 };
	Rig rig;
	if( !SingRig( rig, size ) )
		return 1;
	rig.Set( PT_PITCH_SOURCE, DefaultKey() );
	const Signal tone = Tone( 220.0, -20.0, 6.0 );
	rig.audio.signal  = &tone;
	rig.Render( 12 );
	const sing::Counters c0 = sing::CountersForTest();
	const uint32_t gen0     = rig.plugin->NoteForTest().generation;
	Engine& engine          = rig.plugin->EngineForTest();
	engine.ClearEvents();
	double integral = 0.0;
	int lamps = 0, frames = 0;
	auto sing30 = [ & ]() {
		for( int i = 0; i < 30; ++i )
		{
			if( !rig.Render( 1 ) )
				return false;
			integral += rig.plugin->NoteForTest().rate / kFps;
			lamps += rig.plugin->LampForTest().drawn ? 1 : 0;
			++frames;
		}
		return true;
	};
	if( !sing30() )
		return 1;
	// A new raster: the host's framebuffer reallocated at 4/3 the size.
	if( !rig.Resize( big.first, big.second ) || !sing30() )
		return 1;
	// The context lost and found: DeInitGL, InitGL.
	rig.plugin->DeInitGL();
	FFGLViewportStruct viewport = {};
	viewport.width              = static_cast< FFUInt32 >( big.first );
	viewport.height             = static_cast< FFUInt32 >( big.second );
	if( rig.plugin->InitGL( &viewport ) != FF_SUCCESS )
		return 1;
	if( perturb.recreateOnInit )
		rig.plugin->PitchForTest().ResetForTest();
	if( !sing30() )
		return 1;
	int bangs = 0;
	for( const Event& e : engine.Events() )
		bangs += e.exposed ? 1 : 0;
	const sing::Counters c1 = sing::CountersForTest();
	const uint32_t gen1     = rig.plugin->NoteForTest().generation;
	Check( c1.engines == c0.engines && c1.feeds == c0.feeds && c1.selects == c0.selects && c1.lists == c0.lists && gen1 == gen0,
	       fmt( "%dx%d -> %dx%d -> DeInitGL/InitGL: %d new earshot engines, %d new feeds, %d selects, %d listings; stream generation %u -> %u",
	            size.first, size.second, big.first, big.second, c1.engines - c0.engines, c1.feeds - c0.feeds, c1.selects - c0.selects,
	            c1.lists - c0.lists, gen0, gen1 ) );
	Check( std::fabs( bangs - integral ) < 1.0 + 1e-9 * integral && lamps == frames,
	       fmt( "the interrupter across both: %d bangs in %d frames where the rate integrates to %.3f (within one); the lamp on %d of them", bangs,
	            frames, integral, lamps ) );
	return Verdict();
}

//===========================================================================
// --demo-music DIR: a report, not a check
//===========================================================================
/// Every .wav in DIR through the pitch input and the sung interrupter, under
/// the law and under "voiced frames only": how often the coil sounds, at what,
/// and in runs how long. This is how the law was chosen (AGENTS.md).
int runDemoMusic( const std::string& dir )
{
	std::printf( "\n=== demo-music: %s\n", dir.c_str() );
	std::vector< std::string > files;
	if( FILE* list = popen( ( "/bin/ls -1 \"" + dir + "\"" ).c_str(), "r" ) )
	{
		char name[ 1024 ];
		while( std::fgets( name, sizeof( name ), list ) )
		{
			std::string n = name;
			while( !n.empty() && ( n.back() == '\n' || n.back() == '\r' ) )
				n.pop_back();
			if( n.size() > 4 && ( n.substr( n.size() - 4 ) == ".wav" || n.substr( n.size() - 4 ) == ".WAV" ) )
				files.push_back( n );
		}
		pclose( list );
	}
	if( files.empty() )
	{
		std::fprintf( stderr, "no .wav files in %s\n", dir.c_str() );
		return 2;
	}
	Settings base = MachineSettings( Machine::Tesla );
	base.reach    = 0.25;
	std::printf( "  %-14s %6s %7s %7s %8s %8s %9s %9s %9s %8s\n", "clip", "secs", "voiced", "below", "sounds", "sounds", "run", "run", "median",
	             "bangs" );
	std::printf( "  %-14s %6s %7s %7s %8s %8s %9s %9s %9s %8s\n", "", "", "%", "gate %", "law %", "voiced %", "law fr", "voiced fr", "BPS",
	             "law" );
	for( const std::string& name : files )
	{
		Signal s;
		std::string error;
		if( !ReadWav( dir + "/" + name, s, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 1;
		}
		Perturb law, voicedOnly;
		voicedOnly.pitchHooks = sing::kHookVoicedOnly;
		const SungRun a       = SingEngine( s, base, 0, -60.0, 60.0, 0.0, law );
		const SungRun b       = SingEngine( s, base, 0, -60.0, 60.0, 0.0, voicedOnly );
		auto summarise        = []( const SungRun& run, int& sounding, double& meanRun, std::vector< double >* rates ) {
            sounding = 0;
            int runs = 0, length = 0;
            for( const SungFrame& f : run.frames )
                if( f.note.rate > 0.0 )
                {
                    ++sounding;
                    ++length;
                    if( rates )
                        rates->push_back( f.note.rate );
                }
                else if( length > 0 )
                {
                    ++runs;
                    length = 0;
                }
            if( length > 0 )
                ++runs;
            meanRun = runs ? static_cast< double >( sounding ) / runs : 0.0;
		};
		int soundA = 0, soundB = 0, voiced = 0, below = 0;
		double runA = 0.0, runB = 0.0;
		std::vector< double > rates;
		summarise( a, soundA, runA, &rates );
		summarise( b, soundB, runB, nullptr );
		for( const SungFrame& f : a.frames )
		{
			voiced += f.note.voiced ? 1 : 0;
			below += f.note.levelDb < -60.0 ? 1 : 0;
		}
		std::sort( rates.begin(), rates.end() );
		const double median = rates.empty() ? 0.0 : rates[ rates.size() / 2 ];
		const double n      = static_cast< double >( a.frames.size() );
		std::printf( "  %-14s %6.2f %7.1f %7.1f %8.1f %8.1f %9.1f %9.1f %9.2f %8zu\n", name.substr( 0, 14 ).c_str(), s.Seconds(), 100.0 * voiced / n,
		             100.0 * below / n, 100.0 * soundA / n, 100.0 * soundB / n, runA, runB, median, a.events.size() );
	}
	std::printf( "\n  60 fps, Gate -60 dBFS, Octave 0. voiced: earshot's clarity >= 0.80 and level >= the gate. below gate: frames whose\n"
	             "  window is under -60 dBFS (a frame with no analysis counts as below). sounds: frames the coil fires, under the law\n"
	             "  (voiced sets, above-gate sustains, a rest forgets) and with voiced frames only. run: the mean sounding run, frames.\n" );
	return 0;
}

//===========================================================================
// --trap-probe: the CoreAudio trap's negative control
//===========================================================================
/// verify.sh runs the mode-off checks under libcatrap (tools/catrap), which
/// logs and refuses every CoreAudio dlopen and call. A trap that cannot fire
/// proves nothing, so this reaches CoreAudio three ways -- earshot's
/// listDevices (miniaudio, dlopen), a direct AudioObject call (load-time
/// binding), and a pitch input that lists its devices at construction -- and
/// verify.sh requires the trap's log to name them. It refuses to run without
/// the trap: on its own it would really ask the OS for devices.
int runTrapProbe()
{
	if( dlsym( RTLD_DEFAULT, "catrap_loaded" ) == nullptr )
	{
		std::fprintf( stderr, "hvtest --trap-probe: the CoreAudio trap is not loaded (DYLD_INSERT_LIBRARIES=.../libcatrap.dylib); "
		                      "refusing to touch CoreAudio without it\n" );
		return 2;
	}
	std::printf( "trap probe: three ways a build could reach CoreAudio; the trap must log each\n" );
	const std::vector< earshot::DeviceInfo > devices = earshot::listDevices();
	std::printf( "  earshot::listDevices: %zu devices\n", devices.size() );
	AudioObjectPropertyAddress address = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	UInt32 bytes                       = 0;
	const OSStatus status              = AudioObjectGetPropertyDataSize( kAudioObjectSystemObject, &address, 0, nullptr, &bytes );
	std::printf( "  AudioObjectGetPropertyDataSize: status %d\n", static_cast< int >( status ) );
	sing::PitchInput::SetBackendFactoryForTest( nullptr );
	sing::PitchInput::SetDefaultHooksForTest( sing::kHookEagerDevices );
	{
		sing::PitchInput eager;
		std::printf( "  a pitch input that lists at construction: started %s, %zu menu elements\n", eager.Started() ? "yes" : "no", eager.Slots().size() );
	}
	sing::PitchInput::SetDefaultHooksForTest( 0 );
	return 0;
}
