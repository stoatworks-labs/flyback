#pragma once

#include "earshot/earshot.h"
#include "earshot/testing.h"

/**
	The stereo pair, over the newest ~43 ms (2048 samples at 48 kHz).

	Correlation is the phase meter's: sum(LR) / sqrt(sum(L^2) sum(R^2)), no
	mean removed, 0 when either side is silent (a hard-panned signal has no
	correlation to report, not a correlation of zero by accident of 0/0).
	Width is the side energy fraction, sum((L-R)^2) / (2 sum(L^2 + R^2)),
	which is spasis's width formula 1 - |L+R|^2 / (2 (|L|^2 + |R|^2)) with
	the two channels in the time domain: 0 for mono, 0.5 for uncorrelated
	equal levels or a hard pan, 1 for anti-phase.

	**Latency**: half the window, 21 ms. **Thread**: render thread. No allocation.
*/
namespace earshot
{

struct StereoMeter
{
	static int windowFor( int fs ) noexcept;
	static void analyse( const float* left, const float* right, int n, bool mono, const testing::Mutations& mut,
						 Stereo& out ) noexcept;
};

} // namespace earshot
