#pragma once

/**
    Every constant and every closed form, in the open.

    Nothing here touches the lattice or GL. Each function is a formula a
    textbook or a paper states, and the comment says which, and says so when
    the number is carried from memory rather than checked against the text in
    this session -- that distinction is written down in AGENTS.md as well,
    because a check against a formula is only as good as the formula.

    SI units throughout: metres, seconds, volts, amperes, farads, joules.
*/
#include <utility>
#include <vector>

namespace flyback::physics
{
constexpr double kPi       = 3.14159265358979323846;
constexpr double kEpsilon0 = 8.8541878128e-12;///< F/m

//---------------------------------------------------------------------------
// Air.
//---------------------------------------------------------------------------

/// Uniform-field breakdown strength of air at STP, V/m. The round number
/// every high-voltage text quotes ("30 kV/cm") and the one the spec asks for:
/// it sets how wide a gap a supply can strike across.
constexpr double kAirBreakdown = 3.0e6;

/// The field a positive streamer needs to keep propagating in air, V/m --
/// about 5 kV/cm. A streamer from a topload at V cannot be longer than
/// V / kStreamerField. (Background knowledge, not checked against a text in
/// this session: the literature quotes 4.5 to 5 kV/cm for positive streamers.)
constexpr double kStreamerField = 5.0e5;

/// Peek's law for the corona-onset / breakdown field at the surface of a
/// sphere of radius r (metres), relative air density 1:
///
///     E_c = 27.2 (1 + 0.54 / sqrt(r_cm)) kV/cm
///
/// F. W. Peek, "Dielectric Phenomena in High Voltage Engineering" (1929).
/// Carried from memory; the cylinder form (30 (1 + 0.301/sqrt r)) is the
/// better known of the two. Returns V/m.
double PeekSphere( double radiusMetres );

//---------------------------------------------------------------------------
// The supply: an open-circuit voltage behind a source resistance.
//---------------------------------------------------------------------------
enum class SupplyKind : int
{
	ZVS = 0,///< a ZVS driver into a flyback: stiff, 20 kV, 25 mA short-circuit
	NST,    ///< a neon-sign transformer: 15 kV, 30 mA, current-limited by its leakage
	Flyback,///< a single-ended flyback driver: 25 kV and weak, 6 mA
	Count
};

struct Supply
{
	double openVolts = 15e3;
	double sourceOhms = 5e5;

	double ShortCircuitAmps() const
	{
		return openVolts / sourceOhms;
	}
	/// Most power it can put into any resistive load, V^2 / 4R.
	double MaxPower() const
	{
		return openVolts * openVolts / ( 4.0 * sourceOhms );
	}
};

/// The nominal supply for each kind. Voltage and Source Impedance trim these.
Supply NominalSupply( SupplyKind kind );
const char* SupplyName( SupplyKind kind );

//---------------------------------------------------------------------------
// The arc: Ayrton's equation, and where it goes out.
//---------------------------------------------------------------------------

/// V_arc = A + B L + (C + D L) / I   (Hertha Ayrton, "The Electric Arc",
/// 1902). The FORM is Ayrton's. Her constants are for carbon arcs at
/// amperes, where the column needs 10.5 W/mm; a Jacob's ladder runs at tens
/// of milliamps, where extrapolating them puts extinction at a centimetre.
/// So the constants here are for the low-current column:
///
/// - A = 350 V, the cathode and anode falls of an atmospheric glow-arc;
/// - B = 1 kV/m, a small current-independent column term;
/// - C = 5 W, the electrode loss;
/// - D = 750 W/m, the column's power per metre at low current. This is
///   the one that decides the ladder: in the limit of a stiff supply the
///   extinction length is V_oc I_sc / 4D, and 750 W/m puts a 15 kV / 30 mA
///   neon-sign transformer's arc out at 13 cm -- the height hobbyists report.
///   That last clause is a calibration against folklore, and says so.
struct Ayrton
{
	double A = 350.0;
	double B = 1.0e3;
	double C = 5.0;
	double D = 750.0;
};

/// The arc of length L on this supply: its current at the stable operating
/// point (the higher of the two intersections of the load line with the
/// arc's falling characteristic), or 0 if the load line misses it.
double ArcCurrent( const Ayrton& arc, const Supply& supply, double length );

/// The extinction length L*, in closed form. Setting the load line equal to
/// Ayrton's equation gives
///
///     R_s I^2 - (V_oc - A - B L) I + (C + D L) = 0,
///
/// which has a real root while its discriminant is non-negative. L* is where
/// the discriminant is zero:
///
///     (V_oc - A - B L)^2 = 4 R_s (C + D L)
///     B^2 L^2 - (2 B (V_oc - A) + 4 R_s D) L + (V_oc - A)^2 - 4 R_s C = 0
///
/// and L* is that quadratic's smaller root (the larger has V_oc - A - BL < 0).
/// Solved with the numerically stable form of the quadratic formula, because
/// B^2 is tiny next to the linear coefficient and the textbook form loses
/// every digit to cancellation.
double ExtinctionLength( const Ayrton& arc, const Supply& supply );

/// The widest gap the supply can strike across: V_oc / kAirBreakdown.
double StrikeGap( const Supply& supply );

//---------------------------------------------------------------------------
// The Van de Graaff: two spheres, one charged by the belt, one grounded.
//---------------------------------------------------------------------------

/// The field and the charge of a sphere of radius `a` at 1 V, facing a
/// grounded sphere of radius `b` across a surface-to-surface gap `d`, by
/// Kelvin's method of images: the charge 4 pi eps0 a at the first sphere's
/// centre, then alternately its image in the grounded sphere (-q b / r at
/// b^2 / r from its centre) and that image's image in the first sphere (to
/// hold it at its potential), until the images are negligible. All the charges
/// lie on the axis, so the field at the two facing points is a sum of 1/r^2
/// terms. (W. Smythe, "Static and Dynamic Electricity", ch. 5; the series is
/// Maxwell's.)
struct TwoSpheres
{
	double capacitance    = 0.0;///< F: charge on the driven sphere per volt
	double fieldDriven    = 0.0;///< V/m per volt at the driven sphere's facing point
	double fieldGrounded  = 0.0;///< V/m per volt at the grounded sphere's facing point
	int images            = 0;
	/// Every charge in the series, (coulombs per volt, x along the axis from
	/// the driven sphere's centre). For `hvtest --vdg`, which checks them
	/// against the boundary conditions rather than trusting this code.
	std::vector< std::pair< double, double > > charges;
};
TwoSpheres SolveTwoSpheres( double a, double b, double gap );

/// The voltage at which the gap breaks down: the higher of the two surface
/// fields reaches Peek's field for that sphere.
double VdgBreakdownVolts( const TwoSpheres& spheres, double a, double b );

/// The discharge sphere is this fraction of the main sphere's radius.
constexpr double kVdgGroundRatio = 0.4;

//---------------------------------------------------------------------------
// The Tesla coil.
//---------------------------------------------------------------------------

/// The primary tank capacitor, farads. A typical MMC for a
/// neon-sign-transformer coil. Fixed: it is not a control.
constexpr double kTeslaPrimaryFarads = 30e-9;

/// The topload's capacitance, approximated as an isolated sphere of the
/// toroid's major radius: 4 pi eps0 R. (A toroid's is somewhat less; the
/// community's empirical formulas were not to hand.)
double ToploadCapacitance( double radius );

/// The topload voltage after lossless resonant transfer from a primary
/// charged to the supply's V_oc: 1/2 C_p V_oc^2 = 1/2 C_top V_top^2.
double ToploadVolts( const Supply& supply, double radius );

/// The energy of one bang, 1/2 C_top V_top^2.
double BangJoules( const Supply& supply, double radius );

//---------------------------------------------------------------------------
// Light: colour from the lines each gas actually emits.
//---------------------------------------------------------------------------
enum class Gas : int
{
	Air = 0,
	Neon,
	Argon,
	NeonXenon,///< the classic plasma-globe fill: mostly neon, a little xenon
	Count
};

struct Rgb
{
	float r = 0, g = 0, b = 0;
};

/// The streamer (cold, non-thermal) emission of a gas, as linear sRGB
/// weights summing to 1, from its strongest visible lines through the CIE
/// 1931 observer (Wyman, Sloan & Shirley's analytic fit, JCGT 2013) and the
/// XYZ-to-linear-sRGB matrix, desaturated into gamut. Air is the N2 second positive system (337,
/// 357, 380 nm; only the violet end is visible) with N2+ 391/428 nm. The
/// line lists and their weights are in Physics.cpp.
Rgb StreamerColour( Gas gas );

/// The thermal (arc) emission: a blackbody at `kelvin`, same weighting.
Rgb BlackbodyColour( double kelvin );

/// Arc temperature: the hot core of an air arc.
constexpr double kArcKelvin = 6500.0;

/// The current at which a channel has thermalised: the glow-to-arc
/// transition in air at atmospheric pressure is at the order of an ampere.
/// A segment's colour moves from the gas's streamer lines toward the arc's
/// blackbody as I / (I + kThermalAmps).
constexpr double kThermalAmps = 0.5;

const char* GasName( Gas gas );
} // namespace flyback::physics
