#pragma once

#include "earshot/earshot.h"
#include "earshot/testing.h"

/**
	The fallback: band envelopes and onsets from Resolume's FFT buffer, when no
	device is chosen (or a chosen one is down and Config allows it).

	## What is assumed, said outright

	The host fills an `FF_USAGE_FFT` buffer once per rendered frame: 64 floats
	whose law nobody in the fleet has measured. So the law is HostBins, an
	assumption the plugin exposes as controls: Magnitude or Power (needle's
	`Bin Law`), Linear or Log in frequency (pattern's `Bin Law`), and the host's
	sample rate, 44.1 kHz when nothing says otherwise. A bin belongs to the
	band its centre frequency falls in under that law. Nothing here is
	calibrated -- levels are relative, `Frame::calibrated` is false -- and
	everything the bins cannot carry (pitch, partials, thirds, tempo, stereo,
	the waveform) stays EMPTY rather than guessed: spasis's rule, a plausible
	guess is worse than an empty display.

	## Onsets on video frames

	Linear magnitude flux per band against an adaptive floor, as pattern does
	on the same bins. Primed on the first frame (previous bins = current,
	floor = an eighth of the band's level). The frame interval comes from the
	host clock as a difference in double; a repeated or backward time steps
	nothing (dt = 0 must not snap the floor: the fleet's bug), and a gap over a
	second re-primes, because flux across a seek is not an onset.

	**Thread**: render thread, inside Engine::update. No allocation.
*/
namespace earshot
{

class HostAnalyser
{
public:
	static constexpr int kMaxBins = 256;

	void reset() noexcept;
	void configure( const Config& config ) noexcept;
	void frame( double hostSeconds, const float* bins, int count, const Config& config,
				const testing::Mutations& mut ) noexcept;
	void publish( Band* out, int& count ) noexcept;

	/// Centre frequency of bin j of `count` under the law.
	static double binHz( int j, int count, const HostBins& law ) noexcept;

	double clock() const noexcept { return now_ - origin_; }
	/// The loudest bin's frequency (for the negative control only).
	double loudestHz() const noexcept { return loudestHz_; }

private:
	struct State
	{
		float  loHz = 0, hiHz = 0;
		double floor = 0, prevFlux = 0, prevEnergy = 0, level = 0, envelope = 0;
		double lastFire = -1e9, strength = 0, lastTime = -1;
		bool   onset = false;
		bool   above = false;   ///< flux was over the threshold on the previous frame
		uint32_t count = 0;
	};

	State  bands_[ kMaxBands ];
	int    bandCount_ = 0;
	double prev_[ kMaxBins ] = {};
	bool   primed_ = false;
	double last_ = 0.0, now_ = 0.0, origin_ = 0.0;
	double loudestHz_ = 0.0;
};

} // namespace earshot
