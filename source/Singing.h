#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if FLYBACK_SINGING
namespace earshot
{
class Engine;
namespace testing
{
class CaptureBackend;
}
} // namespace earshot
#endif

/**
    The singing Tesla coil: a live pitch, and the interrupter rate it sets.

    A musical coil's interrupter fires once per period of the note, so the
    spark's crack IS the note (Kaizer's musical interrupter, the UD3 and
    Syntherrupter all work this way: SPEC-flyback-singing.md). Here the note
    is detected from live audio by earshot (MPM pitch with an NSDF clarity,
    earshot's own capture device, never Resolume's FFT, which has no pitch),
    and the Tesla machine's interrupter follows it instead of the BPS control.

    ## The law (decided from the eighteen Resolume demo WAVs: AGENTS.md)

    - A voiced frame (earshot: NSDF clarity >= 0.80, window level >= Gate,
      40..2000 Hz) sets the note.
    - The coil sounds while it holds a note and the window level is at or
      above the Gate. Unvoiced frames above the Gate SUSTAIN the last note:
      the six demo beats are voiced on 2.9 to 19.1% of their frames, so a
      "voiced frames only" coil was dark for 81 to 97% of each, lit in mean
      runs of 1.2 to 3.7 frames (`hvtest --demo-music`).
    - Below the Gate is a rest: the coil is silent and FORGETS the note, so the
      next one waits for its own pitch instead of starting on the old one.
      A new stream (another device, a restart) forgets it too.
    - The rate is the note x 2^Octave, folded by whole octaves into the BPS
      control's own 5..1000 bangs a second. The top of that range is the
      machine's duty limit (a 100 us bang at 1000 BPS is 10%, the figure the
      musical-interrupter designs give); folding keeps the note's pitch class.

    In MIDI terms: note-on is the first voiced frame after a rest, a later
    voiced frame is a legato change of note (the interrupter's phase runs on),
    note-off is the level falling below the Gate.

    ## CoreAudio: never, until asked

    Nothing here exists until Pitch Source is first set to something other than
    Off: no earshot engine, no device list, no selectDevice. `listDevices` and
    `selectDevice` both start CoreAudio (earshot's own finding), so with the
    mode off the plugin never calls either, and the menu holds only Off, System
    default and spares until the mode is first switched on, when the devices
    are listed and the menu refilled. `hvtest --off` counts it, and verify.sh
    runs the mode-off checks and oxbow's selftest under a dylib that traps
    every CoreAudio dlopen and call.

    FLYBACK_NO_DEVICES=1 never lists and never selects, even with the mode on
    (verify.sh's oxbow runs; woofer's and rubens's gate).

    ## Threads

    `Apply` and `Update` are the render thread's (ProcessOpenGL). The plugin's
    SetFloatParameter only stores the value; nothing here runs on a host's
    parameter thread.
*/
namespace flyback::sing
{
/// Menu length, fixed at declaration: Off, System default, then devices,
/// then "(none)" spares.
constexpr int kDeviceElements = 16;

/// Octave: an option, -4 .. +2 octaves, 0 at index 4.
constexpr int kOctaveLow      = -4;
constexpr int kOctaveHigh     = 2;
constexpr int kOctaveElements = kOctaveHigh - kOctaveLow + 1;
constexpr int kOctaveDefault  = -kOctaveLow;///< the index of 0 octaves

/// Gate: -100 .. -20 dBFS (a sine's level), linear in dB; -60 dBFS, earshot's
/// own pitch gate, at the middle.
constexpr double kGateLowDb  = -100.0;
constexpr double kGateHighDb = -20.0;

/// The interrupter's range: BpsFromParam's.
constexpr double kRateLow  = 5.0;
constexpr double kRateHigh = 1000.0;

/// earshot's deviceKey (FNV-1a folded to 23 bits, 0 kept for none).
float KeyFor( const char* name );
double GateFromParam( float v );///< dBFS
float ParamFromGate( double db );
int OctaveFromParam( float v ); ///< octaves, kOctaveLow .. kOctaveHigh
float ParamFromOctave( int octaves );
/// hz x 2^octaves, folded by whole octaves into [kRateLow, kRateHigh]; 0 for 0.
double RateFor( double hz, int octaves );

/// What the lamp shows.
enum class Lamp : int
{
	Off = 0,///< the mode is off: no lamp at all
	Running,///< green: samples arriving
	Silent, ///< red: open, and bit-exact zeros
	Missing,///< blinking red: not connected, or refused
	Waiting ///< amber (blinking while opening): nothing open yet
};

struct Note
{
	bool active      = false;///< Pitch Source is on
	bool voiced      = false;///< this frame's analysis called it a note
	bool sounding    = false;///< the coil fires this frame
	double hz        = 0.0;  ///< the note held (0: none), Hz
	double rate      = 0.0;  ///< bangs per second this frame (0: a rest)
	double frameHz   = 0.0;  ///< this frame's analysis, voiced or not
	double levelDb   = -200.0;
	double confidence = 0.0;
	Lamp lamp        = Lamp::Off;
	bool blinkOn     = true;///< for a blinking lamp, whether it is lit now
	uint32_t generation = 0;
};

struct Slot
{
	std::string label;///< what the menu shows
	std::string name; ///< what selectDevice is given
	float value;      ///< the element's value
};

/// Process-wide counts of every call that could reach CoreAudio, and every
/// engine made. `hvtest --off` requires them all zero with the mode off.
struct Counters
{
	int engines  = 0;///< earshot engines constructed
	int lists    = 0;///< listDevices (or a test backend's enumerate)
	int selects  = 0;///< selectDevice
	int feeds    = 0;///< feedBegin
	int updates  = 0;///< earshot update()
};
Counters CountersForTest();

/// Negative controls (harness only).
enum Hook : int
{
	kHookVoicedOnly   = 1 << 0,///< no sustain: the coil sounds on voiced frames only
	kHookGateOpen     = 1 << 1,///< the Gate never closes: a rest still sounds
	kHookMenuByIndex  = 1 << 2,///< devices valued by position in the menu, not by name
	kHookEagerDevices = 1 << 3,///< list the devices at construction, as eidophone does
	kHookOctaveOff    = 1 << 4,///< Octave is read one octave high
	kHookKeepNote     = 1 << 5 ///< a rest keeps the note: the next sound above the Gate resumes it
};

#if FLYBACK_SINGING
using BackendFactory = std::unique_ptr< earshot::testing::CaptureBackend > ( * )();
#endif

class PitchInput
{
public:
#if FLYBACK_SINGING
	/// Harness: every engine made afterwards uses this backend (a scripted
	/// device) instead of miniaudio, and lists its devices. Null: miniaudio.
	static void SetBackendFactoryForTest( BackendFactory factory );
#endif
	/// Harness: hooks for every PitchInput constructed afterwards (so the
	/// construction-time ones can act).
	static void SetDefaultHooksForTest( int hooks );
	/// Whether this build has live pitch at all (FLYBACK_SINGING).
	static bool Built();

	PitchInput();
	~PitchInput();
	PitchInput( const PitchInput& ) = delete;
	PitchInput& operator=( const PitchInput& ) = delete;

	/// The menu. Off, System default and spares until the mode is first on.
	const std::vector< Slot >& Slots() const
	{
		return slots;
	}

	/// The render thread, once a frame, before Update: the Pitch Source value
	/// the host stored, and whether Rescan was pressed. Acts only when the
	/// value changes. Returns true when the menu changed (the caller refills
	/// the host's option elements).
	bool Apply( float value, bool rescan );

	/// The render thread, once a frame. With the mode off it returns a Note
	/// with active false and touches nothing.
	Note Update( double hostSeconds, int octaves, double gateDb );

	bool Active() const
	{
		return applied > 0.0f;
	}
	/// Whether the engine exists (the mode has been on at least once).
	bool Started() const;

	/// Harness: audio as if from a device (earshot's feed()). Dropped while
	/// the engine does not exist.
	void Feed( const float* interleaved, int frames, int rate, int channels );

	/// Harness: as if made anew (the engine, the stream and the note gone), so
	/// a check can show it would see a pitch input rebuilt where it must not be.
	void ResetForTest();

	/// What the last select asked for ("" = none).
	const std::string& Requested() const
	{
		return requested;
	}

	int hooks = 0;///< Hook bits (harness only)

private:
	void Start();
	void Refresh( bool reselect );
	void Select( float value );

	std::vector< Slot > slots;
#if FLYBACK_SINGING
	std::unique_ptr< earshot::Engine > engine;
#endif
	std::map< float, std::string > seen;///< every key met in this process, and its name
	float applied = 0.0f;
	std::string requested;
	bool feeding         = false;
	double appliedGate   = 1e9;
	double held          = 0.0;///< the note held, Hz
	uint32_t generation  = 0;
	int lastState        = -1;
};
} // namespace flyback::sing
