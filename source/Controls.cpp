#include "Controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace flyback
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double Lerp( double a, double b, double t )
{
	return a + ( b - a ) * t;
}

/// Geometric: lo at 0, hi at 1.
double Geometric( float v, double lo, double hi )
{
	return lo * std::pow( hi / lo, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

float InverseGeometric( double x, double lo, double hi )
{
	return static_cast< float >( std::log( x / lo ) / std::log( hi / lo ) );
}
} // namespace

Nominal NominalFor( Machine machine )
{
	// eta, lattice sites across the scene's height, channel memory, the
	// camera. The camera is a photographer's choice per subject and is set so
	// each machine as built exposes like a photograph of it; it changes no
	// physics and no light total.
	switch( machine )
	{
	case Machine::Ladder: return { 4.0, 128, 0.020, 0.900, physics::SupplyKind::NST };
	case Machine::Tesla: return { 1.6, 136, 0.020, 0.400, physics::SupplyKind::NST };
	case Machine::VanDeGraaff: return { 3.5, 144, 0.020, 4.000, physics::SupplyKind::NST };
	case Machine::Globe: return { 2.0, 96, 0.080, 3.000, physics::SupplyKind::Flyback };
	case Machine::Lichtenberg: return { 1.0, 176, 0.020, 1.000, physics::SupplyKind::ZVS };
	default: return { 1.6, 136, 0.020, 0.400, physics::SupplyKind::NST };
	}
}

int Option( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::clamp( index, 0, count - 1 );
}

//---------------------------------------------------------------------------
double RelativeFromParam( float v, double range )
{
	return std::pow( range, 2.0 * std::clamp( static_cast< double >( v ), 0.0, 1.0 ) - 1.0 );
}

float ParamFromRelative( double factor, double range )
{
	return static_cast< float >( 0.5 * ( std::log( factor ) / std::log( range ) + 1.0 ) );
}

/// Off at the very bottom, then 5 to 1000 bangs a second, geometrically: the
/// musical end and the fizzing end both get room.
double BpsFromParam( float v )
{
	if( v < 0.02f )
		return 0.0;
	return Geometric( ( v - 0.02f ) / 0.98f, 5.0, 1000.0 );
}

float ParamFromBps( double bps )
{
	if( bps <= 0.0 )
		return 0.0f;
	return 0.02f + 0.98f * InverseGeometric( bps, 5.0, 1000.0 );
}

double BeltFromParam( float v )
{
	return Geometric( v, 1e-6, 50e-6 );
}

float ParamFromBelt( double amps )
{
	return InverseGeometric( amps, 1e-6, 50e-6 );
}

double ShutterFromParam( float v )
{
	return 0.02 + 0.98 * std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}

double PersistenceFromParam( float v )
{
	if( v < 0.02f )
		return 0.0;
	return Geometric( ( v - 0.02f ) / 0.98f, 0.005, 0.5 );
}

double TopFromParam( float v )
{
	return Lerp( 0.06, 0.35, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

double SphereFromParam( float v )
{
	return Lerp( 0.05, 0.30, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

double GapFromParam( float v )
{
	return Geometric( v, 0.01, 0.20 );
}

double GlobeFromParam( float v )
{
	return Lerp( 0.08, 0.24, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

double SpreadFromParam( float v )
{
	return Lerp( 8.0, 40.0, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) ) * kPi / 180.0;
}

double RodLengthFromParam( float v )
{
	return Lerp( 0.25, 0.60, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

double RiseFromParam( float v )
{
	return Geometric( v, 0.2, 3.0 );
}

float ParamFromRise( double metresPerSecond )
{
	return InverseGeometric( metresPerSecond, 0.2, 3.0 );
}

/// -0.5 to +0.5 m/s. A breeze is a fraction of the column's own rise: at a
/// metre a second sideways the arc stretched to L* in 65 ms and never climbed.
double WindFromParam( float v )
{
	return Lerp( -0.5, 0.5, std::clamp( static_cast< double >( v ), 0.0, 1.0 ) );
}

//---------------------------------------------------------------------------
Resolved Resolve( const float* p, double level, double aspect )
{
	Resolved out;
	Settings& s    = out.engine;
	const Machine m = static_cast< Machine >( Option( p[ PT_MACHINE ], static_cast< int >( Machine::Count ) ) );
	const Nominal n = NominalFor( m );

	s.machine = m;
	s.seed    = 1u + static_cast< uint32_t >( std::floor( std::clamp( p[ PT_SEED ], 0.0f, 1.0f ) * 999.0f ) );

	const auto kind = static_cast< physics::SupplyKind >( Option( p[ PT_SUPPLY ], static_cast< int >( physics::SupplyKind::Count ) ) );
	physics::Supply supply = physics::NominalSupply( kind );
	const double drive     = 1.0 + std::clamp( static_cast< double >( p[ PT_AUDIO_DRIVE ] ), 0.0, 1.0 ) * std::clamp( level, 0.0, 1.0 );
	supply.openVolts *= RelativeFromParam( p[ PT_VOLTAGE ], 2.0 ) * drive;
	supply.sourceOhms *= RelativeFromParam( p[ PT_IMPEDANCE ], 4.0 );
	s.supply = supply;

	s.eta       = n.eta * RelativeFromParam( p[ PT_BRANCHING ], 2.0 );
	s.cellsHigh = std::clamp( static_cast< int >( std::lround( n.cells * RelativeFromParam( p[ PT_DETAIL ], 2.0 ) ) ), 32, 512 );
	s.memory    = n.memory * RelativeFromParam( p[ PT_MEMORY ], 4.0 );
	s.reach     = RelativeFromParam( p[ PT_REACH ], 4.0 );

	s.rodSpread = SpreadFromParam( p[ PT_ROD_SPREAD ] );
	s.rodLength = RodLengthFromParam( p[ PT_ROD_LENGTH ] );
	s.rise      = RiseFromParam( p[ PT_RISE ] );
	s.wind      = WindFromParam( p[ PT_WIND ] );

	s.bps     = BpsFromParam( p[ PT_BPS ] );
	s.topload = TopFromParam( p[ PT_TOPLOAD ] );
	s.target  = Option( p[ PT_TARGET ], 3 );
	s.targetX = std::clamp( p[ PT_TARGET_X ], 0.0f, 1.0f );
	s.targetY = std::clamp( p[ PT_TARGET_Y ], 0.0f, 1.0f );

	s.belt   = BeltFromParam( p[ PT_BELT ] );
	s.sphere = SphereFromParam( p[ PT_SPHERE ] );
	s.gap    = GapFromParam( p[ PT_GAP ] );

	s.globe   = GlobeFromParam( p[ PT_GLOBE ] );
	s.finger  = p[ PT_FINGER ] > 0.5f;
	s.fingerX = std::clamp( p[ PT_FINGER_X ], 0.0f, 1.0f );
	s.fingerY = std::clamp( p[ PT_FINGER_Y ], 0.0f, 1.0f );

	s.origin = Option( p[ PT_ORIGIN ], 2 );

	s.efficiency = 0.01 * RelativeFromParam( p[ PT_EFFICIENCY ], 10.0 );
	s.shutter    = ShutterFromParam( p[ PT_SHUTTER ] );
	s.aspect     = aspect;

	s.posX     = ( std::clamp( p[ PT_POS_X ], 0.0f, 1.0f ) - 0.5 ) * aspect;
	s.posY     = std::clamp( p[ PT_POS_Y ], 0.0f, 1.0f ) - 0.5;
	s.scale    = RelativeFromParam( p[ PT_SCALE ], 4.0 );
	s.rotation = ( std::clamp( p[ PT_ROTATION ], 0.0f, 1.0f ) - 0.5 ) * 2.0 * kPi;

	Renderer::Look& look = out.look;
	look.sceneHeight     = SceneHeight( m );
	look.posX            = s.posX;
	look.posY            = s.posY;
	look.scale           = s.scale;
	look.rotation        = s.rotation;
	look.camera          = static_cast< float >( n.camera );
	look.glow            = 0.9f * std::clamp( p[ PT_GLOW ], 0.0f, 1.0f );
	look.gas             = physics::StreamerColour( static_cast< physics::Gas >( Option( p[ PT_GAS ], static_cast< int >( physics::Gas::Count ) ) ) );
	look.arc             = physics::BlackbodyColour( physics::kArcKelvin );
	look.apparatus       = p[ PT_APPARATUS ] > 0.5f ? 1.0f : 0.0f;
	look.background[ 0 ] = std::clamp( p[ PT_BACK_R ], 0.0f, 1.0f );
	look.background[ 1 ] = std::clamp( p[ PT_BACK_G ], 0.0f, 1.0f );
	look.background[ 2 ] = std::clamp( p[ PT_BACK_B ], 0.0f, 1.0f );
	look.illumination    = 40.0f * std::clamp( p[ PT_ILLUMINATION ], 0.0f, 1.0f );
	look.mix             = std::clamp( p[ PT_MIX ], 0.0f, 1.0f );

	out.persistenceSeconds = static_cast< float >( PersistenceFromParam( p[ PT_PERSISTENCE ] ) );
	out.detect             = Option( p[ PT_DETECT ], 3 );
	out.threshold          = std::clamp( p[ PT_THRESHOLD ], 0.0f, 1.0f );
	out.audioFires         = std::clamp( p[ PT_AUDIO_FIRES ], 0.0f, 1.0f );
	return out;
}

//---------------------------------------------------------------------------
std::string Display( unsigned int index, const float* p )
{
	const Resolved r = Resolve( p, 0.0, 16.0 / 9.0 );
	const Settings& s = r.engine;
	char buffer[ 64 ] = {};
	switch( index )
	{
	case PT_SEED: std::snprintf( buffer, sizeof( buffer ), "%u", s.seed ); break;
	case PT_VOLTAGE: std::snprintf( buffer, sizeof( buffer ), "%.1f kV", s.supply.openVolts / 1000.0 ); break;
	case PT_IMPEDANCE: std::snprintf( buffer, sizeof( buffer ), "%.2f MOhm", s.supply.sourceOhms / 1e6 ); break;
	case PT_BRANCHING: std::snprintf( buffer, sizeof( buffer ), "eta %.2f", s.eta ); break;
	case PT_DETAIL: std::snprintf( buffer, sizeof( buffer ), "%d sites", s.cellsHigh ); break;
	case PT_MEMORY: std::snprintf( buffer, sizeof( buffer ), "%.0f ms", s.memory * 1000.0 ); break;
	case PT_REACH: std::snprintf( buffer, sizeof( buffer ), "x%.2f", s.reach ); break;
	case PT_ROD_SPREAD: std::snprintf( buffer, sizeof( buffer ), "%.0f deg", s.rodSpread * 180.0 / kPi ); break;
	case PT_ROD_LENGTH: std::snprintf( buffer, sizeof( buffer ), "%.2f m", s.rodLength ); break;
	case PT_RISE: std::snprintf( buffer, sizeof( buffer ), "%.2f m/s", s.rise ); break;
	case PT_WIND: std::snprintf( buffer, sizeof( buffer ), "%+.2f m/s", s.wind ); break;
	case PT_BPS:
		if( s.bps <= 0.0 )
			std::snprintf( buffer, sizeof( buffer ), "off" );
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f BPS", s.bps );
		break;
	case PT_TOPLOAD: std::snprintf( buffer, sizeof( buffer ), "%.2f m", s.topload ); break;
	case PT_BELT: std::snprintf( buffer, sizeof( buffer ), "%.1f uA", s.belt * 1e6 ); break;
	case PT_SPHERE: std::snprintf( buffer, sizeof( buffer ), "%.2f m", s.sphere ); break;
	case PT_GAP: std::snprintf( buffer, sizeof( buffer ), "%.1f cm", s.gap * 100.0 ); break;
	case PT_GLOBE: std::snprintf( buffer, sizeof( buffer ), "%.2f m", s.globe ); break;
	case PT_EFFICIENCY: std::snprintf( buffer, sizeof( buffer ), "%.2f %%", s.efficiency * 100.0 ); break;
	case PT_GLOW: std::snprintf( buffer, sizeof( buffer ), "%.0f %%", r.look.glow * 100.0f ); break;
	case PT_SHUTTER: std::snprintf( buffer, sizeof( buffer ), "%.0f deg", s.shutter * 360.0 ); break;
	case PT_PERSISTENCE:
		if( r.persistenceSeconds <= 0.0f )
			std::snprintf( buffer, sizeof( buffer ), "off" );
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f ms", r.persistenceSeconds * 1000.0 );
		break;
	case PT_ILLUMINATION: std::snprintf( buffer, sizeof( buffer ), "x%.1f", r.look.illumination ); break;
	case PT_SCALE: std::snprintf( buffer, sizeof( buffer ), "x%.2f", s.scale ); break;
	case PT_ROTATION: std::snprintf( buffer, sizeof( buffer ), "%.0f deg", s.rotation * 180.0 / kPi ); break;
	default: return std::string();
	}
	return buffer;
}
} // namespace flyback
