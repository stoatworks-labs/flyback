#include "earshot/testing.h"

#include "MiniaudioConfig.h"
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

/**
	The miniaudio backend: spasis's `Capture::open` and `enumerate`, behind the
	CaptureBackend interface so the device worker can drive it and the harness
	can replace it.

	What is kept from spasis, verbatim in intent:
	- a device is found by NAME; kDefaultDevice is miniaudio's null id;
	- a named device that is absent is NotConnected (the worker shows Missing
	  and retries), never quietly replaced by the default;
	- the native channel count and native rate are requested, float format,
	  shared mode, and the callback slices the first n channels itself.

	What is new: the device's own "stopped" notification (unplugged, or the
	driver gave up) calls AudioSink::lost(), so the worker can show Missing and
	reopen by name when it comes back. miniaudio fires the same notification
	when WE stop the device, so `closing_` gates it.
*/
namespace earshot
{
namespace testing
{
namespace
{
class MiniaudioBackend final : public CaptureBackend
{
public:
	~MiniaudioBackend() override { close(); }

	OpenResult open( const char* name, AudioSink& sink, char* openedName, char* error ) noexcept override
	{
		close();
		closing_.store( false );
		if( ma_context_init( nullptr, 0, nullptr, &context_ ) != MA_SUCCESS )
		{
			std::snprintf( error, 160, "could not start the audio backend" );
			return OpenResult::Failed;
		}
		contextReady_ = true;

		const bool       wantDefault = name == nullptr || name[ 0 ] == 0 || std::strcmp( name, kDefaultDevice ) == 0;
		ma_device_info*  playback    = nullptr;
		ma_uint32        playbackN   = 0;
		ma_device_info*  capture     = nullptr;
		ma_uint32        captureN    = 0;
		ma_device_id     chosen{};
		ma_device_id*    chosenPtr   = nullptr;
		char             chosenName[ 128 ] = {};
		if( ma_context_get_devices( &context_, &playback, &playbackN, &capture, &captureN ) != MA_SUCCESS )
		{
			std::snprintf( error, 160, "could not list audio inputs" );
			close();
			return OpenResult::Failed;
		}
		for( ma_uint32 i = 0; i < captureN; ++i )
		{
			const bool match = wantDefault ? capture[ i ].isDefault != 0 : std::strcmp( name, capture[ i ].name ) == 0;
			if( !match )
				continue;
			chosen    = capture[ i ].id;
			chosenPtr = &chosen;
			std::snprintf( chosenName, sizeof( chosenName ), "%s", capture[ i ].name );
			break;
		}
		if( chosenPtr == nullptr && !wantDefault )
		{
			std::snprintf( error, 160, "\"%s\" is not connected", name );
			close();
			return OpenResult::NotConnected;
		}
		if( wantDefault )
			chosenPtr = nullptr;   // follow the system default, as miniaudio does with a null id

		ma_device_config config     = ma_device_config_init( ma_device_type_capture );
		config.capture.pDeviceID    = chosenPtr;
		config.capture.format       = ma_format_f32;
		config.capture.channels     = 0;   // native: slice, never mix
		config.capture.shareMode    = ma_share_mode_shared;
		config.sampleRate           = 0;   // native
		config.dataCallback         = &MiniaudioBackend::data;
		config.notificationCallback = &MiniaudioBackend::notify;
		config.pUserData            = this;
		if( ma_device_init( &context_, &config, &device_ ) != MA_SUCCESS )
		{
			std::snprintf( error, 160, "could not open \"%s\"", chosenName[ 0 ] ? chosenName : "the default input" );
			close();
			return OpenResult::Failed;
		}
		deviceReady_ = true;
		native_      = (int)device_.capture.channels;
		sink.begin( (int)device_.sampleRate, native_ );
		sink_.store( &sink );

		if( ma_device_start( &device_ ) != MA_SUCCESS )
		{
			std::snprintf( error, 160, "could not start \"%s\"", chosenName[ 0 ] ? chosenName : "the default input" );
			close();
			return OpenResult::Failed;
		}
		std::snprintf( openedName, 128, "%s", chosenName[ 0 ] ? chosenName : "Default input" );
		return OpenResult::Opened;
	}

	void close() noexcept override
	{
		closing_.store( true );
		if( deviceReady_ )
		{
			ma_device_uninit( &device_ );   // blocks until the callback has returned for good
			deviceReady_ = false;
		}
		sink_.store( nullptr );
		if( contextReady_ )
		{
			ma_context_uninit( &context_ );
			contextReady_ = false;
		}
	}

	std::vector< DeviceInfo > enumerate() noexcept override
	{
		std::vector< DeviceInfo > out;
		ma_context context;
		if( ma_context_init( nullptr, 0, nullptr, &context ) != MA_SUCCESS )
			return out;
		ma_device_info* playback  = nullptr;
		ma_uint32       playbackN = 0;
		ma_device_info* capture   = nullptr;
		ma_uint32       captureN  = 0;
		if( ma_context_get_devices( &context, &playback, &playbackN, &capture, &captureN ) == MA_SUCCESS )
		{
			try
			{
				out.reserve( captureN );
				for( ma_uint32 i = 0; i < captureN; ++i )
				{
					DeviceInfo info;
					info.name      = capture[ i ].name;
					info.isDefault = capture[ i ].isDefault != 0;
					// Only some backends fill nativeDataFormats before this extra
					// call, and the channel count is what tells the operator
					// whether the device can carry their feed (spasis).
					ma_device_info detail;
					if( ma_context_get_device_info( &context, ma_device_type_capture, &capture[ i ].id, &detail ) == MA_SUCCESS )
					{
						ma_uint32 best = 0;
						for( ma_uint32 f = 0; f < detail.nativeDataFormatCount; ++f )
							best = std::max( best, detail.nativeDataFormats[ f ].channels );
						info.channels = (int)best;
					}
					out.push_back( std::move( info ) );
				}
			}
			catch( ... )
			{
				out.clear();
			}
		}
		ma_context_uninit( &context );
		return out;
	}

private:
	static void data( ma_device* device, void* /*output*/, const void* input, ma_uint32 frames )
	{
		MiniaudioBackend* self = static_cast< MiniaudioBackend* >( device->pUserData );
		AudioSink*        sink = self != nullptr ? self->sink_.load( std::memory_order_acquire ) : nullptr;
		if( sink != nullptr && input != nullptr )
			sink->deliver( static_cast< const float* >( input ), (uint32_t)frames, self->native_ );
	}

	static void notify( const ma_device_notification* note )
	{
		if( note == nullptr || note->pDevice == nullptr || note->type != ma_device_notification_type_stopped )
			return;
		MiniaudioBackend* self = static_cast< MiniaudioBackend* >( note->pDevice->pUserData );
		if( self == nullptr || self->closing_.load() )
			return;
		if( AudioSink* sink = self->sink_.load( std::memory_order_acquire ) )
			sink->lost();
	}

	ma_context                context_{};
	ma_device                 device_{};
	bool                      contextReady_ = false;
	bool                      deviceReady_  = false;
	int                       native_       = 0;
	std::atomic< AudioSink* > sink_{ nullptr };
	std::atomic< bool >       closing_{ false };
};
} // namespace

std::unique_ptr< CaptureBackend > makeMiniaudioBackend() noexcept
{
	return std::unique_ptr< CaptureBackend >( new( std::nothrow ) MiniaudioBackend() );
}

} // namespace testing
} // namespace earshot
