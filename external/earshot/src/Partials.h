#pragma once

#include "Fft.h"

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <complex>
#include <vector>

/**
	The strongest partials: a Hann-windowed FFT of the newest 170 ms, its local
	maxima refined by quadratic interpolation of the log magnitude (QIFFT;
	Smith & Serra 1987, Abe & Smith 2004), the loudest kMaxPartials kept.

	The window is a power of two near 170 ms (8192 samples at 44.1 and 48 kHz),
	so partials 6 Hz apart sit a whole bin apart and a 40 Hz fundamental's
	harmonics are resolved. Amplitude is corrected for the window's coherent
	gain, so a sine of amplitude A reads A. The interpolator's own bias on this
	window is a closed-form property of Hann and QIFFT, and `estest --partials`
	computes it from the window's transform and uses it as the tolerance.

	A partial is labelled with the nearest harmonic of the detected pitch when
	it lies within 30 cents of one.

	**Latency**: the window is centred 85 ms behind the newest sample.
	**Thread**: render thread, inside Engine::update. Allocates only in init().
*/
namespace earshot
{

class PartialTracker
{
public:
	bool init( int maxRate ) noexcept;

	/// Samples of history needed at `fs`.
	static int windowFor( int fs ) noexcept;

	void analyse( const float* mono, int n, int fs, const Pitch& pitch, const testing::Mutations& mut,
				  const Fft& fft, Partial* out, int& count ) noexcept;

private:
	std::vector< std::complex< float > > buf_;
	std::vector< float >                 window_;
	int                                  windowSize_ = 0;
};

} // namespace earshot
