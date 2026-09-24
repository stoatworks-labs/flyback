#pragma once

#include "engine/Engine.h"
#include "engine/Physics.h"
#include "render/Renderer.h"

#include <string>

/**
    Host parameters, and what they mean.

    **Every ranged parameter is a plain 0..1 float.** `SetParamInfo` clamps an
    `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` can widen it,
    so a parameter declared in kilovolts could not declare a default in
    kilovolts. The physics lives here, in the conversions, and the host shows
    the physical value through `Display()`.

    ## Relative controls

    Five machines share one panel, and a Tesla coil's natural numbers are not
    a plasma globe's. So the shared controls are **relative to each machine's
    own nominal**, and the middle of every one of them is that machine as
    built:

    - Voltage and Source Impedance trim the chosen Supply's nominal V_oc and
      R_s (x0.5..x2 and x0.25..x4).
    - Branching trims the machine's nominal eta (x0.5..x2): a ladder's arc is
      eta 4, a Van de Graaff's spark 3.5, a globe's filaments 2, a Tesla
      coil's streamers 1.6, a Lichtenberg figure 1.
    - Detail, Channel Memory and Reach likewise (x0.5..x2, x0.25..x4, x0.25..x4).
    - Efficiency trims the luminous efficiency, 1% nominal (x0.1..x10).

    So switching the Machine dropdown on its own gives each machine looking like
    itself, and the displayed value always says the physical number.
*/
namespace flyback
{
enum ParamId : unsigned int
{
	// Machine
	PT_PRESET = 0,
	PT_MACHINE,
	PT_FIRE,
	PT_SEED,

	// Supply
	PT_SUPPLY,
	PT_VOLTAGE,
	PT_IMPEDANCE,

	// Discharge
	PT_BRANCHING,
	PT_DETAIL,
	PT_MEMORY,
	PT_REACH,

	// Jacob's Ladder
	PT_ROD_SPREAD,
	PT_ROD_LENGTH,
	PT_RISE,
	PT_WIND,

	// Tesla Coil
	PT_BPS,
	PT_TOPLOAD,
	PT_TARGET,
	PT_TARGET_X,
	PT_TARGET_Y,

	// Van de Graaff
	PT_BELT,
	PT_SPHERE,
	PT_GAP,
	PT_FINISH,

	// Plasma Globe
	PT_GLOBE,
	PT_FINGER,
	PT_FINGER_X,
	PT_FINGER_Y,

	// Lichtenberg
	PT_ORIGIN,

	// Audio
	PT_AUDIO,
	PT_AUDIO_FIRES,
	PT_AUDIO_DRIVE,

	// Light
	PT_EFFICIENCY,
	PT_GLOW,
	PT_GAS,
	PT_SHUTTER,
	PT_PERSISTENCE,
	PT_APPARATUS,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,

	// Over
	PT_DETECT,
	PT_THRESHOLD,
	PT_ILLUMINATION,
	PT_MIX,

	// Layout
	PT_POS_X,
	PT_POS_Y,
	PT_SCALE,
	PT_ROTATION,

	// The Stoatworks About block: one text line, then a button per link.
	// Generated: a guide, a page, the source and support, so four buttons;
	// Flyback.cpp static_asserts
	// the run against StoatworksAbout.h.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,
	PT_COUNT
};

/// Bins in the host's FFT buffer.
constexpr int kAudioBins = 64;

/// Each machine as built: the middle of every relative control.
struct Nominal
{
	double eta;
	int cells;
	double memory;///< seconds
	double camera;///< display units per J per frame-height^2
	physics::SupplyKind supply;
};
Nominal NominalFor( Machine machine );

/// Everything the params say, in physical units: what the engine runs on and
/// what the renderer draws with. `level` is the audio level (0..1), `aspect`
/// the output's.
struct Resolved
{
	Settings engine;
	Renderer::Look look;
	float persistenceSeconds = 0.0f;
	int detect               = 0;
	float threshold          = 0.5f;
	float audioFires         = 0.0f;
};
Resolved Resolve( const float* params, double level, double aspect );

/// The option index a 0..1-or-index float names.
int Option( float value, int count );

/// A parameter's value as the host should show it, or empty for "no unit".
std::string Display( unsigned int index, const float* params );

// The individual conversions, exposed for the harness so a check can say
// "120 BPS" rather than "0.6104".
double BpsFromParam( float v );
float ParamFromBps( double bps );
double BeltFromParam( float v );
float ParamFromBelt( double amps );
double RelativeFromParam( float v, double range );///< range^(2v-1): x1/range .. x range
float ParamFromRelative( double factor, double range );
double ShutterFromParam( float v );///< fraction of the frame, 0..1
double PersistenceFromParam( float v );///< seconds, 0 = none
double TopFromParam( float v );
double SphereFromParam( float v );
double GapFromParam( float v );
double FinishFromParam( float v );///< Peek's m_v: 0.82 at 0, 1 (polished) at 1
double GlobeFromParam( float v );
double SpreadFromParam( float v );
double RodLengthFromParam( float v );
double RiseFromParam( float v );
float ParamFromRise( double metresPerSecond );
double WindFromParam( float v );
} // namespace flyback
