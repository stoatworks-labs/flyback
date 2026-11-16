#pragma once

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <cstdint>
#include <vector>

/**
	The waveform tap: the stereo pair resampled to a fixed rate (Config::
	tapRate, 8 kHz by default), as a stream -- every sample once, in order,
	whatever the device rate, the block size or the frame rate.

	woofer drives a Thiele-Small model with this as the voltage. A model
	integrated at its own fixed step cannot use "the newest 2048 samples each
	frame": consecutive frames overlap or leave gaps, and a 44.1 kHz device
	would change the step. So the tap is a resampler on the absolute sample
	clock: output k is the input at t = k / tapRate exactly (the position is
	kept as an integer and a remainder, so it never drifts), through a
	Kaiser-windowed sinc (beta 8, about 81 dB stopband, ten zero crossings a
	side) with its cutoff at 0.4 tapRate, so nothing above the tap's Nyquist
	aliases into it by more than the stopband. Each output is normalised by
	the sum of the weights it used, so DC passes exactly at every phase.

	At 8 kHz: -6 dB at 3.2 kHz (0.4 R), flat within 0.001 dB to 2 kHz and
	at least 81 dB down from about 4 kHz (estest --tap measures 0.0004 dB at
	2 kHz and 87 dB of rejection at 4.4 kHz). A woofer's excursion is
	negligible well below 2 kHz.

	**Latency**: half the kernel, 1.6 ms at any rate, plus however long the
	plugin waits to read. **Thread**: render thread (inside update, and
	readTap). No allocation after init.
*/
namespace earshot
{

class WaveTap
{
public:
	static constexpr int kMinRate = 4000;
	static constexpr int kMaxRate = 48000;

	bool init() noexcept;
	void setRates( int fs, int tapRate, const testing::Mutations& mut ) noexcept;
	void reset() noexcept;

	/// The pair in sequence; `firstSample` is the absolute index of left[0].
	void push( const float* left, const float* right, int n, uint64_t firstSample ) noexcept;

	int  available() const noexcept { return (int)( produced_ - consumed_ ); }
	int  read( float* left, float* right, int maxSamples ) noexcept;
	/// The newest `n` produced samples, without consuming (the negative control).
	int  peekNewest( float* left, float* right, int n ) const noexcept;
	int  rate() const noexcept { return tapRate_; }
	uint64_t dropped() const noexcept { return dropped_; }

private:
	static constexpr int kZeroCrossings = 10;
	static constexpr int kTableRes      = 512;   ///< points per zero crossing
	static constexpr int kHistory       = 8192;  ///< input samples kept (power of two)
	static constexpr int kQueue         = 65536; ///< output samples kept (power of two)

	std::vector< float > table_;   ///< windowed sinc over [0, kZeroCrossings]
	std::vector< float > histL_, histR_, outL_, outR_;

	int      fs_ = 0, tapRate_ = 0;
	double   scale_  = 0.0;   ///< input samples -> zero crossings (2 fc / fs)
	double   halfIn_ = 0.0;   ///< kernel half width in input samples
	uint64_t pushed_ = 0;     ///< input samples received
	uint64_t next_   = 0;     ///< next output index k
	uint64_t produced_ = 0, consumed_ = 0, dropped_ = 0;
};

} // namespace earshot
