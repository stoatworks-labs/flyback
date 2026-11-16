// --silent, --ring, --params, --fallback, --capture, --alloc

#include "Harness.h"
#include "Signals.h"

#include "Ring.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace estest
{

using earshot::CaptureState;
using earshot::Source;
using earshot::Status;
namespace et = earshot::testing;

namespace
{
double wallSeconds()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}

void sleepMs( double ms )
{
	std::this_thread::sleep_for( std::chrono::microseconds( (long long)( ms * 1000.0 ) ) );
}

//---------------------------------------------------------------------------
// A scripted capture backend: the device as the worker sees it, minus the OS.
//---------------------------------------------------------------------------

struct Script
{
	double                           openDelay = 0.0;   ///< seconds open() blocks
	std::vector< et::OpenResult >    results;           ///< one per open(); the last repeats
	int                              rate      = 48000;
	int                              channels  = 2;
	bool                             zeros     = false; ///< deliver bit-exact zeros
	std::string                      knownName = "Scripted A";

	// Observations (shared with the test, which outlives nothing: atomics).
	std::atomic< int >      opens{ 0 }, closes{ 0 };
	std::atomic< bool >     askedDefault{ false };
	std::atomic< uint64_t > deliverAllocations{ 0 };
	std::atomic< bool >     loseNow{ false };
	std::atomic< bool >     destroyedOpen{ false };   ///< the backend was destroyed with its stream running
	std::mutex              namesLock;
	std::vector< std::string > names;
};

class ScriptedBackend final : public et::CaptureBackend
{
public:
	explicit ScriptedBackend( std::shared_ptr< Script > s ) : s_( std::move( s ) ) {}
	/// Records, rather than hides, a device still running when the backend is
	/// destroyed: the Engine must have closed it first (the device callback
	/// writes into the Engine). It is then stopped, so the test can go on.
	~ScriptedBackend() override
	{
		if( thread_.joinable() )
			s_->destroyedOpen = true;
		close();
	}

	et::OpenResult open( const char* name, et::AudioSink& sink, char* openedName, char* error ) noexcept override
	{
		const int n = s_->opens.fetch_add( 1 );
		{
			std::lock_guard< std::mutex > g( s_->namesLock );
			s_->names.push_back( name );
		}
		if( std::strcmp( name, earshot::kDefaultDevice ) == 0 )
			s_->askedDefault = true;
		if( s_->openDelay > 0.0 )
			sleepMs( s_->openDelay * 1000.0 );
		const et::OpenResult r = s_->results.empty() ? et::OpenResult::Opened
													  : s_->results[ (size_t)std::min< int >( n, (int)s_->results.size() - 1 ) ];
		if( r == et::OpenResult::NotConnected )
		{
			std::snprintf( error, 128, "\"%s\" is not connected", name );
			return r;
		}
		if( r == et::OpenResult::Failed )
		{
			std::snprintf( error, 128, "scripted refusal" );
			return r;
		}
		std::snprintf( openedName, 128, "%s", name );
		sink.begin( s_->rate, s_->channels );
		run_  = true;
		sink_ = &sink;
		thread_ = std::thread( [ this ] { pump(); } );
		return r;
	}

	void close() noexcept override
	{
		if( thread_.joinable() )
		{
			run_ = false;
			thread_.join();
			s_->closes++;
		}
	}

	std::vector< earshot::DeviceInfo > enumerate() noexcept override
	{
		return { earshot::DeviceInfo{ s_->knownName, s_->channels, false } };
	}

private:
	/// The device thread: real-time blocks of 256 frames.
	void pump()
	{
		std::vector< float > buf( 256 * (size_t)s_->channels );
		const double         t0 = wallSeconds();
		uint64_t             sent = 0;
		double               phase = 0.0;
		while( run_ )
		{
			if( s_->loseNow.exchange( false ) )
			{
				sink_->lost();   // unplugged: no more callbacks
				while( run_ )
					sleepMs( 1 );
				return;
			}
			const uint64_t due = (uint64_t)( ( wallSeconds() - t0 ) * s_->rate );
			while( sent + 256 <= due && run_ )
			{
				for( int i = 0; i < 256; ++i )
				{
					const float v = s_->zeros ? 0.0f : (float)( 0.3 * std::sin( phase ) );
					phase += 2.0 * kPi * 220.0 / s_->rate;
					for( int c = 0; c < s_->channels; ++c )
						buf[ (size_t)i * (size_t)s_->channels + (size_t)c ] = v;
				}
				startCountingAllocations();
				sink_->deliver( buf.data(), 256, s_->channels );
				s_->deliverAllocations += stopCountingAllocations();
				sent += 256;
			}
			sleepMs( 2 );
		}
	}

	std::shared_ptr< Script > s_;
	et::AudioSink*            sink_ = nullptr;
	std::atomic< bool >       run_{ false };
	std::thread               thread_;
};

/// Steady host bins: a falling spectrum with +-10 % jitter per bin per frame.
struct HostBinsGen
{
	Rng   rng{ 5 };
	float bins[ 64 ];
	const float* next( double jitter = 0.1 )
	{
		for( int j = 0; j < 64; ++j )
			bins[ j ] = (float)( 0.3 / ( 1.0 + 0.2 * j ) * ( 1.0 + jitter * ( 2.0 * rng.uniform() - 1.0 ) ) );
		return bins;
	}
};

uint32_t onsetTotal( const Frame& f )
{
	uint32_t n = 0;
	for( int b = 0; b < f.bandCount; ++b )
		n += f.bands[ b ].onsetCount;
	return n;
}
} // namespace

//---------------------------------------------------------------------------
// Silent. spasis's rule: open, running and bit-exactly zero for 3 s.
//
// silent.zero -- digital silence: Running until 3 s of audio, Silent from 3 s
//   plus one block plus one frame at most. Negative control: never Silent.
// silent.quiet -- noise at -120 dBFS (rms 1e-6) for 5 s is a quiet passage,
//   not a dead device: never Silent. Negative control: a level threshold
//   (|x| < 1e-4) instead of bit-exact zero.
// silent.recovery -- 4 s of zeros (Silent), then a tone: Running on the
//   first frame whose audio holds it. Negative control: Silent latches.
// silent.raw -- four channels, the pair on 0 and 1, both zero, a tone on
//   channel 3: judged on the raw device buffer, so never Silent. Negative
//   control: zeros judged on the kept channels only.
//---------------------------------------------------------------------------

void checkSilent( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const double      slack = (double)run.block / run.fs + 1.0 / run.fps + 1e-9;

	{
		Driver d( run, Config(), mut );
		double firstSilent = -1.0, lastRunning = -1.0;
		d.play( silence( run.fs, 4.0, 2 ), [ & ]( const Frame& f, double vt ) {
			if( f.capture == CaptureState::Silent && firstSilent < 0.0 )
				firstSilent = vt;
			if( f.capture == CaptureState::Running )
				lastRunning = vt;
		} );
		const bool ok = firstSilent >= 3.0 && firstSilent <= 3.0 + slack && lastRunning < 3.0 + slack;
		rep.add( "silent.zero", cell, ok, "digital silence: Silent from %.3f s (want 3.000..%.3f), last Running frame %.3f s", firstSilent,
				 3.0 + slack, lastRunning );
		rep.figure( "silent: zeros to Silent (s)", firstSilent );
	}
	{
		Signal s = silence( run.fs, 5.0, 2 );
		addNoise( s, 1e-6, 51 );
		Driver d( run, Config(), mut );
		int    silent = 0;
		d.play( s, [ & ]( const Frame& f, double ) { silent += f.capture == CaptureState::Silent ? 1 : 0; } );
		rep.add( "silent.quiet", cell, silent == 0, "noise at -120 dBFS for 5 s: %d Silent frames (must be 0)", silent );
	}
	{
		Driver d( run, Config(), mut );
		bool   wasSilent = false;
		d.play( silence( run.fs, 4.0, 2 ), [ & ]( const Frame& f, double ) { wasSilent = f.capture == CaptureState::Silent; } );
		Signal tone = silence( run.fs, 0.5, 2 );
		addSine( tone, 440.0, 0.1 );
		double back = -1.0;
		const double t0 = d.fedSeconds();
		d.resume( tone, [ & ]( const Frame& f, double vt ) {
			if( back < 0.0 && f.capture == CaptureState::Running )
				back = vt - t0;
		} );
		const bool ok = wasSilent && back >= 0.0 && back <= slack;
		rep.add( "silent.recovery", cell, ok, "Silent after 4 s of zeros: %s; Running again %.3f s after the tone starts (limit %.3f)",
				 wasSilent ? "yes" : "NO", back, slack );
	}
	{
		Signal s = silence( run.fs, 4.0, 4 );
		addSine( s, 440.0, 0.1, 0.0, 3 );
		Config cfg;
		cfg.maxChannels = 2;
		Driver d( run, cfg, mut );
		int    silent = 0;
		d.play( s, [ & ]( const Frame& f, double ) { silent += f.capture == CaptureState::Silent ? 1 : 0; } );
		rep.add( "silent.raw", cell, silent == 0, "4 channels, pair 0-1 zero, tone on 3: %d Silent frames (must be 0)", silent );
	}
}

//---------------------------------------------------------------------------
// Ring. The seqlock ring with a real producer thread racing the reader.
//
// ring.race -- a writer thread streams frames whose samples encode their own
//   index (blocks of 1..700 frames, 0.3 s flat out); the reader takes windows
//   anywhere in the readable range. Every accepted read must hold exactly
//   the frames asked for. A reset makes a reader's old epoch fail, even
//   once the same frame numbers have been written again (so only the epoch
//   can refuse it). Negative control: the epoch is not checked.
// ring.lapped -- the reader aims at the oldest frames, which the writer is
//   about to overwrite. Some reads must be refused, and no accepted read may
//   be torn. Negative control: trust the copy without re-checking the claim.
//---------------------------------------------------------------------------

namespace
{
struct RingResult
{
	uint64_t accepted = 0, refused = 0, torn = 0, written = 0;
};

RingResult raceRing( bool lapped, bool validate )
{
	earshot::Ring ring;
	ring.allocate( 2, 4096 );
	ring.reset( 2 );
	std::atomic< bool > stop{ false };
	std::thread writer( [ & ] {
		Rng                  rng( 9 );
		std::vector< float > buf( 700 * 2 );
		uint64_t             i = 0;
		while( !stop.load( std::memory_order_relaxed ) )
		{
			const int n = 1 + (int)( rng.uniform() * 700 );
			for( int f = 0; f < n; ++f )
			{
				const float v      = (float)( ( i + (uint64_t)f ) & 0xFFFFFF );
				buf[ (size_t)f * 2 ] = v;
				buf[ (size_t)f * 2 + 1 ] = -v;
			}
			ring.write( buf.data(), (size_t)n );
			i += (uint64_t)n;
		}
	} );
	RingResult           r;
	Rng                  rng( 10 );
	std::vector< float > out( 2048 * 2 );
	const uint32_t       epoch = ring.epoch();
	const double         t0    = wallSeconds();
	while( wallSeconds() - t0 < 0.3 )
	{
		const uint64_t w   = ring.written();
		const size_t   len = 256 + (size_t)( rng.uniform() * 1792 );
		if( w < 4096 + len )
			continue;
		const uint64_t start = lapped ? w - 4096 + 8 : w - len - (uint64_t)( rng.uniform() * ( 4096 - len ) );
		if( !ring.read( start, len, out.data(), epoch, validate ) )
		{
			++r.refused;
			continue;
		}
		++r.accepted;
		for( size_t f = 0; f < len; ++f )
		{
			const float want = (float)( ( start + f ) & 0xFFFFFF );
			if( out[ f * 2 ] != want || out[ f * 2 + 1 ] != -want )
			{
				++r.torn;
				break;
			}
		}
	}
	stop = true;
	writer.join();
	r.written = ring.written();
	return r;
}
} // namespace

void checkRing( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell     = "threads";
	const bool        validate = !mut.ringNoValidate;
	(void)run;
	{
		const RingResult r = raceRing( false, validate );
		// The epoch: a reset invalidates every reader's view.
		earshot::Ring ring;
		ring.allocate( 2, 1024 );
		ring.reset( 2 );
		float buf[ 64 * 2 ] = {};
		ring.write( buf, 64 );
		const uint32_t e = ring.epoch();
		const bool     before = ring.read( 0, 64, buf, e, validate, !mut.ringNoEpoch );
		ring.reset( 2 );
		ring.write( buf, 64 );   // the same frame numbers exist again: only the epoch tells them apart
		const bool after    = ring.read( 0, 64, buf, e, validate, !mut.ringNoEpoch );
		const bool afterNew = ring.read( 0, 64, buf, ring.epoch(), validate, !mut.ringNoEpoch );
		const bool ok       = r.torn == 0 && r.accepted > 1000 && before && !after && afterNew;
		rep.add( "ring.race", cell, ok,
				 "%llu frames written by a racing thread; %llu reads accepted, %llu torn (must be 0), %llu refused; old epoch after reset: %s",
				 (unsigned long long)r.written, (unsigned long long)r.accepted, (unsigned long long)r.torn, (unsigned long long)r.refused,
				 after ? "ACCEPTED" : ( afterNew ? "refused" : "refused, but so is the new one" ) );
	}
	{
		const RingResult r  = raceRing( true, validate );
		const bool       ok = r.torn == 0 && r.refused > 0 && r.accepted + r.refused > 1000;
		rep.add( "ring.lapped", cell, ok, "reading the oldest frames while they are overwritten: %llu accepted, %llu torn (must be 0), %llu refused (must be > 0)",
				 (unsigned long long)r.accepted, (unsigned long long)r.torn, (unsigned long long)r.refused );
	}
}

//---------------------------------------------------------------------------
// Params. spasis: a parameter change must not clear the ring.
//
// params.keep -- a groove with a held tone for 8 s (tempo settled), then
//   configure() with every runtime field changed. On the next update with
//   no new audio, the newest window (peekWaveform) is the same samples as
//   before and the generation does not move; then 2 s more audio: tempo
//   stays settled on every frame, and every frame's pitch is identical to a
//   run configured that way from the start (so the pitch window was full of
//   history at once, not refilling). Negative control: configure() clears
//   the ring and re-primes.
//---------------------------------------------------------------------------

void checkParams( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const Groove&     g    = grooves()[ 0 ];
	Signal            s    = render( g, run.fs, 10.0, 3 );
	addSine( s, 220.0, 0.3 );
	Signal first = silence( run.fs, 8.0 ), rest = silence( run.fs, 2.0 );
	std::copy( s.x.begin(), s.x.begin() + (long)first.x.size(), first.x.begin() );
	std::copy( s.x.begin() + (long)first.x.size(), s.x.begin() + (long)( first.x.size() + rest.x.size() ), rest.x.begin() );

	Config c;
	c.pairFirst      = 0;
	c.pitchMinHz     = 50.0f;
	c.pitchMaxHz     = 1500.0f;
	c.pitchClarity   = 0.75f;
	c.bandCount      = 3;
	c.bandEdges[ 0 ] = 40.0f;
	c.bandEdges[ 1 ] = 200.0f;
	c.bandEdges[ 2 ] = 2000.0f;
	c.bandEdges[ 3 ] = 12000.0f;
	c.onsetDeviations  = 10.0f;
	c.envelopeRelease  = 0.3f;
	c.thirdHoldSeconds = 2.0f;
	c.tapRate          = 11025;

	// The control: the same audio analysed with the new settings from the start.
	std::vector< earshot::Pitch > want;
	{
		Driver b( run, c, mut );
		b.play( widen( first, 2 ), []( const Frame&, double ) {} );
		b.resume( widen( rest, 2 ), [ & ]( const Frame& f, double ) { want.push_back( f.pitch ); } );
	}

	Driver d( run, Config(), mut );
	bool   settledBefore = false;
	d.play( widen( first, 2 ), [ & ]( const Frame& f, double ) { settledBefore = f.tempo.settled; } );
	Engine&              e = d.engine();
	const int            n = 1024;
	std::vector< float > l0( n ), r0( n ), l1( n ), r1( n );
	const bool           had = e.peekWaveform( l0.data(), r0.data(), n );
	const uint32_t       gen = e.frame().generation;

	e.configure( c );
	e.update( d.fedSeconds() + 0.001, nullptr, 0 );
	const bool     has    = e.peekWaveform( l1.data(), r1.data(), n );
	const bool     same   = had && has && l0 == l1 && r0 == r1;
	const uint32_t genNow = e.frame().generation;

	int    lostTempo = 0, pitchDiffers = 0, frames = 0;
	size_t k         = 0;
	d.resume( widen( rest, 2 ), [ & ]( const Frame& f, double ) {
		++frames;
		lostTempo += f.tempo.settled ? 0 : 1;
		if( k < want.size() )
		{
			const auto& w = want[ k++ ];
			pitchDiffers += ( f.pitch.voiced != w.voiced || std::fabs( f.pitch.hz - w.hz ) > 1e-3f ) ? 1 : 0;
		}
	} );
	const bool applied = e.frame().bandCount == 3;
	const bool ok      = settledBefore && same && genNow == gen && lostTempo == 0 && pitchDiffers == 0 && frames > 0 && applied;
	rep.add( "params.keep", cell, ok,
			 "after configure() with every runtime field changed: newest window %s, generation %u -> %u; over the next %d frames tempo "
			 "unsettled on %d (was %s) and pitch different on %d from a run configured that way from the start (both must be 0); new band plan %s",
			 same ? "unchanged" : "CHANGED", gen, genNow, frames, lostTempo, settledBefore ? "settled" : "UNSETTLED", pitchDiffers,
			 applied ? "applied" : "NOT APPLIED" );
}

//---------------------------------------------------------------------------
// Fallback: Resolume's 64 FFT bins when no device is chosen.
//
// fallback.empty -- with only host bins: source Host, not calibrated, band
//   envelopes present, and pitch, partials, thirds, tempo and stereo EMPTY.
//   Negative control: pitch invented from the loudest bin.
// fallback.prime.quiet -- a steady spectrum from the very first frame for
//   1 s: no onset. Negative control: no priming (the first frame is judged
//   against silence).
// fallback.prime.hears -- the same, and at 0.1 s the two lowest bins jump
//   3.2x (a kick): band 1 fires on that frame. Negative control: pattern
//   v0.1.0's seed (the floor starts at the whole level, so the threshold is
//   2.7x the level and the kick's flux 2.2x).
// fallback.dt0 -- a jittered spectrum (+-10 % per bin per frame) for 4 s,
//   once with one update a frame and once with every frame delivered twice
//   at the same host time: the onset counts must match exactly. (Linear flux
//   against 3x its mean does fire now and then on jitter; that is pattern's
//   detector, and the claim here is only that a repeated time changes
//   nothing.) Negative control: dt = 0 snaps the floor (the fleet's bug).
// fallback.gap -- the host clock jumps 2 s (a pause) and the spectrum has
//   changed: no onset (re-primed), while the same change 1/60 s later fires.
// fallback.law -- HostBins as documented: bin j's centre is (j + 0.5)/64 of
//   Nyquist (linear) or 20 Hz * (Nyquist / 20 Hz)^((j + 0.5)/64) (log), and a
//   single bin's energy lands in the band holding that centre; Power values
//   are square-rooted. Expected bands computed here from those formulas.
//   Negative control: Power bins used as magnitudes.
// fallback.down -- a named device that is Missing: host bins stand in
//   (fallbackWhenDeviceDown); with it off, nothing. A Silent device: never the
//   host (the operator must see the device is dead). Negative control: the
//   host stands in for a Silent device too.
//---------------------------------------------------------------------------

void checkFallback( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	const double      dt   = 1.0 / run.fps;

	{
		Engine       e( Config(), nullptr );
		e.mutations() = mut;
		HostBinsGen  g;
		for( int i = 0; i < 60; ++i )
			e.update( 10.0 + i * dt, g.next(), 64 );
		const Frame& f  = e.frame();
		const bool   ok = f.source == Source::Host && !f.calibrated && f.bandCount == 4 && !f.pitch.valid && !f.pitch.voiced &&
						f.partialCount == 0 && f.thirdCount == 0 && !f.tempo.valid && !f.stereo.valid && f.capture == CaptureState::Closed;
		rep.add( "fallback.empty", cell, ok, "host bins only: source %s, calibrated %d, %d bands; pitch valid %d, %d partials, %d thirds, tempo %d, stereo %d",
				 f.source == Source::Host ? "Host" : "OTHER", f.calibrated, f.bandCount, f.pitch.valid, f.partialCount, f.thirdCount, f.tempo.valid,
				 f.stereo.valid );
	}
	{
		Engine      e( Config(), nullptr );
		e.mutations() = mut;
		HostBinsGen g;
		const float* steady = g.next( 0.0 );
		uint32_t    quietOnsets = 0;
		for( int i = 0; i < (int)run.fps; ++i )
			quietOnsets = onsetTotal( e.update( 5.0 + i * dt, steady, 64 ) );
		rep.add( "fallback.prime.quiet", cell, quietOnsets == 0, "a steady spectrum from frame one for 1 s: %u onsets (must be 0)", quietOnsets );

		Engine      e2( Config(), nullptr );
		e2.mutations() = mut;
		int         kickFrame = (int)std::lround( 0.1 * run.fps ), firedAt = -1;
		for( int i = 0; i < kickFrame + 10; ++i )
		{
			float k[ 64 ];
			std::copy( steady, steady + 64, k );
			if( i == kickFrame )
				k[ 0 ] *= 3.2f, k[ 1 ] *= 3.2f;
			const Frame& f = e2.update( 5.0 + i * dt, k, 64 );
			if( firedAt < 0 && f.bandCount > 1 && f.bands[ 1 ].onset )
				firedAt = i;
		}
		rep.add( "fallback.prime.hears", cell, firedAt == kickFrame, "a kick on frame %d (0.1 s): band 1 fired on frame %d", kickFrame, firedAt );
	}
	{
		// The same jittered spectrum, once with one update per frame and once
		// with every frame delivered twice at the same host time (two outputs):
		// a repeated time must change nothing, so the onset counts match.
		auto count = [ & ]( bool twice ) {
			Engine      e( Config(), nullptr );
			e.mutations() = mut;
			HostBinsGen g;
			uint32_t    n = 0;
			for( int i = 0; i < (int)( 4.0 * run.fps ); ++i )
			{
				const float* b = g.next();
				n              = onsetTotal( e.update( 5.0 + i * dt, b, 64 ) );
				if( twice )
					n = onsetTotal( e.update( 5.0 + i * dt, b, 64 ) );
			}
			return n;
		};
		const uint32_t once = count( false ), twice = count( true );
		rep.add( "fallback.dt0", cell, once == twice, "4 s of a jittered spectrum: %u onsets with one update a frame, %u with each frame delivered twice (must match)",
				 once, twice );
	}
	{
		auto change = [ & ]( double jump ) {
			Engine      e( Config(), nullptr );
			e.mutations() = mut;
			HostBinsGen g;
			double      t = 5.0;
			for( int i = 0; i < (int)run.fps; ++i, t += dt )
				e.update( t, g.next( 0.0 ), 64 );
			float k[ 64 ];
			std::copy( g.bins, g.bins + 64, k );
			k[ 0 ] *= 3.5f;
			k[ 1 ] *= 3.5f;
			return onsetTotal( e.update( t - dt + jump, k, 64 ) );
		};
		const uint32_t afterGap = change( 2.0 ), normal = change( dt );
		rep.add( "fallback.gap", cell, afterGap == 0 && normal > 0, "the same spectrum change after a 2 s host-clock jump: %u onsets (must be 0); after one frame: %u (must be > 0)",
				 afterGap, normal );
	}
	{
		int  wrong = 0, checked = 0;
		bool powerOk = true;
		for( int layout = 0; layout < 2; ++layout )
		{
			Config c;
			c.hostBins.layout = layout ? earshot::HostBins::Layout::Log : earshot::HostBins::Layout::Linear;
			const double nyq  = 0.5 * c.hostBins.sampleRate;
			for( int j = 0; j < 64; ++j )
			{
				const double u      = ( j + 0.5 ) / 64.0;
				const double centre = layout ? 20.0 * std::pow( nyq / 20.0, u ) : u * nyq;
				int          want   = -1;
				for( int b = 0; b < c.bandCount; ++b )
					if( centre >= c.bandEdges[ b ] && centre < c.bandEdges[ b + 1 ] )
						want = b;
				Engine e( c, nullptr );
				e.mutations() = mut;
				float bins[ 64 ] = {};
				bins[ j ]        = 0.5f;
				const Frame& f   = e.update( 1.0, bins, 64 );
				int          got = -1;
				for( int b = 0; b < f.bandCount; ++b )
					if( f.bands[ b ].level > 0.0f )
						got = got == -1 ? b : -2;
				++checked;
				wrong += got != want ? 1 : 0;
			}
		}
		{
			Config c;
			c.hostBins.value = earshot::HostBins::Value::Power;
			Engine e( c, nullptr );
			e.mutations()     = mut;
			float  bins[ 64 ] = {};
			bins[ 0 ]         = 0.25f;   // power 0.25 is magnitude 0.5
			const Frame& f    = e.update( 1.0, bins, 64 );
			powerOk           = std::fabs( f.bands[ 1 ].level - 0.5f ) < 1e-6f;
		}
		rep.add( "fallback.law", cell, wrong == 0 && powerOk, "%d single-bin spectra (linear and log): %d in the wrong band; Power law square-rooted: %s",
				 checked, wrong, powerOk ? "yes" : "NO" );
	}
	{
		// Missing, through a scripted backend that never finds the device.
		auto missing = [ & ]( bool fallback ) {
			auto   script = std::make_shared< Script >();
			script->results = { et::OpenResult::NotConnected };
			Config c;
			c.fallbackWhenDeviceDown = fallback;
			Engine e( c, std::unique_ptr< et::CaptureBackend >( new ScriptedBackend( script ) ) );
			e.mutations() = mut;
			e.selectDevice( "Dante 1-2" );
			HostBinsGen g;
			const double t0 = wallSeconds();
			while( e.captureState() != CaptureState::Missing && wallSeconds() - t0 < 1.0 )
				sleepMs( 2 );
			return e.update( 1.0, g.next(), 64 ).source;
		};
		const Source withFb = missing( true ), without = missing( false );
		Driver d( run, Config(), mut );
		d.play( silence( run.fs, 3.5, 2 ), []( const Frame&, double ) {} );
		HostBinsGen  g;
		const Frame& f  = d.engine().update( d.fedSeconds() + 0.01, g.next(), 64 );
		const bool   ok = withFb == Source::Host && without == Source::None && f.capture == CaptureState::Silent && f.source == Source::Device;
		rep.add( "fallback.down", cell, ok, "Missing: source %s with fallbackWhenDeviceDown, %s without; Silent: source %s (must be Device)",
				 withFb == Source::Host ? "Host" : "NOT HOST", without == Source::None ? "None" : "NOT NONE",
				 f.source == Source::Device ? "Device" : ( f.source == Source::Host ? "HOST" : "None" ) );
	}
}

//---------------------------------------------------------------------------
// Capture: the device worker, against a scripted backend, in real time.
//
// capture.slowopen -- the device takes 300 ms to open: selectDevice()
//   returns, and no update() in the first second takes, over 100 ms; the
//   state goes Opening -> Running, the host bins stand in while it opens,
//   and audio flows once Running. Why 100 ms: a caller that waited for the
//   open would take the whole 300 ms, and a third of it separates the two
//   on a busy machine. The unloaded figures are 0.01 ms and 0.7 ms; one
//   update took 31 ms on this Mac with other builds and a release running
//   (wall time measures the scheduler too). Negative control: selectDevice()
//   waits for the open.
// capture.missing -- "Dante 1-2" is not connected, then is: Missing (with
//   the reason in Status::error), retried, Running after one retry period
//   (2 s, +-0.5 s). The default device is never asked for. Negative control:
//   a missing device swapped for the default.
// capture.lost -- a running device is unplugged: Missing within 0.25 s,
//   back to Running after the retry, as a new stream (generation moves).
//   Negative control: the stop is not noticed.
// capture.rate -- a device at 44.1 kHz replaces one at 48: sampleRate
//   follows, the generation moves, the third-octave bank drops to 30 bands.
//   Negative control: the first rate is kept.
// capture.destructor -- an Engine destroyed with a device running closes it
//   before its backend goes, and returns within 1 s. Negative control: the
//   worker exits without closing.
//---------------------------------------------------------------------------

void checkCapture( Report& rep, const Run&, const Mutations& mut )
{
	const std::string cell = "real time";
	auto              make = [ & ]( std::shared_ptr< Script > s, const Config& c = Config() ) {
		std::unique_ptr< Engine > e( new Engine( c, std::unique_ptr< et::CaptureBackend >( new ScriptedBackend( std::move( s ) ) ) ) );
		e->mutations() = mut;
		return e;
	};
	HostBinsGen bins;

	{
		auto script       = std::make_shared< Script >();
		script->openDelay = 0.3;
		auto e            = make( script );
		const double t0   = wallSeconds();
		e->selectDevice( "Scripted A" );
		const double selectMs = ( wallSeconds() - t0 ) * 1000.0;
		double       worstMs = 0.0, runningAt = -1.0, audioAt1s = 0.0;
		bool         sawOpening = false, hostWhileOpening = false;
		for( int i = 0; i < 60; ++i )
		{
			const double a = wallSeconds();
			const Frame& f = e->update( a, bins.next(), 64 );
			worstMs        = std::max( worstMs, ( wallSeconds() - a ) * 1000.0 );
			if( f.capture == CaptureState::Opening )
			{
				sawOpening = true;
				hostWhileOpening = hostWhileOpening || f.source == Source::Host;
			}
			if( f.capture == CaptureState::Running && runningAt < 0.0 )
				runningAt = a - t0;
			audioAt1s = f.audioTime;
			sleepMs( 1000.0 / 60.0 );
		}
		const bool ok = selectMs < 100.0 && worstMs < 100.0 && sawOpening && hostWhileOpening && runningAt >= 0.29 && runningAt < 0.6 && audioAt1s > 0.3;
		rep.add( "capture.slowopen", cell, ok,
				 "300 ms open: selectDevice %.2f ms, worst update %.2f ms (limits 100), Opening seen %s with host bins %s, Running at %.3f s, %.2f s of audio by 1 s",
				 selectMs, worstMs, sawOpening ? "yes" : "NO", hostWhileOpening ? "yes" : "NO", runningAt, audioAt1s );
		rep.figure( "capture: worst update() during a 300 ms open (ms)", worstMs );
	}
	{
		auto script     = std::make_shared< Script >();
		script->results = { et::OpenResult::NotConnected, et::OpenResult::Opened };
		auto         e  = make( script );
		const double t0 = wallSeconds();
		e->selectDevice( "Dante 1-2" );
		double missingAt = -1.0, runningAt = -1.0;
		char   reason[ 160 ] = {};
		while( wallSeconds() - t0 < 3.5 && runningAt < 0.0 )
		{
			const Frame& f = e->update( wallSeconds(), nullptr, 0 );
			if( f.capture == CaptureState::Missing && missingAt < 0.0 )
			{
				missingAt = wallSeconds() - t0;
				Status st;
				e->status( st );
				std::snprintf( reason, sizeof( reason ), "%s", st.error );
			}
			if( f.capture == CaptureState::Running )
				runningAt = wallSeconds() - t0;
			sleepMs( 5 );
		}
		const bool ok = missingAt >= 0.0 && missingAt < 0.2 && reason[ 0 ] != 0 && runningAt > 1.5 && runningAt < 2.5 && !script->askedDefault;
		rep.add( "capture.missing", cell, ok, "Missing at %.3f s (\"%s\"), Running at %.3f s (want 1.5..2.5), default device asked for: %s", missingAt, reason,
				 runningAt, script->askedDefault ? "YES" : "never" );
	}
	{
		auto         script = std::make_shared< Script >();
		auto         e      = make( script );
		e->selectDevice( "Scripted A" );
		double t0 = wallSeconds();
		while( e->captureState() != CaptureState::Running && wallSeconds() - t0 < 1.0 )
			sleepMs( 2 );
		for( int i = 0; i < 10; ++i, sleepMs( 16 ) )
			e->update( wallSeconds(), nullptr, 0 );
		const uint32_t gen0 = e->frame().generation;
		script->loseNow     = true;
		t0                  = wallSeconds();
		double missingAt = -1.0, backAt = -1.0;
		while( wallSeconds() - t0 < 3.5 && backAt < 0.0 )
		{
			const Frame& f = e->update( wallSeconds(), nullptr, 0 );
			if( f.capture == CaptureState::Missing && missingAt < 0.0 )
				missingAt = wallSeconds() - t0;
			if( missingAt >= 0.0 && f.capture == CaptureState::Running )
				backAt = wallSeconds() - t0;
			sleepMs( 5 );
		}
		for( int i = 0; i < 5; ++i, sleepMs( 16 ) )
			e->update( wallSeconds(), nullptr, 0 );
		const uint32_t gen1 = e->frame().generation;
		const bool     ok   = missingAt >= 0.0 && missingAt < 0.25 && backAt > 1.5 && backAt < 2.7 && gen1 != gen0;
		rep.add( "capture.lost", cell, ok, "unplugged: Missing after %.3f s (limit 0.25), Running again at %.3f s, generation %u -> %u", missingAt, backAt,
				 gen0, gen1 );
	}
	{
		auto script = std::make_shared< Script >();
		auto e      = make( script );
		auto waitRunning = [ & ]() {
			const double t0 = wallSeconds();
			while( wallSeconds() - t0 < 1.0 )
			{
				const Frame& f = e->update( wallSeconds(), nullptr, 0 );
				if( f.capture == CaptureState::Running && f.audioTime > 0.05 )
					return;
				sleepMs( 10 );
			}
		};
		e->selectDevice( "Scripted A" );
		waitRunning();
		const int      rate0 = e->frame().sampleRate, thirds0 = e->frame().thirdCount;
		const uint32_t gen0  = e->frame().generation;
		script->rate         = 44100;
		e->selectDevice( "Scripted B" );
		sleepMs( 50 );
		waitRunning();
		const int      rate1 = e->frame().sampleRate, thirds1 = e->frame().thirdCount;
		const uint32_t gen1  = e->frame().generation;
		const bool     ok    = rate0 == 48000 && rate1 == 44100 && gen1 != gen0 && thirds0 == 31 && thirds1 == 30;
		rep.add( "capture.rate", cell, ok, "48 kHz device then a 44.1 kHz one: sampleRate %d -> %d, generation %u -> %u, thirds %d -> %d", rate0, rate1, gen0,
				 gen1, thirds0, thirds1 );
	}
	{
		auto script = std::make_shared< Script >();
		{
			auto e = make( script );
			e->selectDevice( "Scripted A" );
			const double t0 = wallSeconds();
			while( e->captureState() != CaptureState::Running && wallSeconds() - t0 < 1.0 )
				sleepMs( 2 );
			const double a = wallSeconds();
			e.reset();
			const double ms = ( wallSeconds() - a ) * 1000.0;
			const bool   ok = ms < 1000.0 && script->closes.load() == script->opens.load() && !script->destroyedOpen;
			rep.add( "capture.destructor", cell, ok,
					 "Engine destroyed with a device running: returned in %.1f ms, %d opens, %d closes, closed before the backend went: %s", ms,
					 script->opens.load(), script->closes.load(), script->destroyedOpen ? "NO" : "yes" );
		}
		rep.figure( "capture: allocations in the device callback", (double)script->deliverAllocations.load() );
	}
}

//---------------------------------------------------------------------------
// Alloc. After construction nothing on the render or audio thread
// allocates: a groove with a held tone, 6 s at every cell, every feed() and
// update() (and readTap) counted on its own thread by estest's operator new.
// The control proves the counter counts: one deliberate new inside a counted
// region must read 1. (The device callback is counted in capture.*.)
// Negative control: update() allocates and frees a block every frame.
//---------------------------------------------------------------------------

void checkAlloc( Report& rep, const Run& run, const Mutations& mut )
{
	const std::string cell = run.name();
	Signal            s    = render( grooves()[ 2 ], run.fs, 6.0, 4 );
	addSine( s, 330.0, 0.2 );
	Driver d( run, Config(), mut );
	d.countAllocations = true;
	d.collectTap       = true;
	d.play( widen( s, 2 ), []( const Frame&, double ) {} );

	// A direct ::operator new call: a new-expression whose result is unused
	// may be elided by the compiler (C++14), and was.
	static void* volatile sink = nullptr;
	startCountingAllocations();
	sink = ::operator new( 24 );
	const uint64_t control = stopCountingAllocations();
	::operator delete( sink );

	const bool ok = d.allocations == 0 && control == 1 && d.updates > 300;
	rep.add( "alloc.render", cell, ok, "%d updates and every feed(): %llu allocations (must be 0); control: one new counted as %llu",
			 d.updates, (unsigned long long)d.allocations, (unsigned long long)control );
	rep.figure( "update() time, worst (ms)", d.maxUpdateMs );
	rep.figure( "update() time, mean (ms)", d.totalUpdateMs / std::max( 1, d.updates ) );
}

} // namespace estest
