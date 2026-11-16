// --tempo: pattern v0.1.1's three-register detector, fed from earshot's flux.

#include "Harness.h"
#include "Signals.h"

#include <algorithm>
#include <cmath>

namespace estest
{

/*
	tempo.<groove> -- each groove (Signals.cpp: four-on-the-floor at 125 with
	the kick 10 dB over the clap, a bare kick at 128, a break at 96, drum and
	bass at 174, a triplet shuffle at 110) for 16 s. Two claims:
	  - settled by 6 s (the detector needs 3 s of envelope and two agreeing
	    readings a second apart, so about 4 s; 6 s allows two more readings);
	  - from 6 s, EVERY settled frame within 1 BPM of the notated tempo.
	Why 1 BPM: the detector reads lags on a 100 Hz grid, refined at up to
	four multiples of the lag. Before refinement the error is at most half a
	cell at four lags: 0.5 / (4 x 6000 / BPM), which is 0.33 BPM at 125 and
	0.63 BPM at 174 (the fastest groove here). The parabola only shrinks it,
	so 1 BPM bounds the unrefined error for every groove. Octave errors (62,
	250, 87) are off by tens of BPM and cannot hide in it.
	Negative control: pattern v0.1.0's detector (raw summed flux, best lag,
	half at 70 %), which read 62 on its release video's 125.

	tempo.fps -- the four-on-the-floor at 24, 30 and 60 fps: the same reading
	(the envelope is built from audio hops, not frames). Same limits.

	tempo.noise -- 16 s of white noise and 16 s of a held chord: nothing
	repeats, so nothing may settle. A detector that always answers is not a
	detector. Negative control: no onset gate (the chord settled at 167 BPM).
*/
namespace
{
struct TempoRun
{
	double settledAt = -1.0;
	int    readings  = 0, inside = 0;
	double worst     = 0.0;
	double last      = 0.0;
	double confidence = 0.0;
	bool   everSettled = false;
};

TempoRun runTempo( const Run& run, const Signal& s, const Mutations& mut, double expected )
{
	TempoRun r;
	Driver   d( run, Config(), mut );
	d.play( widen( s, 2 ), [ & ]( const Frame& f, double vt ) {
		if( !f.tempo.settled )
			return;
		r.everSettled = true;
		if( r.settledAt < 0.0 )
			r.settledAt = vt;
		r.last       = f.tempo.bpm;
		r.confidence = f.tempo.confidence;
		if( vt >= 6.0 && expected > 0.0 )
		{
			++r.readings;
			const double e = std::fabs( f.tempo.bpm - expected );
			r.worst        = std::max( r.worst, e );
			r.inside += e <= 1.0 ? 1 : 0;
		}
	} );
	return r;
}

bool good( const TempoRun& r ) { return r.settledAt >= 0.0 && r.settledAt <= 6.0 && r.readings > 0 && r.inside == r.readings; }
} // namespace

void checkTempo( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();

	for( const Groove& g : grooves() )
	{
		const Signal   s = render( g, run.fs, 16.0, 11 );
		const TempoRun r = runTempo( run, s, mut, g.expected );
		rep.add( "tempo." + g.name, cell, good( r ),
				 "notated %.0f: settled at %.2f s (limit 6), from 6 s %d of %d frames within 1 BPM, worst %.2f off, last %.2f BPM (confidence %.2f)",
				 g.expected, r.settledAt, r.inside, r.readings, r.worst, r.last, r.confidence );
		rep.figure( "tempo " + g.name + ": worst BPM error from 6 s", r.worst );
		rep.figure( "tempo " + g.name + ": settled at (s)", r.settledAt );
	}

	//-- Frame rate does not move it ----------------------------------------
	{
		const Groove& g = grooves()[ 0 ];
		const Signal  s = render( g, run.fs, 16.0, 11 );
		bool          ok = true;
		std::string   detail;
		for( double fps : { 24.0, 30.0, 60.0 } )
		{
			Run r2 = run;
			r2.fps = fps;
			const TempoRun r = runTempo( r2, s, mut, g.expected );
			ok               = ok && good( r );
			detail += format( " %.0f fps: %.2f BPM (worst %.2f, settled %.2f s);", fps, r.last, r.worst, r.settledAt );
		}
		rep.add( "tempo.fps", cell, ok, "%s at three frame rates:%s", g.name.c_str(), detail.c_str() );
	}

	//-- Nothing periodic, nothing settled ------------------------------------
	{
		Signal noise = silence( run.fs, 16.0 );
		addNoise( noise, 0.1, 31 );
		Signal chord = silence( run.fs, 16.0 );
		for( double hz : { 130.81, 164.81, 196.0, 261.63 } )
			addSine( chord, hz, 0.1, hz );
		const TempoRun a = runTempo( run, noise, mut, 0.0 );
		const TempoRun b = runTempo( run, chord, mut, 0.0 );
		rep.add( "tempo.noise", cell, !a.everSettled && !b.everSettled,
				 "white noise: %s (confidence %.2f); held chord: %s (confidence %.2f)",
				 a.everSettled ? format( "settled at %.2f BPM", a.last ).c_str() : "never settled", a.confidence,
				 b.everSettled ? format( "settled at %.2f BPM", b.last ).c_str() : "never settled", b.confidence );
	}
}

} // namespace estest
