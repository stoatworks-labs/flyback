#include "Capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace earshot
{
namespace
{
double nowSeconds()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}

void copyName( char* dst, size_t size, const char* src )
{
	std::snprintf( dst, size, "%s", src != nullptr ? src : "" );
}
} // namespace

CaptureCore::~CaptureCore()
{
	{
		std::lock_guard< std::mutex > guard( lock_ );
		quit_ = true;
	}
	wake_.notify_all();
	if( thread_.joinable() )
		thread_.join();
}

bool CaptureCore::init( int maxChannels, size_t ringFrames, std::unique_ptr< testing::CaptureBackend > backend ) noexcept
{
	maxChannels_ = std::max( 1, std::min( maxChannels, kMaxChannels ) );
	backend_     = std::move( backend );
	return ring_.allocate( maxChannels_, ringFrames );
}

//---------------------------------------------------------------------------
// The backend side
//---------------------------------------------------------------------------

void CaptureCore::begin( int sampleRate, int nativeChannels ) noexcept
{
	ring_.reset( std::max( 1, std::min( nativeChannels, maxChannels_ ) ) );
	native_.store( nativeChannels, std::memory_order_relaxed );
	zeroRun_.store( 0, std::memory_order_relaxed );
	silentLatch_.store( false, std::memory_order_relaxed );
	lost_.store( false, std::memory_order_relaxed );
	rate_.store( sampleRate, std::memory_order_release );
}

void CaptureCore::deliver( const float* in, uint32_t frames, int native ) noexcept
{
	if( in == nullptr || frames == 0 || native <= 0 )
		return;

	// Silence is judged on the raw device buffer, before slicing, so a layout
	// that happens to exclude the only live channel is not called a dead
	// device (spasis). Bit-exact zero: a quiet passage is never exactly zero.
	bool         any   = false;
	const size_t total = (size_t)frames * (size_t)native;
	if( mut != nullptr && mut->silentByLevel )
	{
		for( size_t i = 0; i < total && !any; ++i )
			any = std::fabs( in[ i ] ) >= 1e-4f;
	}
	else if( mut != nullptr && mut->silentOnPair )
	{
		const int kept = std::min( ring_.channels(), native );
		for( size_t i = 0; i < total && !any; ++i )
			any = (int)( i % (size_t)native ) < kept && in[ i ] != 0.0f;
	}
	else
	{
		for( size_t i = 0; i < total && !any; ++i )
			any = in[ i ] != 0.0f;
	}
	if( any )
		zeroRun_.store( 0, std::memory_order_relaxed );
	else
		zeroRun_.fetch_add( (int64_t)frames, std::memory_order_relaxed );

	const int want = ring_.channels();
	if( native == want )
	{
		ring_.write( in, frames );
		return;
	}

	// The first `want` channels, through a stack buffer: the audio thread
	// never allocates.
	constexpr uint32_t kChunk = 512;
	float              scratch[ kChunk * kMaxChannels ];
	const int          take = std::min( want, native );
	for( uint32_t done = 0; done < frames; done += kChunk )
	{
		const uint32_t n = std::min( kChunk, frames - done );
		for( uint32_t f = 0; f < n; ++f )
			for( int c = 0; c < take; ++c )
				scratch[ f * (uint32_t)take + (uint32_t)c ] = in[ (size_t)( done + f ) * (size_t)native + (size_t)c ];
		ring_.write( scratch, n );
	}
}

void CaptureCore::lost() noexcept
{
	if( mut != nullptr && mut->captureLostIgnored )
		return;
	lost_.store( true, std::memory_order_release );
	wake_.notify_all();
}

//---------------------------------------------------------------------------
// Control
//---------------------------------------------------------------------------

void CaptureCore::select( const char* name ) noexcept
{
	if( feeding_.exchange( false ) )
		setState( CaptureState::Closed );

	std::unique_lock< std::mutex > guard( lock_ );
	copyName( requested_, sizeof( requested_ ), name );
	changed_ = true;
	if( !started_ && requested_[ 0 ] != 0 )
	{
		if( !backend_ )
		{
			copyName( error_, sizeof( error_ ), "this build has no capture backend" );
			setState( CaptureState::Failed );
			return;
		}
		try
		{
			thread_  = std::thread( &CaptureCore::worker, this );
			started_ = true;
		}
		catch( ... )
		{
			copyName( error_, sizeof( error_ ), "could not start the device thread" );
			setState( CaptureState::Failed );
			return;
		}
	}
	guard.unlock();
	wake_.notify_all();

	if( mut != nullptr && mut->captureOpenInline )
	{
		// The negative control: spasis's open, which the caller waited for.
		for( int i = 0; i < 5000; ++i )
		{
			{
				std::lock_guard< std::mutex > g( lock_ );
				if( !changed_ && std::strcmp( requested_, current_ ) == 0 && state() != CaptureState::Opening )
					break;
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
}

void CaptureCore::feedBegin( int sampleRate, int channels ) noexcept
{
	// Close any real device and wait for the worker to have done it: begin()
	// below resets the ring, which only one producer may touch at a time.
	{
		std::lock_guard< std::mutex > guard( lock_ );
		requested_[ 0 ] = 0;
		changed_        = true;
	}
	wake_.notify_all();
	for( int i = 0; i < 5000; ++i )
	{
		{
			std::lock_guard< std::mutex > guard( lock_ );
			if( !started_ || ( current_[ 0 ] == 0 && !deviceOpen_ && !changed_ ) )
				break;
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}

	feedChannels_ = std::max( 1, std::min( channels, kMaxChannels ) );
	begin( sampleRate, feedChannels_ );
	{
		std::lock_guard< std::mutex > guard( lock_ );
		copyName( opened_, sizeof( opened_ ), "(feed)" );
		error_[ 0 ] = 0;
	}
	feeding_.store( true, std::memory_order_release );
	setState( CaptureState::Running );
}

void CaptureCore::feed( const float* interleaved, int frames ) noexcept
{
	if( feeding_.load( std::memory_order_acquire ) && frames > 0 )
		deliver( interleaved, (uint32_t)frames, feedChannels_ );
}

void CaptureCore::feedEnd() noexcept
{
	if( feeding_.exchange( false ) )
	{
		setState( CaptureState::Closed );
		std::lock_guard< std::mutex > guard( lock_ );
		opened_[ 0 ] = 0;
	}
}

double CaptureCore::silentSeconds() const noexcept
{
	const int rate = rate_.load( std::memory_order_acquire );
	return rate > 0 ? (double)zeroRun_.load( std::memory_order_relaxed ) / rate : 0.0;
}

CaptureState CaptureCore::state() const noexcept
{
	const CaptureState s = (CaptureState)state_.load( std::memory_order_acquire );
	const bool         never = mut != nullptr && mut->silentNever;
	const bool         latch = mut != nullptr && mut->silentLatched;
	if( s == CaptureState::Running && !never && ( silentSeconds() >= kSilenceTimeout || ( latch && silentLatch_.load() ) ) )
	{
		if( latch )
			silentLatch_.store( true );
		return CaptureState::Silent;
	}
	return s;
}

void CaptureCore::status( Status& out ) const noexcept
{
	out.state          = state();
	out.sampleRate     = rate_.load( std::memory_order_acquire );
	out.nativeChannels = native_.load( std::memory_order_relaxed );
	out.channels       = ring_.channels();
	out.silentSeconds  = silentSeconds();
	std::lock_guard< std::mutex > guard( lock_ );
	copyName( out.requested, sizeof( out.requested ), requested_ );
	copyName( out.opened, sizeof( out.opened ), opened_ );
	copyName( out.error, sizeof( out.error ), error_ );
}

//---------------------------------------------------------------------------
// The worker
//---------------------------------------------------------------------------

void CaptureCore::closeDevice() noexcept
{
	if( deviceOpen_ && backend_ )
		backend_->close();
	deviceOpen_ = false;
}

void CaptureCore::worker() noexcept
{
	std::unique_lock< std::mutex > lk( lock_ );
	double                         retryAt = 0.0;

	auto attempt = [ & ]( const char* target ) {
		setState( CaptureState::Opening );
		char opened[ 128 ] = {}, error[ 160 ] = {};
		lk.unlock();
		testing::OpenResult result = backend_->open( target, *this, opened, error );
		if( result == testing::OpenResult::NotConnected && mut != nullptr && mut->captureMissingToDefault )
			result = backend_->open( kDefaultDevice, *this, opened, error );   // the negative control: what spasis forbids
		lk.lock();
		switch( result )
		{
		case testing::OpenResult::Opened:
			deviceOpen_ = true;
			copyName( opened_, sizeof( opened_ ), opened[ 0 ] ? opened : target );
			error_[ 0 ] = 0;
			setState( CaptureState::Running );
			break;
		case testing::OpenResult::NotConnected:
			copyName( error_, sizeof( error_ ), error[ 0 ] ? error : "not connected" );
			setState( CaptureState::Missing );
			retryAt = nowSeconds() + kRetrySeconds;
			break;
		case testing::OpenResult::Failed:
		default:
			copyName( error_, sizeof( error_ ), error[ 0 ] ? error : "the backend refused the device" );
			setState( CaptureState::Failed );
			retryAt = nowSeconds() + kRetrySeconds;
			break;
		}
	};

	while( !quit_ )
	{
		const bool lostNow = lost_.exchange( false );
		const bool differs = std::strcmp( requested_, current_ ) != 0;
		if( differs || lostNow )
		{
			changed_ = false;
			char target[ 128 ];
			copyName( target, sizeof( target ), requested_ );
			lk.unlock();
			closeDevice();
			lk.lock();
			copyName( current_, sizeof( current_ ), target );
			opened_[ 0 ] = 0;
			if( target[ 0 ] == 0 )
			{
				error_[ 0 ] = 0;
				if( !feeding_.load() )
					setState( CaptureState::Closed );
			}
			else if( lostNow && !differs )
			{
				std::snprintf( error_, sizeof( error_ ), "\"%s\" stopped", target );
				setState( CaptureState::Missing );
				retryAt = nowSeconds() + kRetrySeconds;
			}
			else
				attempt( target );
		}
		else
		{
			changed_ = false;
			const CaptureState s = (CaptureState)state_.load();
			if( current_[ 0 ] != 0 && ( s == CaptureState::Missing || s == CaptureState::Failed ) &&
				nowSeconds() >= retryAt )
			{
				char target[ 128 ];
				copyName( target, sizeof( target ), current_ );
				attempt( target );
			}
		}
		wake_.wait_for( lk, std::chrono::milliseconds( 100 ),
						[ & ] { return quit_ || changed_ || lost_.load(); } );
	}
	lk.unlock();
	if( mut == nullptr || !mut->captureNoCloseOnDestroy )
		closeDevice();
}

} // namespace earshot
