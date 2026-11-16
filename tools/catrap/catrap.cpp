/**
    libcatrap -- a CoreAudio trap, for proving a code path never reaches it.

        CATRAP_LOG=/tmp/trap.log DYLD_INSERT_LIBRARIES=build/libcatrap.dylib <program>

    Loaded into a process, it logs "loaded" and then one "HIT" line for each
    attempt to reach CoreAudio, and refuses the attempt:

      - dlopen of CoreAudio, AudioToolbox, AudioUnit or CoreAudioKit (how
        miniaudio, and so earshot, gets at CoreAudio: it links nothing and
        dlopens the frameworks when a context is made -- listDevices and
        selectDevice both make one);
      - a direct call to the HAL or the component manager (AudioObject*,
        AudioDevice*, AudioComponent*, AudioUnit*, AudioOutputUnit*), for any
        code that links the frameworks and calls them by name.

    Every refusal is an error return, never a crash, so the program carries on
    and the log shows everything it tried. verify.sh runs the singing mode's
    off path (hvtest --off, --defaults, and oxbow's selftest on both bundles)
    under it and requires "loaded" and no "HIT"; `hvtest --trap-probe` is the
    negative control, and must leave HITs.

    The interposition is dyld's (DYLD_INTERPOSE: a __DATA,__interpose table
    rebinds every other image's imports). It does not survive a SIP-protected
    parent: perl, /usr/bin/env and the like strip DYLD_* -- run it from bash.
*/
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CATRAP_INTERPOSE( replacement, replacee )                                                                                          \
	__attribute__( ( used ) ) static const struct                                                                                            \
	{                                                                                                                                        \
		const void* r;                                                                                                                       \
		const void* e;                                                                                                                       \
	} interpose_##replacee __attribute__( ( section( "__DATA,__interpose" ) ) ) = { reinterpret_cast< const void* >( &replacement ),         \
		                                                                         reinterpret_cast< const void* >( &replacee ) };

/// Present only while the trap is loaded: `hvtest --trap-probe` looks it up
/// and refuses to reach CoreAudio without it.
extern "C" __attribute__( ( visibility( "default" ) ) ) int catrap_loaded = 1;

namespace
{
void logLine( const char* kind, const char* what )
{
	const char* path = std::getenv( "CATRAP_LOG" );
	char line[ 1200 ];
	const int n = std::snprintf( line, sizeof( line ), "%s pid=%d %s\n", kind, static_cast< int >( getpid() ), what ? what : "" );
	if( n <= 0 )
		return;
	const size_t len = static_cast< size_t >( n ) < sizeof( line ) ? static_cast< size_t >( n ) : sizeof( line ) - 1;
	const int fd     = path != nullptr && *path != '\0' ? open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 ) : 2;
	if( fd < 0 )
		return;
	ssize_t ignored = write( fd, line, len );
	( void )ignored;
	if( fd != 2 )
		close( fd );
}

void hit( const char* what )
{
	logLine( "HIT", what );
}

bool audioFramework( const char* path )
{
	if( path == nullptr )
		return false;
	for( const char* name : { "CoreAudio.framework", "AudioToolbox.framework", "AudioUnit.framework", "CoreAudioKit.framework" } )
		if( std::strstr( path, name ) != nullptr )
			return true;
	return false;
}

__attribute__( ( constructor ) ) void announce()
{
	char what[ 1100 ];
	char exe[ 1024 ] = {};
	uint32_t size    = sizeof( exe );
	if( _NSGetExecutablePath( exe, &size ) != 0 )
		exe[ 0 ] = '\0';
	std::snprintf( what, sizeof( what ), "exe=%s", exe );
	logLine( "loaded", what );
}

//---------------------------------------------------------------------------
void* trap_dlopen( const char* path, int mode )
{
	if( audioFramework( path ) )
	{
		char what[ 1100 ];
		std::snprintf( what, sizeof( what ), "dlopen %s", path );
		hit( what );
		return nullptr;
	}
	return dlopen( path, mode );
}

OSStatus trap_AudioObjectGetPropertyDataSize( AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32* )
{
	hit( "AudioObjectGetPropertyDataSize" );
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioObjectGetPropertyData( AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*, void* )
{
	hit( "AudioObjectGetPropertyData" );
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioObjectSetPropertyData( AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void* )
{
	hit( "AudioObjectSetPropertyData" );
	return kAudioHardwareUnspecifiedError;
}

Boolean trap_AudioObjectHasProperty( AudioObjectID, const AudioObjectPropertyAddress* )
{
	hit( "AudioObjectHasProperty" );
	return false;
}

OSStatus trap_AudioObjectAddPropertyListener( AudioObjectID, const AudioObjectPropertyAddress*, AudioObjectPropertyListenerProc, void* )
{
	hit( "AudioObjectAddPropertyListener" );
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioDeviceCreateIOProcID( AudioObjectID, AudioDeviceIOProc, void*, AudioDeviceIOProcID* )
{
	hit( "AudioDeviceCreateIOProcID" );
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioDeviceStart( AudioObjectID, AudioDeviceIOProcID )
{
	hit( "AudioDeviceStart" );
	return kAudioHardwareUnspecifiedError;
}

AudioComponent trap_AudioComponentFindNext( AudioComponent, const AudioComponentDescription* )
{
	hit( "AudioComponentFindNext" );
	return nullptr;
}

OSStatus trap_AudioComponentInstanceNew( AudioComponent, AudioComponentInstance* out )
{
	hit( "AudioComponentInstanceNew" );
	if( out )
		*out = nullptr;
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioUnitInitialize( AudioUnit )
{
	hit( "AudioUnitInitialize" );
	return kAudioHardwareUnspecifiedError;
}

OSStatus trap_AudioOutputUnitStart( AudioUnit )
{
	hit( "AudioOutputUnitStart" );
	return kAudioHardwareUnspecifiedError;
}
} // namespace

CATRAP_INTERPOSE( trap_dlopen, dlopen )
CATRAP_INTERPOSE( trap_AudioObjectGetPropertyDataSize, AudioObjectGetPropertyDataSize )
CATRAP_INTERPOSE( trap_AudioObjectGetPropertyData, AudioObjectGetPropertyData )
CATRAP_INTERPOSE( trap_AudioObjectSetPropertyData, AudioObjectSetPropertyData )
CATRAP_INTERPOSE( trap_AudioObjectHasProperty, AudioObjectHasProperty )
CATRAP_INTERPOSE( trap_AudioObjectAddPropertyListener, AudioObjectAddPropertyListener )
CATRAP_INTERPOSE( trap_AudioDeviceCreateIOProcID, AudioDeviceCreateIOProcID )
CATRAP_INTERPOSE( trap_AudioDeviceStart, AudioDeviceStart )
CATRAP_INTERPOSE( trap_AudioComponentFindNext, AudioComponentFindNext )
CATRAP_INTERPOSE( trap_AudioComponentInstanceNew, AudioComponentInstanceNew )
CATRAP_INTERPOSE( trap_AudioUnitInitialize, AudioUnitInitialize )
CATRAP_INTERPOSE( trap_AudioOutputUnitStart, AudioOutputUnitStart )
