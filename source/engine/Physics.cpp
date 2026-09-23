#include "Physics.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace flyback::physics
{
double PeekSphere( double radiusMetres )
{
	const double rcm = std::max( radiusMetres * 100.0, 1e-3 );
	return 27.2e5 * ( 1.0 + 0.54 / std::sqrt( rcm ) );
}

//---------------------------------------------------------------------------
Supply NominalSupply( SupplyKind kind )
{
	switch( kind )
	{
	case SupplyKind::ZVS: return { 20e3, 8e5 };    // 25 mA short-circuit, 125 W available
	case SupplyKind::NST: return { 15e3, 5e5 };    // 30 mA, the classic 15/30 sign transformer
	case SupplyKind::Flyback: return { 25e3, 4e6 };// 6 mA, a single-transistor driver
	default: return { 15e3, 5e5 };
	}
}

const char* SupplyName( SupplyKind kind )
{
	switch( kind )
	{
	case SupplyKind::ZVS: return "ZVS";
	case SupplyKind::NST: return "NST";
	case SupplyKind::Flyback: return "Flyback";
	default: return "?";
	}
}

//---------------------------------------------------------------------------
double ArcCurrent( const Ayrton& arc, const Supply& supply, double length )
{
	const double b    = supply.openVolts - arc.A - arc.B * length;
	const double c    = arc.C + arc.D * length;
	const double disc = b * b - 4.0 * supply.sourceOhms * c;
	if( b <= 0.0 || disc < 0.0 )
		return 0.0;
	return ( b + std::sqrt( disc ) ) / ( 2.0 * supply.sourceOhms );
}

double ExtinctionLength( const Ayrton& arc, const Supply& supply )
{
	const double v     = supply.openVolts - arc.A;
	const double alpha = arc.B * arc.B;
	const double beta  = 2.0 * arc.B * v + 4.0 * supply.sourceOhms * arc.D;
	const double gamma = v * v - 4.0 * supply.sourceOhms * arc.C;
	if( v <= 0.0 || gamma <= 0.0 )
		return 0.0;
	const double disc = beta * beta - 4.0 * alpha * gamma;
	if( disc < 0.0 )
		return 0.0;
	// 2 gamma / (beta + sqrt(disc)) is the smaller root without the
	// cancellation of (beta - sqrt(disc)) / 2 alpha.
	return 2.0 * gamma / ( beta + std::sqrt( disc ) );
}

double StrikeGap( const Supply& supply )
{
	return supply.openVolts / kAirBreakdown;
}

//---------------------------------------------------------------------------
TwoSpheres SolveTwoSpheres( double a, double b, double gap )
{
	TwoSpheres out;
	const double k  = 4.0 * kPi * kEpsilon0;
	const double s  = a + gap + b;// centre to centre
	const double pa = a;          // the driven sphere's facing point
	const double pb = s - b;      // the grounded sphere's

	struct Charge
	{
		double q, x;
	};
	std::vector< Charge > charges;
	charges.push_back( { k * a, 0.0 } );// the driven sphere at 1 V, alone

	double inDriven = k * a;
	Charge last     = charges.back();
	for( int n = 0; n < 400; ++n )
	{
		// Its image in the grounded sphere...
		const double r  = s - last.x;
		const Charge gb = { -last.q * b / r, s - b * b / r };
		charges.push_back( gb );
		// ...and that image's in the driven one, which keeps it at 1 V.
		const double r1 = gb.x;
		const Charge ga = { -gb.q * a / r1, a * a / r1 };
		charges.push_back( ga );
		inDriven += ga.q;
		last = ga;
		out.images += 2;
		if( std::fabs( ga.q ) < 1e-14 * k * a )
			break;
	}

	double ea = 0.0, eb = 0.0;
	for( const Charge& c : charges )
	{
		const double da = pa - c.x;
		const double db = pb - c.x;
		ea += c.q * da / ( std::fabs( da ) * da * da );
		eb += c.q * db / ( std::fabs( db ) * db * db );
	}
	for( const Charge& c : charges )
		out.charges.push_back( { c.q, c.x } );
	out.capacitance   = inDriven;
	out.fieldDriven   = std::fabs( ea ) / k;
	out.fieldGrounded = std::fabs( eb ) / k;
	return out;
}

double VdgBreakdownVolts( const TwoSpheres& spheres, double a, double b )
{
	const double va = PeekSphere( a ) / std::max( spheres.fieldDriven, 1e-12 );
	const double vb = PeekSphere( b ) / std::max( spheres.fieldGrounded, 1e-12 );
	return std::min( va, vb );
}

//---------------------------------------------------------------------------
double ToploadCapacitance( double radius )
{
	return 4.0 * kPi * kEpsilon0 * radius;
}

double ToploadVolts( const Supply& supply, double radius )
{
	return supply.openVolts * std::sqrt( kTeslaPrimaryFarads / ToploadCapacitance( radius ) );
}

double BangJoules( const Supply& supply, double radius )
{
	const double v = ToploadVolts( supply, radius );
	return 0.5 * ToploadCapacitance( radius ) * v * v;
}

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
namespace
{
double Lobe( double l, double mu, double s1, double s2 )
{
	const double t = ( l - mu ) / ( l < mu ? s1 : s2 );
	return std::exp( -0.5 * t * t );
}

/// CIE 1931 2-degree observer, Wyman, Sloan & Shirley's multi-lobe fit.
void Observer( double l, double& x, double& y, double& z )
{
	x = 1.056 * Lobe( l, 599.8, 37.9, 31.0 ) + 0.362 * Lobe( l, 442.0, 16.0, 26.7 ) - 0.065 * Lobe( l, 501.1, 20.4, 26.2 );
	y = 0.821 * Lobe( l, 568.8, 46.9, 40.5 ) + 0.286 * Lobe( l, 530.9, 16.3, 31.1 );
	z = 1.217 * Lobe( l, 437.0, 11.8, 36.0 ) + 0.681 * Lobe( l, 459.0, 26.0, 13.8 );
}

/// XYZ to linear sRGB (D65), then brought into gamut by desaturating toward
/// white -- adding the most negative channel's magnitude to all three -- and
/// normalised to sum 1: these are energy weights, so a channel can only take
/// a share. Violet sits outside sRGB. Clipping its negative red to zero, as
/// the first version did, turned argon's lavender pure blue; desaturating keeps
/// the hue, which is what a camera's gamut mapping does too.
Rgb ToWeights( double X, double Y, double Z )
{
	double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
	double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
	double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
	const double low = std::min( { r, g, b } );
	if( low < 0.0 )
	{
		r -= low;
		g -= low;
		b -= low;
	}
	const double sum = r + g + b;
	if( sum <= 0.0 )
		return { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
	return { static_cast< float >( r / sum ), static_cast< float >( g / sum ), static_cast< float >( b / sum ) };
}

struct Line
{
	double nm, weight;
};

Rgb FromLines( const Line* lines, int count )
{
	double X = 0, Y = 0, Z = 0;
	for( int i = 0; i < count; ++i )
	{
		double x, y, z;
		Observer( lines[ i ].nm, x, y, z );
		X += lines[ i ].weight * x;
		Y += lines[ i ].weight * y;
		Z += lines[ i ].weight * z;
	}
	return ToWeights( X, Y, Z );
}

// Air: the N2 second positive system (C -> B) and the N2+ first negative
// (B -> X), the emission of corona and streamers in air.
//
// The second positive bands are the v' = 0 progression, weighted by their
// Franck-Condon factors times nu^4 (radiant energy per band goes as q nu^4 for
// a fixed upper-state population): q(0,v'') = 0.500, 0.319, 0.101, 0.0488,
// 0.0247 for v'' = 0..4 -- R. W. Nicholls, "Franck-Condon factors to high
// vibrational quantum numbers I: N2 and N2+", J. Res. NBS 65A (1961), table 2.
// The v' = 1 and 2 progressions are left out: the copy of that table read in
// this session was not legible enough to trust, and they add mostly UV.
//
// The first negative's 391.4 nm band is taken at 0.2 of 337.1. That ratio
// is NOT from a source: it depends on the reduced field (it is how streamer
// fields are measured), and published corona spectra put it anywhere from a
// few percent to comparable. 427.8 nm follows from N2+ B-X Franck-Condon
// factors q00 = 0.66, q01 = 0.25 (from memory) and the same nu^4.
//
// Only 380 nm and up is visible, so the colour is set almost entirely by
// 380.5, 391.4, 405.9, 427.8 and 434.4 nm.
constexpr Line kAir[] = {
	{ 337.1, 1.000 }, { 357.7, 0.503 }, { 380.5, 0.124 }, { 405.9, 0.0465 }, { 434.4, 0.0179 },
	{ 391.4, 0.200 }, { 427.8, 0.053 },
};

// Neon's red-orange forest (NIST ASD strong lines; weights approximate).
constexpr Line kNeon[] = {
	{ 585.2, 0.80 }, { 588.2, 0.50 }, { 594.5, 0.50 }, { 603.0, 0.30 }, { 607.4, 0.50 }, { 609.6, 0.50 },
	{ 614.3, 0.70 }, { 616.4, 0.30 }, { 621.7, 0.30 }, { 626.6, 0.50 }, { 633.4, 0.60 }, { 638.3, 0.70 },
	{ 640.2, 1.00 }, { 650.6, 0.60 }, { 659.9, 0.30 }, { 667.8, 0.30 }, { 692.9, 0.40 }, { 703.2, 0.50 },
};

// Argon: the red Ar I lines the eye barely sees, and the blue Ar I / Ar II
// lines that make an argon glow lavender (weights approximate).
constexpr Line kArgon[] = {
	{ 415.9, 0.20 }, { 420.1, 0.20 }, { 427.2, 0.10 }, { 430.0, 0.10 }, { 434.8, 0.30 }, { 454.5, 0.20 },
	{ 460.9, 0.30 }, { 476.5, 0.40 }, { 480.6, 0.40 }, { 488.0, 0.60 }, { 696.5, 0.40 }, { 706.7, 0.30 },
	{ 738.4, 0.30 }, { 750.4, 0.60 }, { 763.5, 0.80 },
};
// Xenon's strong visible Xe I lines, blue-green (NIST ASD; weights
// approximate).
constexpr Line kXenon[] = {
	{ 450.1, 0.40 }, { 452.5, 0.30 }, { 462.4, 0.80 }, { 467.1, 1.00 }, { 473.4, 0.50 },
	{ 480.7, 0.60 }, { 482.9, 0.40 }, { 484.4, 0.30 }, { 491.7, 0.40 },
};
} // namespace

Rgb StreamerColour( Gas gas )
{
	switch( gas )
	{
	case Gas::Neon: return FromLines( kNeon, static_cast< int >( sizeof( kNeon ) / sizeof( kNeon[ 0 ] ) ) );
	case Gas::Argon: return FromLines( kArgon, static_cast< int >( sizeof( kArgon ) / sizeof( kArgon[ 0 ] ) ) );
	case Gas::NeonXenon:
	{
		// Both spectra, the xenon at twice the neon's line weight: xenon's
		// lower excitation energy lets a small fraction of it emit out of
		// proportion to its share -- which is why a globe is violet, not the
		// orange a pure-neon one would be. The ratio is a look, and says so.
		Line mix[ sizeof( kNeon ) / sizeof( kNeon[ 0 ] ) + sizeof( kXenon ) / sizeof( kXenon[ 0 ] ) ];
		int n = 0;
		for( const Line& l : kNeon )
			mix[ n++ ] = { l.nm, 0.5 * l.weight };
		for( const Line& l : kXenon )
			mix[ n++ ] = { l.nm, 1.0 * l.weight };
		return FromLines( mix, n );
	}
	default: return FromLines( kAir, static_cast< int >( sizeof( kAir ) / sizeof( kAir[ 0 ] ) ) );
	}
}

Rgb BlackbodyColour( double kelvin )
{
	constexpr double h = 6.62607015e-34, c = 2.99792458e8, kb = 1.380649e-23;
	double X = 0, Y = 0, Z = 0;
	for( double l = 380.0; l <= 780.0; l += 5.0 )
	{
		const double m  = l * 1e-9;
		const double bb = 1.0 / ( std::pow( m, 5.0 ) * ( std::exp( h * c / ( m * kb * kelvin ) ) - 1.0 ) );
		double x, y, z;
		Observer( l, x, y, z );
		X += bb * x;
		Y += bb * y;
		Z += bb * z;
	}
	return ToWeights( X, Y, Z );
}

const char* GasName( Gas gas )
{
	switch( gas )
	{
	case Gas::Air: return "Air";
	case Gas::Neon: return "Neon";
	case Gas::Argon: return "Argon";
	case Gas::NeonXenon: return "Neon-Xenon";
	default: return "?";
	}
}
} // namespace flyback::physics
