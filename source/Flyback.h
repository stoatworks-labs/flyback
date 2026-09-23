#pragma once

#include "Audio.h"
#include "Controls.h"
#include "Presets.h"
#include "engine/Engine.h"
#include "render/Renderer.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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

	~FlybackPlugin() override;

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
	/// The engine, idle: waits for any frame the worker is computing.
	Engine& EngineForTest()
	{
		Wait();
		return engine;
	}

	/// Run the engine inline, on the calling thread, in the frame it is for --
	/// no worker, no frame of latency. The harness's physics-to-pixels checks
	/// need the frame they rendered to be the one the engine just stepped.
	void SetSynchronousForTest( bool on )
	{
		Wait();
		synchronous = on;
	}
	Renderer& RendererForTest()
	{
		return renderer;
	}
	/// The engine's own time for its last completed step, ms, without waiting
	/// for the one in flight.
	double LastEngineMillisForTest() const
	{
		return lastEngineMillis.load();
	}

	/// The frame last rendered (in the worker mode, last frame's engine step).
	const Frame& LastFrameForTest() const
	{
		return synchronous ? frame : shown;
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

	//-------------------------------------------------------------------
	// The engine's worker thread.
	//
	// ProcessOpenGL for frame n waits for the engine step it started at
	// frame n-1, renders that, and hands the worker frame n's step. The
	// picture is one frame behind the clock, and the render thread only ever
	// waits for whatever of the engine's time did not overlap the host's own
	// work. The engine sees exactly the calls, in exactly the order, it sees
	// synchronously -- which is why determinism survives the thread.
	//-------------------------------------------------------------------
	struct Job
	{
		Settings settings;
		std::vector< uint8_t > clip;///< a copy: the render thread refills its own next frame
		bool fire     = false;
		double now    = 0.0;
		double period = 1.0 / 60.0;
		Renderer::Look look;
	};
	void RunJob( Job& job );
	void Submit( Job&& job );
	void Wait();
	void StartWorker();
	void StopWorker();
	void WorkerLoop();

	std::thread worker;
	std::mutex jobMutex;
	std::condition_variable jobSignal;
	Job job;
	bool busy        = false;
	bool busyResult  = false;///< a finished step is waiting to be collected
	bool quitting    = false;
	bool synchronous = false;
	Frame shown;
	Renderer::Look shownLook;
	bool haveShown = false;
	std::atomic< double > lastEngineMillis { 0.0 };

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
