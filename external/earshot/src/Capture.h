#pragma once

#include "Ring.h"

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

/**
	The capture side: the audio callback's body, the device worker, and the
	state the operator is shown.

	Lifted from spasis's `Capture` with its rules kept and one fixed:

	- **Devices are remembered by name, never by index.** An index is a
	  position in a list that changes whenever anything is plugged in. A named
	  device that is not connected is Missing -- never silently swapped for
	  the default (a composition saved against a Dante feed that reopens on
	  the laptop microphone is worse than one that says the feed is gone) --
	  and the worker retries every two seconds, so plugging it back in brings
	  it back.
	- **The first n channels, never a downmix.** miniaudio is asked for the
	  native channel count and the callback slices.
	- **Silent** is open, running and bit-exactly zero for three seconds: no
	  microphone permission (macOS gives a host a working device that returns
	  zeros, not an error), an unpatched Dante receiver, a loopback with nothing
	  routed. Bit-exact, because a quiet passage is not silence: dither and room
	  noise are never exactly zero.
	- **Opening is off the caller's thread** -- the fix. spasis opened inside
	  ProcessOpenGL and measured CoreAudio blocking Arena's render thread for
	  long enough that parameter writes were dropped; its AGENTS.md lists the
	  worker thread as the next thing to do. Here a worker does every open and
	  close; selectDevice() only posts a name.
*/
namespace earshot
{

class CaptureCore final : public testing::AudioSink
{
public:
	CaptureCore() = default;
	~CaptureCore() override;

	bool init( int maxChannels, size_t ringFrames, std::unique_ptr< testing::CaptureBackend > backend ) noexcept;

	//-- AudioSink (backend side) ---------------------------------------------
	void begin( int sampleRate, int nativeChannels ) noexcept override;
	void deliver( const float* interleaved, uint32_t frames, int nativeChannels ) noexcept override;
	void lost() noexcept override;

	//-- Control ---------------------------------------------------------------
	void select( const char* name ) noexcept;
	void feedBegin( int sampleRate, int channels ) noexcept;
	void feed( const float* interleaved, int frames ) noexcept;
	void feedEnd() noexcept;

	CaptureState state() const noexcept;
	void         status( Status& out ) const noexcept;
	int          sampleRate() const noexcept { return rate_.load( std::memory_order_acquire ); }
	double       silentSeconds() const noexcept;

	Ring&       ring() noexcept { return ring_; }
	const Ring& ring() const noexcept { return ring_; }

	const testing::Mutations* mut = nullptr;

	static constexpr double kSilenceTimeout = 3.0;   ///< spasis's figure
	static constexpr double kRetrySeconds   = 2.0;

private:
	void worker() noexcept;
	void closeDevice() noexcept;   // worker thread
	void setState( CaptureState s ) noexcept { state_.store( (int)s, std::memory_order_release ); }

	Ring ring_;
	int  maxChannels_ = 2;
	std::unique_ptr< testing::CaptureBackend > backend_;

	std::atomic< int >     state_{ (int)CaptureState::Closed };
	std::atomic< int >     rate_{ 0 };
	std::atomic< int >     native_{ 0 };
	std::atomic< int64_t > zeroRun_{ 0 };   ///< consecutive bit-exact-zero frames
	mutable std::atomic< bool > silentLatch_{ false };   ///< the silentLatched negative control only
	std::atomic< bool >    lost_{ false };
	std::atomic< bool >    feeding_{ false };
	int                    feedChannels_ = 0;

	mutable std::mutex      lock_;          // the strings and the request; never held across a backend call
	std::condition_variable wake_;
	std::thread             thread_;
	bool                    started_ = false, quit_ = false, changed_ = false;
	std::atomic< bool >     deviceOpen_{ false };  // set by the worker
	char                    requested_[ 128 ] = {};
	char                    current_[ 128 ]   = {};   // what the worker is serving
	char                    opened_[ 128 ]    = {};
	char                    error_[ 160 ]     = {};
};

} // namespace earshot
