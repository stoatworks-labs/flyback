#pragma once

#include "Dbm.h"
#include "Physics.h"
#include "Rng.h"
#include "Tree.h"

#include <cstdint>
#include <memory>
#include <vector>

/**
    The machines, and the clock they run on.

    Five circuits around one growth model. Each decides WHEN the air breaks
    and HOW MUCH energy the channel carries; the dielectric breakdown model
    (Dbm.h) decides WHERE. Nothing here touches GL: the engine hands the
    renderer a list of light-carrying segments in scene metres, each with the
    joules of light it emitted inside this frame's shutter, and the shapes of
    the apparatus. Everything in it can be run, and measured, without a GPU.

    ## Time

    The engine runs on the host's clock in double-precision seconds. Events
    happen at their own exact times -- a bang at k / BPS, a Van de Graaff
    spark when the charge reaches C V_b -- not at frame boundaries. A frame
    asks for everything between the last frame and now, and gets the light
    of whatever happened while its shutter was open:

        shutter window = ( now - shutter * frame period, now ]

    A spark lasts microseconds, so it lands in exactly one frame at a 360
    degree shutter (the windows tile time), and in one or none at 180 (half
    of time is between windows). A sustained arc integrates its power over
    the open part of the frame. This is `--exposure`.

    A jump in the host clock -- a clip retriggered, a seek, a stall -- does
    not replay the gap: the engine re-anchors and carries on.
*/
namespace flyback
{
enum class Machine : int
{
	Ladder = 0,
	Tesla,
	VanDeGraaff,
	Globe,
	Lichtenberg,
	Count
};
const char* MachineName( Machine machine );

/// The machine's own framing: how many metres of scene the frame's height
/// shows, and the camera's exposure for it (display units per J/m^2 of
/// light). A photographer sets a different exposure for a 100 W arc and a
/// 0.1 J spark; so does this. Neither affects the physics.
double SceneHeight( Machine machine );

/// The engine's inputs, all in physical units. Controls.cpp makes these.
struct Settings
{
	Machine machine = Machine::Tesla;
	uint32_t seed   = 1;

	physics::Supply supply;
	double eta       = 2.5;///< the breakdown model's exponent
	int cellsHigh    = 104;///< lattice sites across the scene's height
	double memory    = 0.02;///< channel memory tau, seconds
	double reach     = 1.0;///< multiplier on each machine's nominal growth per event

	// Jacob's ladder
	double rodSpread = 0.35;///< radians between the rods
	double rodLength = 0.45;///< metres
	double rise      = 1.0; ///< buoyant rise of the hot column, m/s
	double wind      = 0.0; ///< sideways air, m/s

	// Tesla coil
	double bps      = 120.0;///< interrupter, bangs per second (0: off)
	double topload  = 0.15; ///< toroid major radius, metres
	int target      = 1;    ///< 0 none, 1 floor, 2 point
	double targetX  = 0.78, targetY = 0.22;///< frame coordinates 0..1, y up

	// Van de Graaff
	double belt   = 10e-6;///< amperes
	double sphere = 0.12; ///< metres
	double gap    = 0.06; ///< metres, surface to surface
	double finish = 1.0;  ///< Peek's irregularity factor m_v: 1 polished, down to 0.82

	// Plasma globe
	double globe   = 0.20;///< glass radius, metres
	bool finger    = false;
	double fingerX = 0.72, fingerY = 0.62;///< frame coordinates

	// Lichtenberg
	int origin = 0;///< 0 a point at the centre, 1 the bottom edge

	// Light
	double efficiency = 0.01;///< fraction of the electrical energy emitted as light
	double shutter    = 1.0; ///< fraction of the frame the shutter is open (360 deg = 1)

	// Frame
	double aspect = 16.0 / 9.0;

	// Layout: scene to frame. Frame coordinates are in frame heights, centred.
	double posX = 0.0, posY = 0.0, scale = 1.0, rotation = 0.0;

	// Over: the clip as ground. `clip` is w x h, 0/1, row 0 at the bottom, in
	// frame coordinates; empty for the source build.
	const std::vector< uint8_t >* clip = nullptr;
	int clipW = 0, clipH = 0;
	uint64_t clipStamp = 0;///< changes whenever the clip mask does
};

/// A piece of channel and the light it gave off this frame.
struct Segment
{
	float x0, y0, x1, y1;///< scene metres
	float joules;        ///< light, not electrical energy
	float radius;        ///< the luminous radius, metres
	float thermal;       ///< 0 a cold streamer, 1 a thermal arc: the colour
};

/// A piece of apparatus, for Show Apparatus. Scene metres.
struct Shape
{
	enum Kind : int
	{
		Capsule = 0,///< segment (x0,y0)-(x1,y1), radius r: rods, a toroid seen side on, a column
		Disc,       ///< centre (x0,y0), radius r: spheres
		Ring,       ///< centre (x0,y0), radius r, thickness x1: the globe's glass
		Box         ///< corners (x0,y0)-(x1,y1): bases, the floor, the slab
	};
	enum Material : int
	{
		Dull = 0,///< a painted base, an insulator
		Metal,   ///< polished aluminium or copper rod: round-shaded, it catches the light
		Copper,  ///< a wound secondary: copper, with its windings
		Glass,   ///< the globe's glass, a stem
		Floor,   ///< the floor
		Acrylic  ///< the Lichtenberg slab
	};
	Kind kind;
	float x0, y0, x1, y1, r;
	float material;///< a Material, as a float for the uniform array
};

struct Frame
{
	std::vector< Segment > segments;
	std::vector< Shape > shapes;
	double joules      = 0.0;///< light in this frame: the sum over segments
	int events         = 0;  ///< discharges whose light landed in this frame
	double sceneHeight = 1.0;
};

/// One discharge, for the harness.
struct Event
{
	double time     = 0.0;
	double joules   = 0.0;///< electrical energy
	double light    = 0.0;///< what landed in a frame (0 if the shutter was shut)
	double length   = 0.0;///< longest root-to-tip path after it, metres
	int nodes       = 0;
	int branches    = 0;  ///< free tips
	bool connected  = false;
	bool exposed    = false;
	int frame       = -1; ///< which Advance() call it landed in
	double groundX = 0, groundY = 0;///< a connected discharge's contact, scene metres
};

/// The Jacob's ladder's state, for the harness.
struct LadderProbe
{
	bool lit            = false;
	double length       = 0.0;///< column length now, metres
	double extinction   = 0.0;///< L*, closed form
	double apex         = 0.0;///< highest point of the column, metres
	double footHeight   = 0.0;///< the feet, metres above the rods' bottom
	double strikeGap    = 0.0;
	double current      = 0.0;
	double power        = 0.0;
	/// How fast the arc's roots slide up the rods, as a fraction of the free
	/// column's rise. Less than 1 is what bows the arc; exactly 1 keeps it
	/// straight, which is what `--ladder` uses for the closed-form climb.
	double rootSpeed    = 0.9;
	std::vector< double > strikes;    ///< times
	std::vector< double > extinctions;///< times
	std::vector< double > lengthsAt;  ///< column length at each extinction
	std::vector< int > overTheTop;    ///< 1 where it went out by running off the top of the rods, not at L*
	std::vector< double > strikeHeights;
};

class Engine
{
public:
	Engine();
	~Engine();

	void Configure( const Settings& settings );
	const Settings& Current() const
	{
		return settings;
	}

	/// Fire now: a bang, a spark, a restrike, a surge, a new figure.
	void Fire();

	/// Everything from the last call to `now`. `framePeriod` is this frame's
	/// length (the shutter is a fraction of it).
	void Advance( double now, double framePeriod, Frame& out );

	/// Forget every discharge and re-anchor the clock (a clip trigger).
	void Restart();

	//-------------------------------------------------------------------
	// For the harness.
	//-------------------------------------------------------------------
	const std::vector< Event >& Events() const
	{
		return events;
	}
	void ClearEvents()
	{
		events.clear();
	}
	const Tree& CurrentTree() const;
	/// The last tree whose light was emitted, whatever machine, whatever became
	/// of it after (a spark's is gone within the frame).
	const Tree& LastEmitted() const;
	Field& CurrentField();
	LadderProbe& Ladder();
	/// Engine CPU time of the last Advance, milliseconds.
	double LastMillis() const
	{
		return lastMillis;
	}
	/// Van de Graaff: breakdown volts, capacitance and the interval they imply.
	double VdgBreakdown() const;
	double VdgCapacitance() const;
	/// Van de Graaff: the sphere's voltage now, its corona onset, the corona
	/// conductance G, and the distance to the room's ground that G uses.
	double VdgVolts() const;
	double VdgCoronaOnset() const;
	double VdgConductance() const;
	double VdgGroundDistance() const;
	/// The Van de Graaff's charging-ODE step, seconds (backward Euler).
	static constexpr double kVdgSubstep = 1e-5;
	/// Tesla: the energy and voltage of a bang.
	double TeslaBangJoules() const;

	class Impl;

private:
	Settings settings;
	std::unique_ptr< Impl > impl;
	std::vector< Event > events;
	double lastMillis = 0.0;
};
} // namespace flyback
