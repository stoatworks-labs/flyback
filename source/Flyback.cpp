#include "Flyback.h"

#include "Diag.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace flyback
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double WallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// Which parameter each preset column drives, in presets::Param order.
constexpr unsigned int kPresetColumns[ presets::kParamCount ] = {
	PT_MACHINE, PT_SUPPLY,   PT_VOLTAGE, PT_IMPEDANCE, PT_BRANCHING, PT_MEMORY, PT_REACH,  PT_ROD_SPREAD, PT_ROD_LENGTH,
	PT_RISE,    PT_WIND,     PT_BPS,     PT_TOPLOAD,   PT_TARGET,    PT_BELT,   PT_SPHERE, PT_GAP,        PT_GLOBE,
	PT_FINGER,  PT_ORIGIN,   PT_EFFICIENCY, PT_GLOW,   PT_GAS,       PT_SHUTTER, PT_PERSISTENCE,
};
} // namespace

// The About block is declared one button per link; the run in the enum and
// the run the block has must agree. They diverge the day a guide is written.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- add or remove a PT_ABOUT_BUTTON_n" );

FlybackPlugin::FlybackPlugin( bool overInput_ ) :
	overInput( overInput_ )
{
	diag::init();
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );
	Declare();
}

void FlybackPlugin::Declare()
{
	//---------------------------------------------------------------------
	// Defaults: the Tesla coil, as preset row 1 says (hvtest --defaults).
	// Relative controls sit at 0.5, each machine's own nominal.
	//---------------------------------------------------------------------
	params[ PT_PRESET ]  = 0.0f;
	params[ PT_MACHINE ] = static_cast< float >( Machine::Tesla );
	params[ PT_SEED ]    = 0.0f;

	params[ PT_SUPPLY ]    = static_cast< float >( physics::SupplyKind::NST );
	params[ PT_VOLTAGE ]   = 0.5f;
	params[ PT_IMPEDANCE ] = 0.5f;

	params[ PT_BRANCHING ] = 0.5f;
	params[ PT_DETAIL ]    = 0.5f;
	params[ PT_MEMORY ]    = 0.5f;
	params[ PT_REACH ]     = 0.5f;

	params[ PT_ROD_SPREAD ] = 0.3750f;
	params[ PT_ROD_LENGTH ] = 0.4286f;
	params[ PT_RISE ]       = 0.5943f;
	params[ PT_WIND ]       = 0.5f;

	params[ PT_BPS ]      = 0.6078f;
	params[ PT_TOPLOAD ]  = 0.3103f;
	params[ PT_TARGET ]   = 1.0f;
	params[ PT_TARGET_X ] = 0.78f;
	params[ PT_TARGET_Y ] = 0.24f;

	params[ PT_BELT ]   = 0.5886f;
	params[ PT_SPHERE ] = 0.4000f;
	params[ PT_GAP ]    = 0.7686f;

	params[ PT_GLOBE ]    = 0.7500f;
	params[ PT_FINGER ]   = 0.0f;
	params[ PT_FINGER_X ] = 0.72f;
	params[ PT_FINGER_Y ] = 0.66f;

	params[ PT_ORIGIN ] = 0.0f;

	params[ PT_AUDIO_FIRES ] = 0.0f;
	params[ PT_AUDIO_DRIVE ] = 0.0f;

	params[ PT_EFFICIENCY ]  = 0.5f;
	params[ PT_GLOW ]        = 0.45f;
	params[ PT_GAS ]         = 0.0f;
	params[ PT_SHUTTER ]     = 1.0f;
	params[ PT_PERSISTENCE ] = 0.30f;
	params[ PT_APPARATUS ]   = 1.0f;
	params[ PT_BACK_R ]      = 0.010f;
	params[ PT_BACK_G ]      = 0.012f;
	params[ PT_BACK_B ]      = 0.022f;

	params[ PT_DETECT ]       = 0.0f;
	params[ PT_THRESHOLD ]    = 0.60f;
	params[ PT_ILLUMINATION ] = 0.35f;
	params[ PT_MIX ]          = 1.0f;

	params[ PT_POS_X ]    = 0.5f;
	params[ PT_POS_Y ]    = 0.5f;
	params[ PT_SCALE ]    = 0.5f;
	params[ PT_ROTATION ] = 0.5f;

	//---------------------------------------------------------------------
	// Declaration, in the order the host shows them. SetParamGroup collapses
	// RUNS of same-group ids, which is why Controls.h keeps them consecutive.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, const char* name, std::initializer_list< const char* > elements ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( elements.size() ), params[ id ] );
		unsigned int i = 0;
		for( const char* e : elements )
		{
			SetParamElementInfo( id, i, e, static_cast< float >( i ) );
			++i;
		}
	};
	auto standard = [ this ]( unsigned int id, const char* name ) { SetParamInfo( id, name, FF_TYPE_STANDARD, params[ id ] ); };
	auto group    = [ this ]( unsigned int from, unsigned int to, const char* name ) {
		for( unsigned int i = from; i <= to; ++i )
			SetParamGroup( i, name );
	};

	{
		SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, 0.0f );
		SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
		for( int i = 0; i < presets::kCount; ++i )
			SetParamElementInfo( PT_PRESET, static_cast< unsigned int >( 1 + i ), presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );
	}
	option( PT_MACHINE, "Machine", { "Jacob's Ladder", "Tesla Coil", "Van de Graaff", "Plasma Globe", "Lichtenberg" } );
	SetParamInfo( PT_FIRE, "Fire", FF_TYPE_EVENT, false );
	standard( PT_SEED, "Seed" );
	group( PT_PRESET, PT_SEED, "Machine" );

	option( PT_SUPPLY, "Supply", { "ZVS", "NST", "Flyback" } );
	standard( PT_VOLTAGE, "Voltage" );
	standard( PT_IMPEDANCE, "Source Impedance" );
	group( PT_SUPPLY, PT_IMPEDANCE, "Supply" );

	standard( PT_BRANCHING, "Branching" );
	standard( PT_DETAIL, "Detail" );
	standard( PT_MEMORY, "Channel Memory" );
	standard( PT_REACH, "Reach" );
	group( PT_BRANCHING, PT_REACH, "Discharge" );

	standard( PT_ROD_SPREAD, "Rod Spread" );
	standard( PT_ROD_LENGTH, "Rod Length" );
	standard( PT_RISE, "Rise Speed" );
	standard( PT_WIND, "Wind" );
	group( PT_ROD_SPREAD, PT_WIND, "Jacob's Ladder" );

	standard( PT_BPS, "BPS" );
	standard( PT_TOPLOAD, "Topload Size" );
	option( PT_TARGET, "Target", { "None", "Floor", "Point" } );
	SetParamInfo( PT_TARGET_X, "Target X", FF_TYPE_XPOS, params[ PT_TARGET_X ] );
	SetParamInfo( PT_TARGET_Y, "Target Y", FF_TYPE_YPOS, params[ PT_TARGET_Y ] );
	group( PT_BPS, PT_TARGET_Y, "Tesla Coil" );

	standard( PT_BELT, "Belt Current" );
	standard( PT_SPHERE, "Sphere Size" );
	standard( PT_GAP, "Gap" );
	group( PT_BELT, PT_GAP, "Van de Graaff" );

	standard( PT_GLOBE, "Globe Size" );
	SetParamInfo( PT_FINGER, "Finger", FF_TYPE_BOOLEAN, params[ PT_FINGER ] > 0.5f );
	SetParamInfo( PT_FINGER_X, "Finger X", FF_TYPE_XPOS, params[ PT_FINGER_X ] );
	SetParamInfo( PT_FINGER_Y, "Finger Y", FF_TYPE_YPOS, params[ PT_FINGER_Y ] );
	group( PT_GLOBE, PT_FINGER_Y, "Plasma Globe" );

	option( PT_ORIGIN, "Origin", { "Point", "Edge" } );
	group( PT_ORIGIN, PT_ORIGIN, "Lichtenberg" );

	// An FFT buffer: Resolume shows an audio-source picker and writes one
	// spectrum bin per element. Zero defaults, so with no audio routed nothing
	// twitches to a phantom signal.
	SetBufferParamInfo( PT_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, static_cast< unsigned int >( i ), "", 0.0f );
	standard( PT_AUDIO_FIRES, "Audio Fires" );
	standard( PT_AUDIO_DRIVE, "Audio Drive" );
	group( PT_AUDIO, PT_AUDIO_DRIVE, "Audio" );

	standard( PT_EFFICIENCY, "Efficiency" );
	standard( PT_GLOW, "Glow" );
	option( PT_GAS, "Gas", { "Air", "Neon", "Argon", "Neon-Xenon" } );
	standard( PT_SHUTTER, "Shutter" );
	standard( PT_PERSISTENCE, "Persistence" );
	SetParamInfo( PT_APPARATUS, "Show Apparatus", FF_TYPE_BOOLEAN, params[ PT_APPARATUS ] > 0.5f );
	// Consecutive red/green/blue is what makes a host draw a colour swatch.
	SetParamInfo( PT_BACK_R, "Background", FF_TYPE_RED, params[ PT_BACK_R ] );
	SetParamInfo( PT_BACK_G, "Background Green", FF_TYPE_GREEN, params[ PT_BACK_G ] );
	SetParamInfo( PT_BACK_B, "Background Blue", FF_TYPE_BLUE, params[ PT_BACK_B ] );
	group( PT_EFFICIENCY, PT_BACK_B, "Light" );

	option( PT_DETECT, "Detect On", { "Luma", "Alpha", "Edges" } );
	standard( PT_THRESHOLD, "Ground Threshold" );
	standard( PT_ILLUMINATION, "Illumination" );
	standard( PT_MIX, "Mix" );
	group( PT_DETECT, PT_MIX, "Over" );

	SetParamInfo( PT_POS_X, "Position X", FF_TYPE_XPOS, params[ PT_POS_X ] );
	SetParamInfo( PT_POS_Y, "Position Y", FF_TYPE_YPOS, params[ PT_POS_Y ] );
	standard( PT_SCALE, "Scale" );
	standard( PT_ROTATION, "Rotation" );
	group( PT_POS_X, PT_ROTATION, "Layout" );

	// Inline: SetParamInfo is protected, so no helper outside the class can
	// call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	group( PT_ABOUT_TEXT, PT_COUNT - 1, "About" );
}

//---------------------------------------------------------------------------
// Parameters.
//---------------------------------------------------------------------------
FFResult FlybackPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_FIRE )
	{
		// An event arrives as 1 on press and 0 on release: fire on the edge.
		const bool down = value >= 0.5f;
		if( down && !fireHeld )
			firePending = true;
		fireHeld          = down;
		params[ PT_FIRE ] = down ? 1.0f : 0.0f;
		return FF_SUCCESS;
	}
	params[ index ] = value;
	return FF_SUCCESS;
}

float FlybackPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

float FlybackPlugin::Effective( unsigned int index ) const
{
	const int preset = Option( params[ PT_PRESET ], presets::kCount + 1 );
	if( preset > 0 )
	{
		const presets::Preset& row = presets::kPresets[ preset - 1 ];
		for( int c = 0; c < presets::kParamCount; ++c )
			if( kPresetColumns[ c ] == index )
				return row.v[ c ];
	}
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

const unsigned int* FlybackPlugin::PresetColumnsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetColumns;
}

char* FlybackPlugin::GetParameterDisplay( unsigned int index )
{
	// Resolved on demand from the params, never from a cache filled while
	// rendering: a host asks as it applies a value, before any frame has
	// seen it (vectrix's trap). And never CFFGLPlugin's fallback, which
	// dereferences a host pointer that is null outside a host.
	float effective[ PT_COUNT ];
	for( unsigned int i = 0; i < PT_COUNT; ++i )
		effective[ i ] = Effective( i );
	display = Display( index, effective );
	if( display.empty() )
	{
		const unsigned int type = GetParamType( index );
		if( type == FF_TYPE_TEXT )
			return GetTextParameter( index );
		char buffer[ 32 ] = {};
		std::snprintf( buffer, sizeof( buffer ), "%.3f", index < PT_COUNT ? effective[ index ] : 0.0f );
		display = buffer;
	}
	return const_cast< char* >( display.c_str() );
}

FFResult FlybackPlugin::SetTextParameter( unsigned int index, const char* )
{
	return index == PT_ABOUT_TEXT ? FF_SUCCESS : FF_FAIL;
}

char* FlybackPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string line = stoatworks::about::textParam( 0 );
		return const_cast< char* >( line.c_str() );
	}
	return const_cast< char* >( "" );
}

//---------------------------------------------------------------------------
// Time.
//---------------------------------------------------------------------------
FFResult FlybackPlugin::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void FlybackPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void FlybackPlugin::UpdateClock()
{
	const double wallNow = WallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	// hostTime is uninitialised in CFFGLPlugin until SetTime lands.
	const double raw = hostTimeSeen ? hostTime : -1.0;
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale  = millisVotes > secondsVotes ? 0.001 : 1.0;
				settledJump = true;
				diag::info( std::string( "host clock is " ) + ( clockScale == 0.001 ? "milliseconds" : "seconds" ) );
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled, the real clock: wrong in origin, right in rate.
	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
// GL.
//---------------------------------------------------------------------------
FFResult FlybackPlugin::InitGL( const FFGLViewportStruct* viewport )
{
	diag::init();
	const GLubyte* version = glGetString( GL_VERSION );
	const GLubyte* gpu     = glGetString( GL_RENDERER );
	diag::info( std::string( "InitGL, GL " ) + ( version ? reinterpret_cast< const char* >( version ) : "?" ) + " on "
	            + ( gpu ? reinterpret_cast< const char* >( gpu ) : "?" ) );
	rendererReady = renderer.InitGL();
	if( !rendererReady )
	{
		renderer.DeInitGL();
		return FF_FAIL;
	}
	return CFFGLPlugin::InitGL( viewport );
}

FFResult FlybackPlugin::DeInitGL()
{
	StopWorker();
	busy       = false;
	busyResult = false;
	haveShown  = false;
	renderer.DeInitGL();
	rendererReady = false;
	return FF_SUCCESS;
}

FFResult FlybackPlugin::ProcessOpenGL( ProcessOpenGLStruct* input )
{
	if( !rendererReady || input == nullptr )
		return FF_FAIL;

	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, viewport );
	GLuint clip = 0;
	float maxU = 1.0f, maxV = 1.0f;
	if( overInput )
	{
		if( input->numInputTextures < 1 || input->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct& texture = *input->inputTextures[ 0 ];
		clip                             = texture.Handle;
		const FFGLTexCoords coords       = GetMaxGLTexCoords( texture );
		maxU                             = coords.s;
		maxV                             = coords.t;
	}
	if( viewport[ 2 ] <= 0 || viewport[ 3 ] <= 0 )
		return FF_FAIL;
	const double aspect = static_cast< double >( viewport[ 2 ] ) / viewport[ 3 ];

	//-----------------------------------------------------------------------
	// The clock, and the frame's length for the shutter and the persistence.
	//-----------------------------------------------------------------------
	UpdateClock();
	double period = lastNow >= 0.0 ? now - lastNow : 1.0 / 60.0;
	const bool jumped = lastNow >= 0.0 && ( period < 0.0 || period > 0.5 || settledJump );
	if( jumped || period <= 0.0 )
		period = 1.0 / 60.0;
	period      = std::clamp( period, 1.0 / 240.0, 0.25 );
	settledJump = false;
	lastNow     = now;

	//-----------------------------------------------------------------------
	// Audio: the spectrum through the analyser, primed on its first frame.
	//-----------------------------------------------------------------------
	float effective[ PT_COUNT ];
	for( unsigned int i = 0; i < PT_COUNT; ++i )
		effective[ i ] = Effective( i );
	{
		float bins[ audio::kBins ] = {};
		int count                  = 0;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			count = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < count; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		if( jumped )
			analyser.Reset();
		audio::Settings settings;
		settings.sensitivity = std::clamp( effective[ PT_AUDIO_FIRES ], 0.0f, 1.0f );
		analyser.Update( bins, count, jumped ? 0.0f : static_cast< float >( period ), settings );
	}

	Resolved r = Resolve( effective, analyser.Level(), aspect );

	//-----------------------------------------------------------------------
	// Over: last frame's ground mask for this frame's lattice.
	//-----------------------------------------------------------------------
	if( overInput )
	{
		const int mw = 160;
		const int mh = std::max( 16, static_cast< int >( std::lround( mw / aspect ) ) );
		if( renderer.Ground( clip, maxU, maxV, r.detect, r.threshold, mw, mh, groundMask ) )
			++groundStamp;
		if( !groundMask.empty() )
		{
			r.engine.clip      = &groundMask;
			r.engine.clipW     = mw;
			r.engine.clipH     = mh;
			r.engine.clipStamp = groundStamp;
		}
	}

	r.look.decay        = r.persistenceSeconds > 0.0f ? static_cast< float >( std::exp( -period / r.persistenceSeconds ) ) : 0.0f;
	r.look.clearHistory = jumped;

	Job next;
	next.settings = r.engine;
	if( r.engine.clip != nullptr )
		next.clip = *r.engine.clip;
	next.fire   = firePending || ( r.audioFires > 0.0f && analyser.Fired() );
	next.now    = now;
	next.period = period;
	next.look   = r.look;
	firePending = false;

	if( synchronous )
	{
		RunJob( next );
		return renderer.Render( frame, next.look, input->HostFBO, viewport, clip, maxU, maxV ) ? FF_SUCCESS : FF_FAIL;
	}

	// Last frame's step is (nearly always) done by now: collect it, start this
	// frame's, and draw the one collected.
	Wait();
	if( busyResult )
	{
		std::swap( frame, shown );
		shownLook = job.look;
		haveShown = true;
		busyResult = false;
	}
	if( !worker.joinable() )
		StartWorker();
	Submit( std::move( next ) );

	if( !haveShown )
	{
		// The very first frame: nothing has been computed yet. The apparatus
		// with no light on it, rather than a black frame.
		Frame empty;
		return renderer.Render( empty, r.look, input->HostFBO, viewport, clip, maxU, maxV ) ? FF_SUCCESS : FF_FAIL;
	}
	// The look travels with its frame, except where it is the clip's: this
	// frame's clip is what the effect is drawn over.
	return renderer.Render( shown, shownLook, input->HostFBO, viewport, clip, maxU, maxV ) ? FF_SUCCESS : FF_FAIL;
}

//---------------------------------------------------------------------------
// The worker.
//---------------------------------------------------------------------------
void FlybackPlugin::RunJob( Job& j )
{
	if( !j.clip.empty() )
		j.settings.clip = &j.clip;
	else
		j.settings.clip = nullptr;
	engine.Configure( j.settings );
	if( j.fire )
		engine.Fire();
	engine.Advance( j.now, j.period, frame );
	lastEngineMillis.store( engine.LastMillis() );
}

void FlybackPlugin::StartWorker()
{
	quitting = false;
	worker   = std::thread( [ this ] { WorkerLoop(); } );
}

void FlybackPlugin::StopWorker()
{
	if( !worker.joinable() )
		return;
	{
		std::lock_guard< std::mutex > lock( jobMutex );
		quitting = true;
	}
	jobSignal.notify_all();
	worker.join();
}

void FlybackPlugin::Submit( Job&& next )
{
	{
		std::lock_guard< std::mutex > lock( jobMutex );
		job  = std::move( next );
		busy = true;
	}
	jobSignal.notify_all();
}

void FlybackPlugin::Wait()
{
	std::unique_lock< std::mutex > lock( jobMutex );
	jobSignal.wait( lock, [ this ] { return !busy; } );
}

void FlybackPlugin::WorkerLoop()
{
	std::unique_lock< std::mutex > lock( jobMutex );
	for( ;; )
	{
		jobSignal.wait( lock, [ this ] { return busy || quitting; } );
		if( quitting )
			return;
		lock.unlock();
		RunJob( job );
		lock.lock();
		busy       = false;
		busyResult = true;
		jobSignal.notify_all();
	}
}

FlybackPlugin::~FlybackPlugin()
{
	StopWorker();
}

} // namespace flyback
