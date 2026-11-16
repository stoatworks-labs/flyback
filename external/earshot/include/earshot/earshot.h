#pragma once

/**
	earshot -- a shared live-audio front end for Stoatworks FFGL plugins.

	FFGL gives a plugin one audio input, `FF_USAGE_FFT`: 64 magnitude bins of a
	mono sum, no phase, no channels, no time domain, with a bin law nobody has
	measured. earshot opens its own capture device (miniaudio, lifted from
	spasis) and turns it into the things the fleet's audio-reactive plugins
	actually need: a pitch with a confidence, the strongest partials, onsets
	per band that are primed on the first frame, band envelopes, an ISO 266
	third-octave bank, a tempo, a stereo pair, and a contiguous waveform tap.
	With no device chosen it falls back to Resolume's FFT for band envelopes and
	onsets only, with the bin law exposed as an assumption.

	## The calls a plugin makes

	    earshot::Engine engine{ config };                    // a member; allocates everything once
	    earshot::listDevices();                              // constructor / Rescan: fill a menu
	    engine.selectDevice( "MacBook Pro Microphone" );     // on a parameter change; returns at once
	    const earshot::Frame& f = engine.update( hostSeconds, fftBins, 64 );   // once per ProcessOpenGL
	    engine.readTap( left, right, max );                  // woofer: the waveform since last frame

	## Threads

	Three threads touch an Engine, and the rules are strict:

	- **The audio thread** (miniaudio's, or whoever calls `feed`) writes the
	  capture ring. It never blocks, never allocates, never takes a lock.
	- **earshot's device worker** opens and closes devices. Opening a CoreAudio
	  device can take long enough that a host drops parameter writes issued
	  meanwhile (measured in spasis, which opened on the render thread), so it
	  never happens on the caller's thread. The worker starts on the first
	  `selectDevice` with a name, not at construction: a host scanning plugins
	  constructs instances it never renders.
	- **The render thread** calls `update`, `readTap`, `frame` and
	  `peekWaveform`. All analysis runs inside `update`, on audio that is already
	  in the ring; results land in the Frame, which the same thread then reads, so
	  reading results needs no lock. Nothing on this path allocates after the
	  constructor.

	`selectDevice`, `configure`, `captureState` and `status` may be called from
	any thread except the audio thread. A plugin that reads the Frame from a
	thread other than the one calling `update` must copy it under its own lock.

	## No exceptions, no global state

	Every call is `noexcept`. An allocation failure in the constructor leaves
	`ok()` false and every call a harmless no-op. There is no static state: two
	Engines (two plugin instances) share nothing, and each opens its own stream.

	Credit: the capture, the ring and the Silent state are spasis's
	(github.com/stoatworks-labs/spasis); the onset priming rule and the tempo
	detector are pattern's v0.1.1 (github.com/stoatworks-labs/pattern). See
	ATTRIBUTIONS.md.
*/

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace earshot
{

constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

constexpr int kMaxChannels   = 8;      ///< channels a device stream may carry into the ring
constexpr int kMaxBands      = 8;      ///< envelope / onset bands
constexpr int kMaxThirds     = 31;     ///< ISO 266 thirds, 20 Hz .. 20 kHz
constexpr int kMaxPartials   = 8;
constexpr int kMinSampleRate = 22050;
constexpr int kMaxSampleRate = 192000; ///< buffers are sized for this once, at construction

/// The name `selectDevice` takes for the system default input.
constexpr const char* kDefaultDevice = "(default)";

//---------------------------------------------------------------------------
// Capture
//---------------------------------------------------------------------------

/**
	What the capture side is doing, and what to tell the operator.

	FFGL gives a plugin no text and no window, so these are for the plugin's own
	diagnostics log and for whatever non-text signal it can draw (spasis
	brightens its graticule). docs/INTEGRATING.md has the wording.
*/
enum class CaptureState : int
{
	Closed,   ///< no device chosen: the host FFT drives band envelopes and onsets
	Opening,  ///< the worker is opening the device; the host FFT stands in
	Running,  ///< samples are arriving and at least one is not zero
	Silent,   ///< open and running, and bit-exactly zero for 3 s: no permission,
	          ///< an unpatched Dante receiver, a loopback nothing is routed to
	Missing,  ///< the named device is not connected; the worker retries every 2 s
	Failed,   ///< the backend refused it; see Status::error
};

const char* toString( CaptureState state ) noexcept;

/// Where this frame's analysis came from.
enum class Source : int
{
	None,    ///< nothing to analyse yet
	Device,  ///< earshot's own capture (or `feed`)
	Host,    ///< Resolume's FFT buffer: band envelopes and onsets only, uncalibrated
};

struct DeviceInfo
{
	std::string name;      ///< what the operator sees, and what gets saved
	int         channels  = 0;
	bool        isDefault = false;
};

/// Capture devices present now. Talks to the OS: call it from the plugin
/// constructor or a Rescan event, never per frame. Never opens a stream, so it
/// does not trigger a microphone-permission prompt.
std::vector< DeviceInfo > listDevices() noexcept;

/**
	A stable key for a device name, for an FFGL option element's value.

	An option parameter saves its element's value in the composition. If the
	value is the element's position, a composition saved against "Dante 1-2"
	reopens on whatever is third in the list tomorrow -- spasis's rule is that a
	device is remembered by name, never by index. This is FNV-1a folded to 23
	bits, so it is exactly representable in a float and survives the host's
	round trip. See docs/INTEGRATING.md.
*/
float deviceKey( const char* name ) noexcept;

//---------------------------------------------------------------------------
// The host's FFT, and what is assumed about it
//---------------------------------------------------------------------------

/**
	Nobody in the fleet has measured Resolume's 64 bins: magnitude or power,
	linear or log in frequency, what sample rate, what full scale. So the
	fallback path takes the law as an assumption the plugin exposes (needle's
	`Bin Law`, pattern's `Bin Law` and `Bin Value`), and nothing it reports is
	calibrated: `Frame::calibrated` is false and levels are relative.
*/
struct HostBins
{
	enum class Value : int { Magnitude, Power };
	enum class Layout : int { Linear, Log };

	Value  value      = Value::Magnitude;
	Layout layout     = Layout::Linear;   ///< Linear: bin j covers [j, j+1)/N of Nyquist
	double sampleRate = 44100.0;          ///< the host never says; 44.1 kHz like pattern
};

//---------------------------------------------------------------------------
// Configuration
//---------------------------------------------------------------------------

struct Config
{
	//-- Read once, at construction -----------------------------------------

	/// Channels taken from the device: the FIRST n, never a downmix (a 64-channel
	/// Dante feed mixed to two is a different signal that looks plausible).
	int   maxChannels = 2;
	/// Ring history, sized for kMaxSampleRate. At 48 kHz this holds 4x longer.
	float ringSeconds = 1.0f;

	//-- May change at any time; never clears the ring ----------------------

	/// The stereo pair is channels pairFirst and pairFirst + 1 (clamped to what
	/// the device has; a mono device duplicates its one channel).
	int   pairFirst = 0;

	/// Pitch range. The window is three periods of pitchMinHz (75 ms at 40 Hz);
	/// pitchMinHz below 40 is clamped, because the buffers are sized for it.
	float pitchMinHz   = 40.0f;
	float pitchMaxHz   = 2000.0f;
	float pitchClarity = 0.80f;   ///< NSDF peak needed to call it voiced
	float pitchGateDb  = -60.0f;  ///< window level (dBFS, sine) below which nothing is voiced

	/// Envelope / onset bands: bandCount bands between bandEdges[0..bandCount].
	int   bandCount = 4;
	float bandEdges[ kMaxBands + 1 ] = { 30.0f, 150.0f, 600.0f, 4000.0f, 16000.0f, 0, 0, 0, 0 };

	float onsetDeviations = 12.0f;   ///< an onset's flux is this many mean absolute deviations over
	                                 ///< the flux's running mean. Steady music's flux is heavy-tailed,
	                                 ///< hence a figure that would be absurd for Gaussian noise (AGENTS.md)
	float onsetMaskDb     = 30.0f;   ///< a band whose energy rise is this far below the
	                                 ///< strongest band's in the same hop cannot fire
	float onsetGateDb     = -80.0f;  ///< a band cannot fire on an energy rise smaller than a sine this loud
	float onsetRefractory = 0.05f;   ///< seconds, per band
	float envelopeAttack  = 0.005f;  ///< seconds
	float envelopeRelease = 0.150f;  ///< seconds

	/// Third-octave display ballistics (needle's peak hold and fall).
	float thirdHoldSeconds = 1.0f;
	float thirdFallDbPerSecond = 20.0f;

	/// The waveform tap's fixed output rate, whatever the device runs at.
	int   tapRate = 8000;

	HostBins hostBins;
	/// Use the host FFT while a chosen device is Opening, Missing or Failed. Not
	/// while Silent: a Silent device is open, and the operator should see that
	/// it is dead rather than have the host FFT paper over it.
	bool  fallbackWhenDeviceDown = true;
};

//---------------------------------------------------------------------------
// One frame's results
//---------------------------------------------------------------------------

/**
	Detected pitch: McLeod's normalised square difference function (MPM) on the
	mono sum of the stereo pair. `confidence` is the NSDF peak, which for a sine
	in white noise is S/(S+N) by construction. Octave errors are handled the
	way MPM does it: the first key maximum within 0.9 of the highest is taken,
	so a period's double (which scores as high) never wins over the period.
*/
struct Pitch
{
	bool  valid      = false;   ///< analysis ran this frame (false on the host fallback)
	bool  voiced     = false;   ///< confidence >= pitchClarity and level above the gate
	float hz         = 0.0f;
	float midi       = 0.0f;    ///< 69 = A4 = 440 Hz, fractional
	float confidence = 0.0f;    ///< 0..1
	float levelDb    = -200.0f; ///< window level, dB re a full-scale sine
};

struct Partial
{
	float hz        = 0.0f;
	float amplitude = 0.0f;   ///< amplitude of the sine, 1 = full scale
	float db        = -200.0f;
	int   harmonic  = 0;      ///< nearest multiple of the detected pitch, 0 if none within 30 cents
};

struct Band
{
	float  loHz = 0.0f, hiHz = 0.0f;
	float  level    = 0.0f;     ///< band amplitude as an equivalent sine, 1 = full scale
	float  db       = -200.0f;
	float  envelope = 0.0f;     ///< level through the attack / release one-pole
	bool   onset    = false;    ///< at least one onset since the previous update
	uint32_t onsetCount   = 0;  ///< monotonic: compare with the last value you saw
	float  onsetStrength  = 0.0f; ///< flux over threshold of the latest onset
	double lastOnsetTime  = -1.0; ///< audio seconds (Frame::audioTime's clock)
};

struct Third
{
	float nominalHz = 0.0f;   ///< ISO 266 R10 label: 20, 25, 31.5 ...
	float exactHz   = 0.0f;   ///< IEC 61260-1 base-ten: 1000 * 10^(x/10)
	float loHz = 0.0f, hiHz = 0.0f;
	float db     = -200.0f;   ///< band level, time-weighted Fast (125 ms), dB re a full-scale sine
	float holdDb = -200.0f;   ///< peak hold, falling at thirdFallDbPerSecond after thirdHoldSeconds
};

struct Tempo
{
	bool  valid      = false;
	bool  settled    = false;  ///< two readings in a row agreed within 2 %
	float bpm        = 0.0f;
	float confidence = 0.0f;   ///< normalised autocorrelation at the chosen lag
};

struct Stereo
{
	bool  valid = false;
	bool  mono  = false;       ///< the device has one channel; both sides are it
	float rmsL = 0.0f, rmsR = 0.0f;
	float peakL = 0.0f, peakR = 0.0f;
	float correlation = 0.0f;  ///< -1..1, 0 when either side is silent
	float width       = 0.0f;  ///< side energy fraction: 0 mono, 0.5 uncorrelated, 1 anti-phase
	float balanceDb   = 0.0f;  ///< 20 log10( rmsL / rmsR ), clamped to +-60
};

struct Frame
{
	Source       source     = Source::None;
	CaptureState capture    = CaptureState::Closed;
	bool         calibrated = false;  ///< false on the host fallback
	double       audioTime  = 0.0;    ///< seconds of audio analysed since the stream began
	int          sampleRate = 0;
	uint32_t     generation = 0;      ///< bumps whenever the stream restarts

	Pitch   pitch;
	Partial partials[ kMaxPartials ];
	int     partialCount = 0;
	Band    bands[ kMaxBands ];
	int     bandCount = 0;
	Third   thirds[ kMaxThirds ];
	int     thirdCount = 0;
	Tempo   tempo;
	Stereo  stereo;

	int     tapAvailable = 0;  ///< samples readTap will hand out now
	uint64_t tapDropped  = 0;  ///< tap samples lost because nobody read them in time (since the stream began)
	uint64_t gapFrames   = 0;  ///< audio skipped because the render thread fell behind
};

/// Fixed-size so `status` never allocates.
struct Status
{
	CaptureState state = CaptureState::Closed;
	char   requested[ 128 ] = {};   ///< what selectDevice asked for
	char   opened[ 128 ]    = {};   ///< what the backend actually opened
	char   error[ 160 ]     = {};   ///< empty unless Missing or Failed
	int    sampleRate     = 0;
	int    nativeChannels = 0;
	int    channels       = 0;      ///< taken into the ring
	double silentSeconds  = 0.0;
};

namespace testing
{
struct Mutations;
class CaptureBackend;
}

//---------------------------------------------------------------------------
// The engine
//---------------------------------------------------------------------------

class Engine
{
public:
	explicit Engine( const Config& config = Config() ) noexcept;
	/// For the harness: a capture backend other than miniaudio. Takes ownership.
	Engine( const Config& config, std::unique_ptr< testing::CaptureBackend > backend ) noexcept;
	~Engine();

	Engine( const Engine& )            = delete;
	Engine& operator=( const Engine& ) = delete;

	bool ok() const noexcept;

	//-- Device (any thread but the audio thread; returns at once) ----------

	/// nullptr or "" closes the device (host fallback). kDefaultDevice opens the
	/// system default. Anything else is a device NAME, matched exactly.
	void selectDevice( const char* name ) noexcept;
	CaptureState captureState() const noexcept;
	void status( Status& out ) const noexcept;

	//-- Configuration -------------------------------------------------------

	/// Runtime fields only (see Config). Never clears the ring, never allocates.
	void configure( const Config& config ) noexcept;
	const Config& config() const noexcept;

	//-- Per frame (render thread) -------------------------------------------

	/// Analyse everything that arrived since the last call. `hostSeconds` is
	/// the host's clock (used only on the fallback path, as a difference);
	/// `hostFft` may be null. Never allocates, never blocks.
	const Frame& update( double hostSeconds, const float* hostFft, int hostFftCount ) noexcept;
	const Frame& frame() const noexcept;

	/// The stereo pair at Config::tapRate, contiguous: every sample since the
	/// previous call exactly once. Returns how many were written (<= max). Call
	/// after update(); samples not read stay queued (65536 of them, 8.2 s at
	/// 8 kHz; older ones are dropped and counted in Frame::tapDropped).
	int readTap( float* left, float* right, int maxSamples ) noexcept;
	int tapRate() const noexcept;

	/// The newest `samples` of the stereo pair at the device rate, as analysed
	/// by the last update (oscilloscopes). False if there is not that much yet.
	bool peekWaveform( float* left, float* right, int samples ) const noexcept;

	//-- Feeding audio without a device ---------------------------------------

	/// Act as if a device opened at this rate with this many channels. Closes
	/// any real device first (and waits for that). For the harness, for file
	/// playback, or for a plugin with its own audio source.
	void feedBegin( int sampleRate, int channels ) noexcept;
	/// The audio thread's call: interleaved float frames. One thread only.
	void feed( const float* interleaved, int frames ) noexcept;
	void feedEnd() noexcept;

	//-- Harness only ----------------------------------------------------------

	testing::Mutations& mutations() noexcept;

private:
	struct Impl;
	std::unique_ptr< Impl > impl_;
};

} // namespace earshot
