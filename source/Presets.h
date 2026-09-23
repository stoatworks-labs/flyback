#pragma once

/**
    Factory presets: one per machine, each the machine as a photographer would
    shoot it.

    **Presets are an OVERRIDE, not a write.** Resolume does not consume value
    events, so a plugin cannot push a preset's values back into the inspector;
    if it changed its own parameters the sliders would keep showing the old
    numbers. So while the dropdown is on anything but Custom, the row's values
    are laid over the operator's at read time (`FlybackPlugin::Effective`),
    and for those columns the inspector is not the truth. Element 0 of the
    dropdown is Custom and is not in this table: it means "the controls are the
    truth". (graticule's model, and its reasoning.)

    **Row 1 is the constructor's defaults.** The Tesla coil, as the plugin
    starts up. `hvtest --defaults` fails when they go out of step -- the fleet
    learned on escapement that a preset retuned without its defaults ships
    quietly wrong.

    **Standard parameters hold the host-facing 0..1; option and boolean
    parameters hold their real value** (an element index, 0 or 1).

    What a row covers: the machine and its own group, the supply, the
    discharge and the light. What it leaves to the operator: Fire and Seed,
    Detail (a cost, not a look), the positions (Target X/Y, Finger X/Y), audio,
    Show Apparatus, Background, the Over group and the layout.
*/
namespace flyback::presets
{
enum Param
{
	kMachine,
	kSupply,
	kVoltage,
	kImpedance,
	kBranching,
	kMemory,
	kReach,
	kRodSpread,
	kRodLength,
	kRise,
	kWind,
	kBps,
	kTopload,
	kTarget,
	kBelt,
	kSphere,
	kGap,
	kFinish,
	kGlobe,
	kFinger,
	kOrigin,
	kEfficiency,
	kGlow,
	kGas,
	kShutter,
	kPersistence,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Options: Machine 0 Jacob's Ladder / 1 Tesla Coil / 2 Van de Graaff /
// 3 Plasma Globe / 4 Lichtenberg; Supply 0 ZVS / 1 NST / 2 Flyback; Target
// 0 None / 1 Floor / 2 Point; Gas 0 Air / 1 Neon / 2 Argon / 3 Neon-Xenon; Origin 0 Point /
// 1 Edge. Relative controls are 0.5 at each machine's nominal (Controls.h).
// 0.6078 is 120 BPS, 0.3103 a 0.15 m toroid, 0.5886 10 uA, 0.4000 a 0.15 m
// sphere, 0.7686 a 10 cm gap, 0.7500 a 0.20 m globe, 0.3750 20 degrees,
// 0.4286 0.40 m rods, 0.5943 1 m/s.
inline constexpr Preset kPresets[] = {
	// The coil as the plugin starts: an NST coil at 120 BPS, streamers to the
	// floor. The constructor's defaults, named so they stay reachable.
	{ "Tesla Coil",
	  { /*Machine*/ 1, /*Supply*/ 1, /*Voltage*/ 0.5f, /*Imp*/ 0.5f, /*Branch*/ 0.5f, /*Memory*/ 0.5f, /*Reach*/ 0.5f,
	    /*Spread*/ 0.3750f, /*RodLen*/ 0.4286f, /*Rise*/ 0.5943f, /*Wind*/ 0.5f, /*BPS*/ 0.6078f, /*Top*/ 0.3103f,
	    /*Target*/ 1, /*Belt*/ 0.5886f, /*Sphere*/ 0.4000f, /*Gap*/ 0.7686f, /*Finish*/ 1.0f, /*Globe*/ 0.7500f, /*Finger*/ 0,
	    /*Origin*/ 0, /*Eff*/ 0.5f, /*Glow*/ 0.45f, /*Gas*/ 0, /*Shutter*/ 1.0f, /*Persist*/ 0.30f } },

	// A neon-sign transformer on 40 cm rods: the arc climbs, bows and snaps.
	{ "Jacob's Ladder",
	  { /*Machine*/ 0, /*Supply*/ 1, /*Voltage*/ 0.5f, /*Imp*/ 0.5f, /*Branch*/ 0.5f, /*Memory*/ 0.5f, /*Reach*/ 0.5f,
	    /*Spread*/ 0.3750f, /*RodLen*/ 0.4286f, /*Rise*/ 0.5943f, /*Wind*/ 0.5f, /*BPS*/ 0.6078f, /*Top*/ 0.3103f,
	    /*Target*/ 1, /*Belt*/ 0.5886f, /*Sphere*/ 0.4000f, /*Gap*/ 0.7686f, /*Finish*/ 1.0f, /*Globe*/ 0.7500f, /*Finger*/ 0,
	    /*Origin*/ 0, /*Eff*/ 0.5f, /*Glow*/ 0.50f, /*Gas*/ 0, /*Shutter*/ 1.0f, /*Persist*/ 0.10f } },

	// A desktop generator: a 15 cm sphere, 10 cm from its discharge ball.
	{ "Van de Graaff",
	  { /*Machine*/ 2, /*Supply*/ 1, /*Voltage*/ 0.5f, /*Imp*/ 0.5f, /*Branch*/ 0.5f, /*Memory*/ 0.5f, /*Reach*/ 0.5f,
	    /*Spread*/ 0.3750f, /*RodLen*/ 0.4286f, /*Rise*/ 0.5943f, /*Wind*/ 0.5f, /*BPS*/ 0.6078f, /*Top*/ 0.3103f,
	    /*Target*/ 1, /*Belt*/ 0.5886f, /*Sphere*/ 0.4000f, /*Gap*/ 0.7686f, /*Finish*/ 1.0f, /*Globe*/ 0.7500f, /*Finger*/ 0,
	    /*Origin*/ 0, /*Eff*/ 0.5f, /*Glow*/ 0.50f, /*Gas*/ 0, /*Shutter*/ 1.0f, /*Persist*/ 0.40f } },

	// A 40 cm neon-xenon globe on a small flyback driver.
	{ "Plasma Globe",
	  { /*Machine*/ 3, /*Supply*/ 2, /*Voltage*/ 0.5f, /*Imp*/ 0.5f, /*Branch*/ 0.5f, /*Memory*/ 0.5f, /*Reach*/ 0.5f,
	    /*Spread*/ 0.3750f, /*RodLen*/ 0.4286f, /*Rise*/ 0.5943f, /*Wind*/ 0.5f, /*BPS*/ 0.6078f, /*Top*/ 0.3103f,
	    /*Target*/ 1, /*Belt*/ 0.5886f, /*Sphere*/ 0.4000f, /*Gap*/ 0.7686f, /*Finish*/ 1.0f, /*Globe*/ 0.7500f, /*Finger*/ 0,
	    /*Origin*/ 0, /*Eff*/ 0.5f, /*Glow*/ 0.55f, /*Gas*/ 3, /*Shutter*/ 1.0f, /*Persist*/ 0.30f } },

	// A figure grown from a point in a charged slab, on a ZVS supply.
	{ "Lichtenberg",
	  { /*Machine*/ 4, /*Supply*/ 0, /*Voltage*/ 0.5f, /*Imp*/ 0.5f, /*Branch*/ 0.5f, /*Memory*/ 0.5f, /*Reach*/ 0.5f,
	    /*Spread*/ 0.3750f, /*RodLen*/ 0.4286f, /*Rise*/ 0.5943f, /*Wind*/ 0.5f, /*BPS*/ 0.6078f, /*Top*/ 0.3103f,
	    /*Target*/ 1, /*Belt*/ 0.5886f, /*Sphere*/ 0.4000f, /*Gap*/ 0.7686f, /*Finish*/ 1.0f, /*Globe*/ 0.7500f, /*Finger*/ 0,
	    /*Origin*/ 0, /*Eff*/ 0.5f, /*Glow*/ 0.40f, /*Gas*/ 0, /*Shutter*/ 1.0f, /*Persist*/ 0.30f } },
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );
} // namespace flyback::presets
