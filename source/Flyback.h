#pragma once

#include "Audio.h"
#include "Controls.h"
#include "Presets.h"
#include "engine/Engine.h"
#include "render/Renderer.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
    Flyback: high-voltage discharges, grown by the dielectric breakdown model.

    Both plugins are this class. The source (`SW Flyback`, HV01) draws the
    machine over its own background; the effect (`SW Flyback Over`, HV02) draws
    it over the clip, and treats the clip's bright parts as ground so the
    discharge strikes into the picture. They differ by a constructor flag and
    their input count, which is little enough that one class stops them
    drifting apart. (downpour's shape.)

    The work is in three places, none of them here:

    - `engine/` -- the machines, the breakdown model, the Laplace solve. No GL.
    - `render/` -- the light, deposited and conserved, and the picture.
    - `Controls.*` -- 0..1 to physics, and back for the display.

    This class talks to the host: parameters, presets, the clock, audio, and
    the clip's ground mask a frame late.
*/
namespace flyback
{
class FlybackPlugin : public CFFGLPlugin
{
public:
	explicit FlybackPlugin( bool overInput );

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

	/// Load-bearing, and its absence is invisible offline: instantiateGL
	/// pushes every declared default back through the setters and deletes the
	/// instance if one fails, and CFFGLPlugin's stub fails. The About line is
	/// a text parameter.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	/// The value a parameter has AFTER the active preset is laid over it.
	float Effective( unsigned int index ) const;

	//-------------------------------------------------------------------
	// For the harness.
	//-------------------------------------------------------------------

	/// The harness declares its clock unit (seconds) rather than letting the
	/// plugin vote on it against the wall clock: it renders hundreds of frames
	/// a second, which the vote would read as milliseconds.
	void SetClockScaleForTest( double scale );
	Engine& EngineForTest()
	{
		return engine;
	}
	Renderer& RendererForTest()
	{
		return renderer;
	}
	const Frame& LastFrameForTest() const
	{
		return frame;
	}
	/// Columns the presets cover, in presets::Param order.
	static const unsigned int* PresetColumnsForTest( int& count );
	/// Whether the onset detector fired on the last frame.
	bool OnsetForTest() const
	{
		return analyser.Fired();
	}
	audio::Analyser& AnalyserForTest()
	{
		return analyser;
	}

private:
	void UpdateClock();
	void Declare();

	const bool overInput;
	float params[ PT_COUNT ] = {};
	bool fireHeld            = false;
	bool firePending         = false;

	Engine engine;
	Renderer renderer;
	Frame frame;
	audio::Analyser analyser;
	bool rendererReady = false;

	std::vector< uint8_t > groundMask;
	uint64_t groundStamp = 0;
	bool lastWasOver     = false;

	// Time: Resolume has sent both seconds and milliseconds through SetTime,
	// and nothing in the call says which. Voted on against the wall clock.
	bool hostTimeSeen   = false;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;

	std::string display;
};
} // namespace flyback
