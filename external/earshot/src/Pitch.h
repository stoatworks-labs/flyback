#pragma once

#include "Fft.h"

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <complex>
#include <vector>

/**
	Pitch: McLeod's Pitch Method (McLeod & Wyvill, "A smarter way to find
	pitch", ICMC 2005), on the mono sum of the stereo pair.

	The normalised square difference function

	    n(tau) = 2 r(tau) / m(tau),
	    r(tau) = sum_j x_j x_{j+tau},   m(tau) = sum_j ( x_j^2 + x_{j+tau}^2 )

	is exactly 1 at a lag where the signal repeats (Cauchy-Schwarz makes it at
	most 1 everywhere), whatever the amplitude and whatever the window's phase.
	For a sine in white noise it is S/(S+N) at the period, which is why it is
	the confidence: `estest --pitch` checks that relation, not a tuned figure.

	**Octave errors.** A periodic signal scores as high at 2T, 3T ... as at T.
	MPM takes the key maxima (the highest point between each positive-going
	zero crossing of n and the next negative-going one) and picks the FIRST
	whose height is within k = 0.9 of the highest. A sub-octave never wins over
	the period it is a multiple of; a missing fundamental is still found,
	because its period is where every harmonic lines up; a strong second
	harmonic scores (a2^2 - a1^2)/(a1^2 + a2^2) at T/2, which stays under 0.9
	until the fundamental is 13 dB below it.

	**Band, in two passes.** The NSDF's noise-times-noise term grows with the
	noise bandwidth: white noise at 10 dB SNR put 17 cents of error on a
	110 Hz sine analysed full band. So pass one runs on the mono sum
	low-passed at 2.5 x pitchMaxHz (fourth-order Butterworth, 5 kHz by
	default), and pass two again with the cut at 8 x the pitch pass one found;
	pass two replaces pass one only if they agree within 50 cents, so it can
	refine but never change the answer. The confidence is therefore S/(S+N)
	with N the noise in the band the answer came from.

	**Window.** Three periods of the lowest pitch (75 ms at 40 Hz): the lag
	search needs at least two periods of overlap at the longest lag. The
	estimate describes the window as a whole, so it trails the newest sample
	by about half a window (38 ms) plus the capture buffer.

	**Thread.** Render thread, inside Engine::update, once per frame on the
	newest window. Allocates only in init().
*/
namespace earshot
{

class PitchDetector
{
public:
	/// Allocates for the largest window: three periods of `minHzFloor` at `maxRate`.
	bool init( int maxRate, float minHzFloor ) noexcept;

	/// Samples of history this configuration needs at `fs`.
	int windowFor( int fs, const Config& config ) const noexcept;

	/// `mono` holds `warm` samples of history, then the newest `n` (= windowFor()).
	void analyse( const float* mono, int n, int warm, int fs, const Config& config, const testing::Mutations& mut,
				  const Fft& fft, Pitch& out ) noexcept;

	/// History the low-pass wants before the window.
	static constexpr int kWarm = 256;

	/// The NSDF of the last analysis, for the harness.
	const float* nsdf() const noexcept { return nsdf_.data(); }

private:
	void spectralPeak( int n, int fs, const Config& config, const Fft& fft, Pitch& out ) noexcept;
	/// Low-pass (mono - mean) into x_; `energy` gets sum(x_^2) over the window.
	void lowPass( const float* mono, int n, int warm, double mean, double fc, int fs, double& energy ) noexcept;
	/// NSDF of x_, key maxima, MPM's choice, parabolic refinement.
	bool estimate( int n, double energy, int fs, double minHz, double maxHz, const testing::Mutations& mut,
				   const Fft& fft, double& lag, double& peak ) noexcept;

	float                                minHzFloor_ = 40.0f;
	int                                  maxWindow_  = 0;
	std::vector< float >                 x_;
	std::vector< float >                 nsdf_;
	std::vector< std::complex< float > > buf_;
};

} // namespace earshot
