#include "Fft.h"

#include "Dsp.h"

#include <cmath>
#include <utility>

namespace earshot
{

bool Fft::init( int maxSize, bool floatRecurrence ) noexcept
{
	if( maxSize < 2 || ( maxSize & ( maxSize - 1 ) ) != 0 )
		return false;
	try
	{
		twiddles_.resize( (size_t)maxSize / 2 );
	}
	catch( ... )
	{
		return false;
	}
	if( floatRecurrence )
	{
		// The negative control: the design spasis measured and rejected.
		const float                 a = (float)( -dsp::kTwoPi / (double)maxSize );
		const std::complex< float > step( std::cos( a ), std::sin( a ) );
		std::complex< float >       w( 1.0f, 0.0f );
		for( int i = 0; i < maxSize / 2; ++i, w *= step )
			twiddles_[ (size_t)i ] = w;
	}
	else
		for( int i = 0; i < maxSize / 2; ++i )
		{
			const double a = -dsp::kTwoPi * (double)i / (double)maxSize;
			twiddles_[ (size_t)i ] = std::complex< float >( (float)std::cos( a ), (float)std::sin( a ) );
		}
	maxSize_ = maxSize;
	return true;
}

void Fft::transform( std::complex< float >* data, int n, bool inverse ) const noexcept
{
	if( n < 2 || n > maxSize_ || ( n & ( n - 1 ) ) != 0 )
		return;

	// Bit reversal, computed in place: no per-size table to allocate.
	for( int i = 1, j = 0; i < n; ++i )
	{
		int bit = n >> 1;
		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
			std::swap( data[ i ], data[ j ] );
	}

	const int tableStride = maxSize_ / n;
	for( int half = 1; half < n; half <<= 1 )
	{
		const int stride = tableStride * ( n / ( half * 2 ) );
		for( int start = 0; start < n; start += half * 2 )
		{
			for( int k = 0; k < half; ++k )
			{
				std::complex< float > w = twiddles_[ (size_t)( k * stride ) ];
				if( inverse )
					w = std::conj( w );
				const std::complex< float > a = data[ start + k ];
				const std::complex< float > b = data[ start + k + half ] * w;
				data[ start + k ]        = a + b;
				data[ start + k + half ] = a - b;
			}
		}
	}
}

} // namespace earshot
