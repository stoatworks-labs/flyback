#pragma once

#include <complex>
#include <vector>

/**
	A radix-2 complex FFT, vendored rather than depended on (spasis's, with an
	inverse and any power-of-two size up to the one it was built for).

	Every plugin that vendors earshot links its own copy, so a dependency here
	is paid once per bundle. One twiddle table at the largest size serves every
	smaller size by striding through it. The table is computed in double and
	stored as float: a running-angle recurrence costs about 20 dB of dynamic
	range at 8192 points (spasis's measurement). `estest --fft` checks both
	directions against a direct DFT.

	Not normalised: forward then inverse multiplies by n.
*/
namespace earshot
{

class Fft
{
public:
	/// Allocates. `maxSize` must be a power of two. `floatRecurrence` builds
	/// the table by a float running-angle recurrence: the harness's negative
	/// control (fftFloatTwiddles) only.
	bool init( int maxSize, bool floatRecurrence = false ) noexcept;
	int  maxSize() const noexcept { return maxSize_; }

	/// In place. `n` a power of two, 2 <= n <= maxSize.
	void forward( std::complex< float >* data, int n ) const noexcept { transform( data, n, false ); }
	void inverse( std::complex< float >* data, int n ) const noexcept { transform( data, n, true ); }

private:
	void transform( std::complex< float >* data, int n, bool inverse ) const noexcept;

	int                                  maxSize_ = 0;
	std::vector< std::complex< float > > twiddles_;
};

} // namespace earshot
