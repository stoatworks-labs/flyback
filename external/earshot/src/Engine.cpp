#include "earshot/earshot.h"
#include "earshot/testing.h"

#include "Capture.h"
#include "Dsp.h"
#include "Fft.h"
#include "HostFft.h"
#include "Onsets.h"
#include "Partials.h"
#include "Pitch.h"
#include "Stereo.h"
#include "Tap.h"
#include "Tempo.h"
#include "ThirdOctave.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

namespace earshot
{
namespace
{
constexpr float  kPitchFloorHz  = 40.0f;   ///< buffers are sized for three periods of this
constexpr int    kChunk         = 1024;    ///< frames pulled from the ring at a time
constexpr double kMaxCatchUp    = 0.5;     ///< seconds analysed in one update at most

Config sanitise( Config c, const Config& fixed )
{
	c.maxChannels = fixed.maxChannels;
	c.ringSeconds = fixed.ringSeconds;
	c.pairFirst   = std::max( 0, std::min( c.pairFirst, kMaxChannels - 1 ) );
	c.pitchMinHz  = std::max( kPitchFloorHz, c.pitchMinHz );
	c.pitchMaxHz  = std::max( c.pitchMinHz * 2.0f, std::min( c.pitchMaxHz, 8000.0f ) );
	c.bandCount   = std::max( 1, std::min( c.bandCount, kMaxBands ) );
	for( int b = 1; b <= c.bandCount; ++b )
		c.bandEdges[ b ] = std::max( c.bandEdges[ b ], c.bandEdges[ b - 1 ] + 1.0f );
	c.onsetDeviations = std::max( 0.5f, c.onsetDeviations );
	c.onsetRefractory = std::max( 0.0f, c.onsetRefractory );
	c.tapRate         = std::max( WaveTap::kMinRate, std::min( c.tapRate, WaveTap::kMaxRate ) );
	return c;
}
} // namespace

const char* toString( CaptureState state ) noexcept
{
	switch( state )
	{
	case CaptureState::Closed:  return "Closed";
	case CaptureState::Opening: return "Opening";
	case CaptureState::Running: return "Running";
	case CaptureState::Silent:  return "Silent";
	case CaptureState::Missing: return "Missing";
	case CaptureState::Failed:  return "Failed";
	}
	return "?";
}

float deviceKey( const char* name ) noexcept
{
	uint32_t h = 2166136261u;
	for( const char* p = name != nullptr ? name : ""; *p; ++p )
	{
		h ^= (uint8_t)*p;
		h *= 16777619u;
	}
	// Fold to 23 bits so every key is an integer a float holds exactly, and
	// keep 0 free for "no device".
	const uint32_t key = ( ( h >> 23 ) ^ h ) & 0x7FFFFFu;
	return (float)( key == 0 ? 1u : key );
}

std::vector< DeviceInfo > listDevices() noexcept
{
	std::unique_ptr< testing::CaptureBackend > backend = testing::makeMiniaudioBackend();
	if( !backend )
		return {};
	return backend->enumerate();
}

//---------------------------------------------------------------------------

struct Engine::Impl
{
	Config             config;
	testing::Mutations mut;
	bool               ok = false;

	// Configuration handed over from any thread, applied on the render thread.
	std::mutex pendingLock;
	Config     pending;
	std::atomic< bool > havePending{ false };

	CaptureCore     capture;
	Fft             fft;   // pitch and partials
	PitchDetector   pitch;
	PartialTracker  partials;
	BandAnalyser    bands;
	TempoTracker    tempo;
	ThirdOctaveBank thirds;
	WaveTap         tap;
	HostAnalyser    host;
	Frame           frame;

	// The stream being followed.
	uint32_t epoch     = 0;
	bool     following = false;
	int      fs        = 0;
	int      channels  = 0;
	uint64_t readPos   = 0;

	// Scratch, sized once.
	std::vector< float > chunk, left, right, mono;
	std::vector< float > win, winL, winR, winM;
	mutable std::vector< float > peek;
	int maxWindow = 0;
	void* volatile allocSink = nullptr;   // the allocPerFrame negative control only

	bool init( const Config& c, std::unique_ptr< testing::CaptureBackend > backend ) noexcept
	{
		config  = sanitise( c, c );
		config.maxChannels = std::max( 1, std::min( c.maxChannels, kMaxChannels ) );
		config.ringSeconds = std::max( 0.25f, std::min( c.ringSeconds, 8.0f ) );
		pending = config;

		const size_t ringFrames = (size_t)std::ceil( config.ringSeconds * kMaxSampleRate );
		if( !capture.init( config.maxChannels, ringFrames, std::move( backend ) ) )
			return false;
		capture.mut = &mut;
		tempo.mut   = &mut;

		if( !pitch.init( kMaxSampleRate, kPitchFloorHz ) || !partials.init( kMaxSampleRate ) || !bands.init( kMaxSampleRate ) ||
			!tap.init() )
			return false;
		const int pitchWindow = pitch.windowFor( kMaxSampleRate, config );
		const int partialWindow = PartialTracker::windowFor( kMaxSampleRate );
		const int stereoWindow  = StereoMeter::windowFor( kMaxSampleRate );
		maxWindow = std::max( pitchWindow + PitchDetector::kWarm, std::max( partialWindow, stereoWindow ) );
		if( !fft.init( std::max( dsp::nextPow2( 2 * pitchWindow ), partialWindow ) ) )
			return false;
		try
		{
			chunk.assign( (size_t)kChunk * kMaxChannels, 0.0f );
			left.assign( kChunk, 0.0f );
			right.assign( kChunk, 0.0f );
			mono.assign( kChunk, 0.0f );
			win.assign( (size_t)maxWindow * kMaxChannels, 0.0f );
			winL.assign( (size_t)maxWindow, 0.0f );
			winR.assign( (size_t)maxWindow, 0.0f );
			winM.assign( (size_t)maxWindow, 0.0f );
			peek.assign( (size_t)maxWindow * kMaxChannels, 0.0f );
		}
		catch( ... )
		{
			return false;
		}
		bands.configure( config );
		host.configure( config );
		return true;
	}

	void applyPending() noexcept
	{
		if( !havePending.load( std::memory_order_acquire ) )
			return;
		std::unique_lock< std::mutex > guard( pendingLock, std::try_to_lock );
		if( !guard.owns_lock() )
			return;   // the writer is mid-copy; take it next frame
		const Config next = pending;
		havePending.store( false, std::memory_order_relaxed );
		guard.unlock();

		const int oldTap = config.tapRate;
		config = next;
		bands.configure( config );
		host.configure( config );
		if( following && config.tapRate != oldTap )
			tap.setRates( fs, config.tapRate, mut );

		if( mut.configureClears && following )
		{
			// The negative control: what spasis forbids. A knob move throws the
			// audio history away and every detector starts again.
			capture.ring().reset( capture.ring().channels() );
			following = false;
		}
	}

	void startStream( uint32_t e ) noexcept
	{
		epoch     = e;
		following = true;
		channels  = capture.ring().channels();
		readPos   = 0;
		const int rate = capture.sampleRate();
		if( rate != fs && !( mut.captureRateFixed && fs > 0 ) )
		{
			fs = rate;
			bands.setRate( fs );
			thirds.setRate( fs, mut );
		}
		bands.configure( config );
		bands.reset();
		thirds.setRate( fs, mut );   // the mutations decide the design, so always redesign
		tap.setRates( fs, config.tapRate, mut );
		tempo.reset();
		frame.generation++;
		frame.gapFrames = 0;
	}

	/// Pull everything new from the ring through the sequential analyses.
	void consume() noexcept
	{
		Ring&          ring = capture.ring();
		const uint64_t w    = ring.written();
		const uint64_t cap  = (uint64_t)std::min< double >( kMaxCatchUp * fs, (double)ring.capacity() / 2 );
		if( w > readPos + cap )
		{
			// The render thread fell behind (a stall, or the first frame after a
			// long open). Skip to the newest half second and prime again:
			// flux across a gap is not an onset.
			frame.gapFrames += w - cap - readPos;
			readPos = w - cap;
			bands.reset();
			tap.reset();
		}
		const int ch = channels;
		const int l  = std::min( config.pairFirst, ch - 1 );
		const int r  = std::min( config.pairFirst + 1, ch - 1 );
		while( readPos < w )
		{
			const int n = (int)std::min< uint64_t >( kChunk, w - readPos );
			if( !ring.read( readPos, (size_t)n, chunk.data(), epoch, !mut.ringNoValidate ) )
			{
				// Overwritten under us: the newest data wins, as always.
				const uint64_t now = ring.written();
				frame.gapFrames += now > readPos ? now - readPos : 0;
				readPos = now;
				bands.reset();
				tap.reset();
				break;
			}
			for( int i = 0; i < n; ++i )
			{
				left[ (size_t)i ]  = chunk[ (size_t)i * ch + l ];
				right[ (size_t)i ] = chunk[ (size_t)i * ch + r ];
				mono[ (size_t)i ]  = 0.5f * ( left[ (size_t)i ] + right[ (size_t)i ] );
			}
			bands.push( mono.data(), n, readPos, config, mut, &tempo );
			thirds.push( mono.data(), n );
			tap.push( left.data(), right.data(), n, readPos );
			readPos += (uint64_t)n;
		}
	}

	/// The newest-window analyses: pitch, partials, stereo. Each runs as soon
	/// as its own window has arrived: pitch needs 75 ms, partials 170 ms, and
	/// the pitch must not wait for the partials.
	void windows() noexcept
	{
		const int pitchN   = pitch.windowFor( fs, config );
		const int partialN = PartialTracker::windowFor( fs );
		const int stereoN  = StereoMeter::windowFor( fs );
		const uint64_t delay = (uint64_t)std::max( 0.0, mut.pitchLagDelay * fs );
		const uint64_t end   = readPos > delay ? readPos - delay : 0;
		const int      want  = std::min( maxWindow, std::max( pitchN + PitchDetector::kWarm, std::max( partialN, stereoN ) ) );
		const int      have  = (int)std::min< uint64_t >( end, (uint64_t)want );

		frame.pitch = Pitch();
		frame.partialCount = 0;
		frame.stereo = Stereo();
		const int shortest = std::min( pitchN, std::min( partialN, stereoN ) );
		if( have < shortest || !capture.ring().read( end - (uint64_t)have, (size_t)have, win.data(), epoch ) )
			return;
		const int ch = channels;
		const int l  = std::min( config.pairFirst, ch - 1 );
		const int r  = std::min( config.pairFirst + 1, ch - 1 );
		for( int i = 0; i < have; ++i )
		{
			winL[ (size_t)i ] = win[ (size_t)i * ch + l ];
			winR[ (size_t)i ] = win[ (size_t)i * ch + r ];
			winM[ (size_t)i ] = 0.5f * ( winL[ (size_t)i ] + winR[ (size_t)i ] );
		}
		if( have >= pitchN )
		{
			const int warm = std::min( PitchDetector::kWarm, have - pitchN );
			pitch.analyse( winM.data() + ( have - pitchN - warm ), pitchN, warm, fs, config, mut, fft, frame.pitch );
		}
		if( have >= partialN )
			partials.analyse( winM.data() + ( have - partialN ), partialN, fs, frame.pitch, mut, fft, frame.partials,
							  frame.partialCount );
		if( have >= stereoN )
			StereoMeter::analyse( winL.data() + ( have - stereoN ), winR.data() + ( have - stereoN ), stereoN, ch == 1,
								  mut, frame.stereo );
	}

	void clearDeviceResults() noexcept
	{
		frame.pitch        = Pitch();
		frame.partialCount = 0;
		frame.thirdCount   = 0;
		frame.tempo        = Tempo();
		frame.stereo       = Stereo();
	}

	const Frame& update( double hostSeconds, const float* hostFft, int hostFftCount ) noexcept
	{
		if( mut.allocPerFrame )
		{
			// A direct ::operator new: a new-expression may be elided.
			allocSink = ::operator new( 64 );
			::operator delete( allocSink );
		}
		applyPending();
		const CaptureState st = capture.state();
		frame.capture = st;

		const bool up = st == CaptureState::Running || ( st == CaptureState::Silent && !mut.fallbackOnSilent );
		const uint32_t e = capture.ring().epoch();
		if( up && ( e & 1u ) == 0 )
		{
			if( !following || e != epoch )
				startStream( e );
			const uint64_t before = readPos;
			consume();
			windows();
			const double dt = fs > 0 ? (double)( readPos - before ) / fs : 0.0;
			thirds.publish( frame.thirds, frame.thirdCount, dt, config );
			bands.publish( frame.bands, frame.bandCount );
			tempo.publish( frame.tempo );
			frame.source     = Source::Device;
			frame.calibrated = true;
			frame.sampleRate = fs;
			frame.audioTime  = fs > 0 ? (double)readPos / fs : 0.0;
		}
		else
		{
			following = false;
			clearDeviceResults();
			const bool useHost = hostFft != nullptr && hostFftCount > 0 &&
								 ( st == CaptureState::Closed ||
								   ( config.fallbackWhenDeviceDown && ( st != CaptureState::Silent || mut.fallbackOnSilent ) ) );
			if( useHost )
			{
				host.frame( hostSeconds, hostFft, hostFftCount, config, mut );
				host.publish( frame.bands, frame.bandCount );
				frame.source     = Source::Host;
				frame.calibrated = false;
				frame.sampleRate = (int)config.hostBins.sampleRate;
				frame.audioTime  = host.clock();
				if( mut.fallbackInventsPitch )
				{
					frame.pitch.valid  = true;
					frame.pitch.voiced = true;
					frame.pitch.hz     = (float)host.loudestHz();
				}
			}
			else
			{
				frame.source    = Source::None;
				frame.bandCount = 0;
			}
		}
		frame.tapAvailable = tap.available();
		frame.tapDropped   = tap.dropped();
		return frame;
	}
};

//---------------------------------------------------------------------------

Engine::Engine( const Config& config ) noexcept : Engine( config, testing::makeMiniaudioBackend() )
{
}

Engine::Engine( const Config& config, std::unique_ptr< testing::CaptureBackend > backend ) noexcept
{
	impl_.reset( new( std::nothrow ) Impl );
	if( impl_ )
		impl_->ok = impl_->init( config, std::move( backend ) );
}

Engine::~Engine() = default;

bool Engine::ok() const noexcept
{
	return impl_ && impl_->ok;
}

void Engine::selectDevice( const char* name ) noexcept
{
	if( ok() )
		impl_->capture.select( name );
}

CaptureState Engine::captureState() const noexcept
{
	return ok() ? impl_->capture.state() : CaptureState::Failed;
}

void Engine::status( Status& out ) const noexcept
{
	out = Status();
	if( ok() )
		impl_->capture.status( out );
}

void Engine::configure( const Config& config ) noexcept
{
	if( !ok() )
		return;
	std::lock_guard< std::mutex > guard( impl_->pendingLock );
	impl_->pending = sanitise( config, impl_->config );
	impl_->havePending.store( true, std::memory_order_release );
}

const Config& Engine::config() const noexcept
{
	static const Config kEmpty;
	return ok() ? impl_->config : kEmpty;
}

const Frame& Engine::update( double hostSeconds, const float* hostFft, int hostFftCount ) noexcept
{
	static const Frame kEmpty;
	return ok() ? impl_->update( hostSeconds, hostFft, hostFftCount ) : kEmpty;
}

const Frame& Engine::frame() const noexcept
{
	static const Frame kEmpty;
	return ok() ? impl_->frame : kEmpty;
}

int Engine::readTap( float* left, float* right, int maxSamples ) noexcept
{
	if( !ok() )
		return 0;
	if( impl_->mut.tapPeekLatest )
		return impl_->tap.peekNewest( left, right, std::min( maxSamples, impl_->tap.rate() / 30 ) );
	return impl_->tap.read( left, right, maxSamples );
}

int Engine::tapRate() const noexcept
{
	return ok() ? impl_->tap.rate() : 0;
}

bool Engine::peekWaveform( float* left, float* right, int samples ) const noexcept
{
	if( !ok() || !impl_->following || samples <= 0 || samples > impl_->maxWindow )
		return false;
	const Impl& m = *impl_;
	if( m.readPos < (uint64_t)samples ||
		!m.capture.ring().read( m.readPos - (uint64_t)samples, (size_t)samples, m.peek.data(), m.epoch ) )
		return false;
	const int ch = m.channels;
	const int l  = std::min( m.config.pairFirst, ch - 1 );
	const int r  = std::min( m.config.pairFirst + 1, ch - 1 );
	for( int i = 0; i < samples; ++i )
	{
		if( left != nullptr )
			left[ i ] = m.peek[ (size_t)i * ch + l ];
		if( right != nullptr )
			right[ i ] = m.peek[ (size_t)i * ch + r ];
	}
	return true;
}

void Engine::feedBegin( int sampleRate, int channels ) noexcept
{
	if( ok() )
		impl_->capture.feedBegin( std::max( kMinSampleRate, std::min( sampleRate, kMaxSampleRate ) ), channels );
}

void Engine::feed( const float* interleaved, int frames ) noexcept
{
	if( ok() )
		impl_->capture.feed( interleaved, frames );
}

void Engine::feedEnd() noexcept
{
	if( ok() )
		impl_->capture.feedEnd();
}

testing::Mutations& Engine::mutations() noexcept
{
	static testing::Mutations kSpare;   // only when construction failed
	return ok() ? impl_->mut : kSpare;
}

} // namespace earshot
