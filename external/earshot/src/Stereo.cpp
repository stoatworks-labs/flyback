#include "Stereo.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace earshot
{

int StereoMeter::windowFor( int fs ) noexcept
{
	const double want = 0.0427 * fs;
	int          n    = dsp::nextPow2( (int)want );
	if( (double)n / want > std::sqrt( 2.0 ) )
		n >>= 1;
	return std::max( 64, n );
}

void StereoMeter::analyse( const float* left, const float* right, int n, bool mono, const testing::Mutations& mut,
						   Stereo& out ) noexcept
{
	out       = Stereo();
	out.valid = true;
	out.mono  = mono;
	if( left == nullptr || right == nullptr || n <= 0 )
		return;
	if( mut.stereoSwap )
		std::swap( left, right );

	double ll = 0.0, rr = 0.0, lr = 0.0, pl = 0.0, pr = 0.0;
	const bool monoLeftOnly = mono && mut.stereoMonoLeftOnly;
	for( int i = 0; i < n; ++i )
	{
		const double l = left[ i ], r = monoLeftOnly ? 0.0 : right[ i ];
		ll += l * l;
		rr += r * r;
		lr += l * r;
		pl = std::max( pl, std::fabs( l ) );
		pr = std::max( pr, std::fabs( r ) );
	}
	out.rmsL  = (float)std::sqrt( ll / n );
	out.rmsR  = (float)std::sqrt( rr / n );
	out.peakL = (float)pl;
	out.peakR = (float)pr;

	if( mut.stereoNoNormalise )
		out.correlation = (float)( lr / n );
	else if( ll > 1e-30 && rr > 1e-30 )
		out.correlation = (float)dsp::clampd( lr / std::sqrt( ll * rr ), -1.0, 1.0 );

	const double total = ll + rr;
	if( mut.stereoWidthSideOverMid )
	{
		// The negative control: side over mid energy, the goniometer shortcut.
		const double mid = 0.25 * ( total + 2.0 * lr ), side = 0.25 * ( total - 2.0 * lr );
		out.width = mid > 1e-30 ? (float)dsp::clampd( side / mid, 0.0, 1.0 ) : ( side > 1e-30 ? 1.0f : 0.0f );
	}
	else
		out.width = total > 1e-30 ? (float)dsp::clampd( ( total - 2.0 * lr ) / ( 2.0 * total ), 0.0, 1.0 ) : 0.0f;

	if( ll > 1e-30 && rr > 1e-30 )
		out.balanceDb = (float)dsp::clampd( 10.0 * std::log10( ll / rr ), -60.0, 60.0 );
	else if( ll > 1e-30 )
		out.balanceDb = 60.0f;
	else if( rr > 1e-30 )
		out.balanceDb = -60.0f;
}

} // namespace earshot
