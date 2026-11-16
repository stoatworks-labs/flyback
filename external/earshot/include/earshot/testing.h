#pragma once

/**
	The harness's hooks into the engine. A plugin never needs this header.

	`Mutations` are the negative controls: each flag perturbs the model the way
	a known bug would (most of them are bugs the fleet actually shipped), and
	`estest --negative` requires every check that the flag targets to FAIL with
	it set. A check that cannot fail is not a check. None of these is reachable
	from Config.

	`CaptureBackend` lets the harness replace miniaudio with a scripted device:
	one that takes a third of a second to open (to prove the render thread never
	waits for it), one that is missing and then appears, one that dies.
*/

#include "earshot.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace earshot
{
namespace testing
{

struct Mutations
{
	// FFT
	bool fftFloatTwiddles   = false; ///< twiddles by a float running-angle recurrence (spasis measured ~20 dB lost)

	// Pitch
	bool pitchGlobalMax     = false; ///< take the highest NSDF peak, not MPM's first key maximum within 0.9
	bool pitchSpectralPeak  = false; ///< report the strongest FFT peak as the pitch
	bool pitchNoInterp      = false; ///< integer-lag period, no parabolic refinement
	double pitchLagDelay    = 0.0;   ///< analyse a window this many seconds older than the newest

	// Partials
	bool partialsNoInterp   = false; ///< bin-centre frequencies, no quadratic refinement

	// Onsets and envelopes
	bool onsetNoPrime       = false; ///< first hop compares against silence, floor starts at zero
	bool onsetFullSeed      = false; ///< pattern v0.1.0's seed: the floor starts at the band's whole level
	bool onsetSnapZeroDt    = false; ///< host path: a repeated host time snaps the floor (alpha = 1)
	bool onsetNoMask        = false; ///< no masking gate: a hard attack's click lights every band
	bool onsetMaskWinnerOnly = false; ///< the masking gate lets only the band with the largest rise fire
	bool onsetNoRisingEdge  = false; ///< fire on every hop over the threshold, not once per crossing
	bool onsetPlainFlux     = false; ///< each bin against its own previous value (no maximum filter)
	bool onsetFullWave      = false; ///< flux and the masking gate count falls as well as rises
	bool onsetLinearFlux    = false; ///< linear magnitude flux on the audio path (pattern's), not log
	bool bandNoParseval     = false; ///< band level without the window's energy correction
	bool envelopeOneRate    = false; ///< the envelope falls at the attack rate

	// Tempo
	bool tempoLegacy        = false; ///< pattern v0.1.0: raw summed flux, best lag, a half at 70 %
	bool tempoNoOnsetGate   = false; ///< take readings with no onsets in the window
	bool tempoSlowestLevel  = false; ///< the slowest family member within 70 % wins, not the fastest

	// Thirds
	bool thirdOrder2        = false; ///< second-order prototype instead of third
	bool thirdFixedOrder    = false; ///< third order everywhere, even where warping breaks the skirt
	bool thirdBase2         = false; ///< G = 2 instead of 10^(3/10)
	bool thirdNoPrewarp     = false; ///< bilinear transform without pre-warping the band edges
	bool thirdFloatArithmetic = false; ///< run the sections in float (coefficients and state)
	bool thirdSlowWeighting = false; ///< time weighting S (1 s) instead of F (125 ms)
	bool thirdNoHold        = false; ///< the peak hold starts falling at once

	// Stereo
	bool stereoSwap         = false; ///< left and right exchanged
	bool stereoNoNormalise  = false; ///< correlation = sum(LR) / N, not normalised
	bool stereoWidthSideOverMid = false; ///< width = side energy / mid energy, clamped to 1
	bool stereoMonoLeftOnly = false; ///< a one-channel stream metered as left with a silent right

	// Tap
	bool tapPeekLatest      = false; ///< hand out the newest window each call instead of a stream
	bool tapNoAntiAlias     = false; ///< interpolate without the low-pass
	bool tapCutoffQuarter   = false; ///< the anti-alias cutoff at a quarter of the tap rate (a "safe" margin)

	// Capture
	bool silentByLevel      = false; ///< call |x| < 1e-4 silence, not bit-exact zero
	bool silentNever        = false; ///< never report Silent
	bool silentLatched      = false; ///< once Silent, stay Silent until the device is reopened
	bool silentOnPair       = false; ///< judge zeros on the sliced channels, not the raw device buffer
	bool configureClears    = false; ///< configure() clears the ring and re-primes everything
	bool ringNoValidate     = false; ///< trust a copy without re-checking the write cursor
	bool ringNoEpoch        = false; ///< a reader's epoch is not checked: a reset stream's old reads succeed
	bool captureOpenInline  = false; ///< selectDevice() waits for the open (spasis's render-thread open)
	bool captureMissingToDefault = false; ///< a missing device is swapped for the system default
	bool captureLostIgnored = false; ///< a device that stops on its own is not noticed (stays Running)
	bool captureRateFixed   = false; ///< a new stream's sample rate is not followed
	bool captureNoCloseOnDestroy = false; ///< the worker exits without closing the device
	bool allocPerFrame      = false; ///< update() allocates (and frees) a scratch block every frame

	// Fallback
	bool fallbackInventsPitch = false; ///< host path fills pitch from the loudest bin
	bool fallbackOnSilent   = false; ///< host bins stand in for a Silent device too (hiding it)
	bool hostPowerAsMagnitude = false; ///< Power-law bins used as magnitudes (not square-rooted)
};

/// What a backend calls while a device runs. Engine owns it.
class AudioSink
{
public:
	virtual ~AudioSink() = default;
	/// Before the stream starts, while no callback can run.
	virtual void begin( int sampleRate, int nativeChannels ) noexcept = 0;
	/// The audio thread. `interleaved` has nativeChannels channels.
	virtual void deliver( const float* interleaved, uint32_t frames, int nativeChannels ) noexcept = 0;
	/// The device stopped on its own (unplugged). Any thread.
	virtual void lost() noexcept = 0;
};

enum class OpenResult : int
{
	Opened,
	NotConnected,  ///< the named device is not present: Missing, and retried
	Failed,        ///< the backend refused: Failed, and retried less eagerly
};

class CaptureBackend
{
public:
	virtual ~CaptureBackend() = default;
	/// Worker thread. Open `name` (kDefaultDevice for the default), call
	/// sink.begin() before starting, then deliver() from the device thread.
	/// `openedName` and `error` are filled in; both are at least 128 bytes.
	virtual OpenResult open( const char* name, AudioSink& sink, char* openedName, char* error ) noexcept = 0;
	/// Worker thread. Returns once no further callback can run.
	virtual void close() noexcept = 0;
	virtual std::vector< DeviceInfo > enumerate() noexcept = 0;
};

/// miniaudio. Null if the library was built with EARSHOT_CAPTURE=OFF.
std::unique_ptr< CaptureBackend > makeMiniaudioBackend() noexcept;

} // namespace testing
} // namespace earshot
