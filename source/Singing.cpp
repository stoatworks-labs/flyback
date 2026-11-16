#include "Singing.h"

#include "Diag.h"

#if FLYBACK_SINGING
#include <earshot/earshot.h>
#include <earshot/testing.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace flyback::sing
{
namespace
{
std::atomic< int > g_engines { 0 }, g_lists { 0 }, g_selects { 0 }, g_feeds { 0 }, g_updates { 0 };
int g_defaultHooks = 0;

#if FLYBACK_SINGING
BackendFactory g_factory = nullptr;
#endif

constexpr const char* kOffLabel     = "Off (BPS)";
constexpr const char* kDefaultLabel = "System default";
constexpr const char* kEmptySlot    = "(none)";

/// FLYBACK_NO_DEVICES=1: never list, never select (verify.sh sets it for
/// oxbow; woofer's and rubens's gate, both calls because both start CoreAudio).
bool devicesDisabled()
{
	const char* v = std::getenv( "FLYBACK_NO_DEVICES" );
	return v != nullptr && *v != '\0' && std::strcmp( v, "0" ) != 0;
}

float defaultKey()
{
#if FLYBACK_SINGING
	return earshot::deviceKey( earshot::kDefaultDevice );
#else
	return KeyFor( "(default)" );
#endif
}
} // namespace

//---------------------------------------------------------------------------
float KeyFor( const char* name )
{
	// earshot's deviceKey, written out so a build without earshot names the
	// same elements (hvtest --device holds the two equal).
	uint32_t h = 2166136261u;
	for( const char* c = name != nullptr ? name : ""; *c; ++c )
	{
		h ^= static_cast< unsigned char >( *c );
		h *= 16777619u;
	}
	h = ( ( h >> 23 ) ^ h ) & 0x7FFFFFu;
	return static_cast< float >( h == 0 ? 1u : h );
}

double GateFromParam( float v )
{
	return kGateLowDb + ( kGateHighDb - kGateLowDb ) * std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}

float ParamFromGate( double db )
{
	return static_cast< float >( std::clamp( ( db - kGateLowDb ) / ( kGateHighDb - kGateLowDb ), 0.0, 1.0 ) );
}

int OctaveFromParam( float v )
{
	const int index = std::clamp( static_cast< int >( std::lround( v ) ), 0, kOctaveElements - 1 );
	return kOctaveLow + index;
}

float ParamFromOctave( int octaves )
{
	return static_cast< float >( std::clamp( octaves, kOctaveLow, kOctaveHigh ) - kOctaveLow );
}

double RateFor( double hz, int octaves )
{
	if( !( hz > 0.0 ) || !std::isfinite( hz ) )
		return 0.0;
	double r = std::ldexp( hz, octaves );
	// Exact: a multiply or divide by two changes only the exponent.
	while( r > kRateHigh )
		r *= 0.5;
	while( r < kRateLow )
		r *= 2.0;
	return r;
}

Counters CountersForTest()
{
	Counters c;
	c.engines = g_engines.load();
	c.lists   = g_lists.load();
	c.selects = g_selects.load();
	c.feeds   = g_feeds.load();
	c.updates = g_updates.load();
	return c;
}

//---------------------------------------------------------------------------
#if FLYBACK_SINGING
void PitchInput::SetBackendFactoryForTest( BackendFactory factory )
{
	g_factory = factory;
}
#endif

void PitchInput::SetDefaultHooksForTest( int h )
{
	g_defaultHooks = h;
}

bool PitchInput::Built()
{
	return FLYBACK_SINGING != 0;
}

PitchInput::PitchInput() :
	hooks( g_defaultHooks )
{
	// The menu as it stands until the mode is first switched on: nothing
	// listed, so nothing asked of the OS.
	slots.push_back( { kOffLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, "(default)", defaultKey() } );
	for( int spare = 1; static_cast< int >( slots.size() ) < kDeviceElements; ++spare )
		slots.push_back( { kEmptySlot, "", -static_cast< float >( spare ) } );
	if( hooks & kHookEagerDevices )
		Start();
}

PitchInput::~PitchInput() = default;

bool PitchInput::Started() const
{
#if FLYBACK_SINGING
	return engine != nullptr;
#else
	return false;
#endif
}

void PitchInput::Start()
{
#if FLYBACK_SINGING
	if( engine )
		return;
	earshot::Config c;
	c.maxChannels = 2;
	// No host FFT is ever handed over (it has no pitch), so there is nothing
	// to fall back to.
	c.fallbackWhenDeviceDown = false;
	if( g_factory != nullptr )
		engine = std::make_unique< earshot::Engine >( c, g_factory() );
	else
		engine = std::make_unique< earshot::Engine >( c );
	++g_engines;
	if( !engine->ok() )
		diag::error( "earshot could not allocate: the coil will not sing" );
	Refresh();
#endif
}

void PitchInput::Refresh()
{
#if FLYBACK_SINGING
	slots.clear();
	slots.push_back( { kOffLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, earshot::kDefaultDevice, earshot::deviceKey( earshot::kDefaultDevice ) } );
	std::vector< earshot::DeviceInfo > devices;
	if( g_factory != nullptr )
	{
		// A throwaway backend of the harness's kind: it lists, it opens nothing.
		if( auto lister = g_factory() )
		{
			++g_lists;
			devices = lister->enumerate();
		}
	}
	else if( !devicesDisabled() )
	{
		++g_lists;
		devices = earshot::listDevices();
	}
	int listed = 0;
	for( const earshot::DeviceInfo& d : devices )
	{
		if( static_cast< int >( slots.size() ) >= kDeviceElements )
			break;
		const float key = ( hooks & kHookMenuByIndex ) ? static_cast< float >( slots.size() ) : earshot::deviceKey( d.name.c_str() );
		seen[ key ]     = d.name;
		slots.push_back( { d.name.substr( 0, 40 ), d.name, key } );
		++listed;
	}
	// Spares: distinct values no key can take (keys are 1 .. 2^23 - 1).
	for( int spare = 1; static_cast< int >( slots.size() ) < kDeviceElements; ++spare )
		slots.push_back( { kEmptySlot, "", -static_cast< float >( spare ) } );
	diag::info( "pitch devices: " + std::to_string( listed ) + " listed" );

	// A device remembered by key and now present again: open it by name.
	if( applied > 0.0f )
	{
		const auto known = seen.find( applied );
		if( known != seen.end() && known->second != requested && !devicesDisabled() )
		{
			requested = known->second;
			feeding   = false;
			++g_selects;
			engine->selectDevice( requested.c_str() );
			diag::info( "pitch device after rescan: " + requested );
		}
	}
#endif
}

void PitchInput::Select( float value )
{
#if FLYBACK_SINGING
	feeding = false;// any selectDevice ends a feed()
	std::string name;
	if( value <= 0.0f )
		name.clear();// Off, or a spare
	else if( value == earshot::deviceKey( earshot::kDefaultDevice ) )
		name = earshot::kDefaultDevice;
	else
	{
		const auto known = seen.find( value );
		// A key with no device met in this process: ask for a name that cannot
		// match, so earshot says Missing (never the default). Rescan resolves
		// it once the device is back.
		name = known != seen.end() ? known->second : "(missing device " + std::to_string( static_cast< long >( value ) ) + ")";
	}
	requested = name;
	held      = 0.0;
	if( devicesDisabled() )
	{
		diag::info( "pitch device: " + ( name.empty() ? std::string( kOffLabel ) : name ) + " (FLYBACK_NO_DEVICES: not opened)" );
		return;
	}
	++g_selects;
	engine->selectDevice( name.c_str() );
	diag::info( "pitch device: " + ( name.empty() ? std::string( kOffLabel ) : name ) );
#else
	( void )value;
#endif
}

bool PitchInput::Apply( float value, bool rescan )
{
	if( !std::isfinite( value ) )
		return false;
	bool menuChanged = false;
	if( value != applied )
	{
		const float was = applied;
		applied         = value;
		if( value > 0.0f )
		{
			if( !Started() )
			{
				Start();
				menuChanged = PitchInput::Built();
				diag::info( "singing: on" );
			}
			Select( value );
		}
		else if( was > 0.0f )
		{
			// Off: close the device, keep the engine (and the menu) for next time.
			Select( 0.0f );
			diag::info( "singing: off" );
		}
	}
	if( rescan && Active() && Started() )
	{
		Refresh();
		menuChanged = true;
	}
	return menuChanged;
}

void PitchInput::Feed( const float* interleaved, int frames, int rate, int channels )
{
#if FLYBACK_SINGING
	if( !engine )
		return;
	if( !feeding )
	{
		++g_feeds;
		engine->feedBegin( rate, channels );
		feeding = true;
	}
	engine->feed( interleaved, frames );
#else
	( void )interleaved;
	( void )frames;
	( void )rate;
	( void )channels;
#endif
}

Note PitchInput::Update( double hostSeconds, int octaves, double gateDb )
{
	Note n;
	if( !Active() )
		return n;
	n.active  = true;
	n.blinkOn = std::fmod( std::fabs( hostSeconds ), 0.5 ) < 0.25;
#if FLYBACK_SINGING
	if( !engine )
	{
		n.lamp = Lamp::Waiting;
		return n;
	}
	if( gateDb != appliedGate )
	{
		// One threshold for both: under the Gate nothing is voiced, and the
		// coil rests.
		earshot::Config c = engine->config();
		c.pitchGateDb     = static_cast< float >( gateDb );
		engine->configure( c );
		appliedGate = gateDb;
	}
	++g_updates;
	const earshot::Frame& f = engine->update( hostSeconds, nullptr, 0 );

	if( static_cast< int >( f.capture ) != lastState )
	{
		earshot::Status status;
		engine->status( status );
		std::string line = std::string( "pitch capture " ) + ( lastState < 0 ? "-" : earshot::toString( static_cast< earshot::CaptureState >( lastState ) ) )
		                   + " -> " + earshot::toString( f.capture );
		if( status.requested[ 0 ] )
			line += std::string( " requested=\"" ) + status.requested + "\"";
		if( status.opened[ 0 ] )
			line += std::string( " opened=\"" ) + status.opened + "\"";
		if( status.error[ 0 ] )
			line += std::string( " error=\"" ) + status.error + "\"";
		if( f.capture == earshot::CaptureState::Silent || f.capture == earshot::CaptureState::Missing || f.capture == earshot::CaptureState::Failed )
			diag::warn( line );
		else
			diag::info( line );
		lastState = static_cast< int >( f.capture );
	}
	switch( f.capture )
	{
	case earshot::CaptureState::Running: n.lamp = Lamp::Running; break;
	case earshot::CaptureState::Silent: n.lamp = Lamp::Silent; break;
	case earshot::CaptureState::Missing:
	case earshot::CaptureState::Failed: n.lamp = Lamp::Missing; break;
	case earshot::CaptureState::Opening:
	case earshot::CaptureState::Closed: n.lamp = Lamp::Waiting; break;
	}

	// A new stream starts with no note.
	if( f.generation != generation )
	{
		generation = f.generation;
		held       = 0.0;
	}
	n.generation = generation;
	if( f.source != earshot::Source::Device || !f.pitch.valid )
	{
		held = 0.0;
		return n;
	}
	n.frameHz    = f.pitch.hz;
	n.levelDb    = f.pitch.levelDb;
	n.confidence = f.pitch.confidence;
	n.voiced     = f.pitch.voiced && f.pitch.hz > 0.0f;
	const bool above = ( hooks & kHookGateOpen ) || static_cast< double >( f.pitch.levelDb ) >= gateDb;
	if( n.voiced )
		held = f.pitch.hz;
	else if( !above || ( hooks & kHookVoicedOnly ) )
		held = 0.0;// a rest forgets the note (and the negative control forgets it at once)
	n.hz       = held;
	n.sounding = held > 0.0 && above;
	const int oct = ( hooks & kHookOctaveOff ) ? octaves + 1 : octaves;
	n.rate     = n.sounding ? RateFor( held, oct ) : 0.0;
#else
	( void )hostSeconds;
	( void )octaves;
	( void )gateDb;
	n.lamp = Lamp::Missing;// this build has no capture: say so the only way it can
#endif
	return n;
}
} // namespace flyback::sing
