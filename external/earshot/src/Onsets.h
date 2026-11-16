#pragma once

#include "Fft.h"

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <complex>
#include <cstdint>
#include <vector>

/**
	Band envelopes and onsets, from a short-time spectrum on a fixed AUDIO hop.

	The hop is the point. Every onset detector in the fleet runs on the video
	frame -- the host hands over one spectrum per frame -- and pattern measured
	what that costs: the same groove read 124.84 BPM at 60 fps and 62.06 at
	30, because a 30 fps host quantises every onset to 33 ms. With samples of
	its own, earshot steps a 21 ms Hann window by a quarter (1024 / 256 at 44.1
	and 48 kHz, scaled by powers of two above), so an onset's time and
	everything downstream are the same at 24 fps and 144, at any block size.
	Onsets carry their audio time.

	## Per hop, per band

	- **Level**: Parseval over the band's bins with the window's energy
	  correction, as the amplitude of the equivalent sine (a sine of amplitude
	  A well inside the band reads A).
	- **Flux**: half-wave-rectified rise of log(1 + g |X|), the knee at
	  -110 dB per bin, each bin measured against the largest of its own and
	  its neighbours' previous values (SuperFlux's maximum filter, Boeck &
	  Widmer, DAFx 2013), summed over the band. Log compression is what lets
	  a soft attack register at all (Boeck et al., DAFx 2012); the knee sits
	  below any real signal so the measure is level-independent -- at -70 dB
	  white noise fading in crossed the knee and the rising flux fired a
	  dozen onsets; the maximum filter keeps noise-only bins, whose log
	  magnitude dips and climbs at random, from looking like onsets.
	- **Threshold**: the larger of an absolute floor (1.0) and the flux's
	  running mean plus onsetDeviations (12) running mean absolute
	  deviations. pattern's rule, three times the running mean, fired eight
	  times a second on a held A major triad: steady music's flux is
	  heavy-tailed (beating between unresolved partials, noise bins), and
	  only a deviation-based threshold this high kept four 12-second steady
	  mixes quiet. Fires on the hop where the flux CROSSES the threshold from
	  below -- one onset per excursion, however many hops the window smears an
	  attack over -- after the refractory period, and only if the gate allows.
	- **Masking gate** (pattern's lesson). The first hop that sees an attack
	  sees it through a few milliseconds of window, so even a tone's body is
	  spread wide, and log flux makes splatter 60 dB down on a silent band
	  look like a landslide. A band may fire only if the energy that moved
	  UP in it this hop (bin by bin) is within onsetMaskDb (30) of the
	  largest such rise of any band. A click that really carries energy
	  into a band (a 1 ms attack reaches about 2 kHz) still lights it; a
	  clap, which is broadband, lights everything it reaches. And nothing
	  fires on a rise below onsetGateDb (-80 dBFS): the first hop can catch
	  an attack's first half millisecond, a -100 dB sliver as wide as the
	  band plan, which lit the 4-16 kHz band for a 90 Hz note at 44.1 kHz.

	## After a (re)start

	A detector that starts from zero reads every bin as risen from silence on
	its first frame, and if its floor then snaps to that flux it is deaf
	until the floor decays: the fleet's deaf-for-1.5-s bug. pattern v0.1.1
	fixed it by seeding the floor with an eighth of the band's level; that
	is calibrated for linear magnitude and is ten times too high for log flux
	(the first kick at 250 ms went unheard). Here the first hop primes (the
	previous spectrum is set to the current one), the next 50 ms only
	measure, and the floor is the plain average of the flux seen so far until
	a second of hops has passed, the 1 s exponential after that: deaf for
	50 ms, not 1.5 s, and calibrated from the signal itself.

	**Latency**: an onset is reported at the end of the first hop whose window
	sees it, within one window (21 ms) of the attack; the plugin sees it at
	its next update. **Thread**: render thread, inside Engine::update, on the
	audio in sequence. Allocates only in init().
*/
namespace earshot
{

class TempoTracker;

class BandAnalyser
{
public:
	bool init( int maxRate ) noexcept;

	/// A new stream or a gap: the next hop primes.
	void reset() noexcept;
	/// New sample rate: window and hop.
	void setRate( int fs ) noexcept;
	/// Band edges and timings. No allocation; keeps every band's history when
	/// the edges are unchanged, re-primes only the bands whose edges moved.
	void configure( const Config& config ) noexcept;

	/// Mono samples in sequence. `firstSample` is the absolute index of mono[0].
	void push( const float* mono, int n, uint64_t firstSample, const Config& config,
			   const testing::Mutations& mut, TempoTracker* tempo ) noexcept;

	/// Copy results into the frame's bands (and clear the per-frame onset flags).
	void publish( Band* out, int& count ) noexcept;

	int hopSize() const noexcept { return hop_; }
	int windowSize() const noexcept { return size_; }

private:
	void hop( uint64_t endSample, const Config& config, const testing::Mutations& mut, TempoTracker* tempo ) noexcept;
	void assignBins() noexcept;

	struct State
	{
		float  loHz = 0, hiHz = 0;
		int    binLo = 0, binHi = 0;   ///< [binLo, binHi)
		double meanFlux = 0, devFlux = 0, prevFlux = 0, prevEnergy = 0;
		double level = 0, envelope = 0;
		double lastFire = -1e9;
		bool   primed = false;
		bool   above  = false;   ///< flux was over the threshold on the previous hop
		int    hops   = 0;       ///< hops averaged into meanFlux since the prime (capped)
		int    quiet  = 0;       ///< hops left that may not fire
		bool   onset  = false;
		uint32_t count = 0;
		double strength = 0, lastTime = -1;
	};

	int    fs_ = 0, size_ = 0, hop_ = 0;
	int    bandCount_ = 0;
	State  bands_[ kMaxBands ];
	int    regLo_ = 0, regHi_ = 0;     ///< tempo registers: [1, regLo), [regLo, regHi), [regHi, N/2)
	bool   havePrev_ = false;

	Fft                                  fft_;
	std::vector< float >                 window_;
	std::vector< float >                 input_;     ///< circular, size_ samples
	std::vector< std::complex< float > > spec_;
	std::vector< float >                 logMag_, prevLogMag_;
	std::vector< double >                power_, prevPower_;
	std::vector< float >                 binFlux_;
	uint64_t                             filled_ = 0;    ///< samples since reset
	int                                  writeAt_ = 0;
	int                                  sinceHop_ = 0;
	double                               windowEnergy_ = 1.0, windowSum_ = 1.0;
};

} // namespace earshot
