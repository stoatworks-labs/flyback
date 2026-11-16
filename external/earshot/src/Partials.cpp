#include "Partials.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace earshot
{
namespace
{
constexpr double kWindowSeconds = 0.17;
constexpr double kFloorDb       = -90.0;  ///< dB re full scale: quieter peaks are never partials
constexpr double kRangeDb       = 60.0;   ///< and nothing this far below the strongest
constexpr double kLowHz         = 20.0;
constexpr double kHarmonicCents = 30.0;
} // namespace

int PartialTracker::windowFor( int fs ) noexcept
{
	// The power of two nearest kWindowSeconds, in log terms.
	const double want = kWindowSeconds * fs;
	int          n    = dsp::nextPow2( (int)want );
	if( (double)n / want > std::sqrt( 2.0 ) )
		n >>= 1;
	return std::max( 256, n );
}

bool PartialTracker::init( int maxRate ) noexcept
{
	const int n = windowFor( maxRate );   // windowFor rises with fs, so this covers every rate
	try
	{
		buf_.assign( (size_t)n, std::complex< float >() );
		window_.assign( (size_t)n, 0.0f );
	}
	catch( ... )
	{
		return false;
	}
	return true;
}

void PartialTracker::analyse( const float* mono, int n, int fs, const Pitch& pitch, const testing::Mutations& mut,
							  const Fft& fft, Partial* out, int& count ) noexcept
{
	count = 0;
	if( mono == nullptr || n < 16 || n > (int)buf_.size() || n > fft.maxSize() || ( n & ( n - 1 ) ) != 0 )
		return;

	if( windowSize_ != n )
	{
		for( int i = 0; i < n; ++i )
			window_[ (size_t)i ] = dsp::hann( i, n );
		windowSize_ = n;
	}
	double sumW = 0.0;
	for( int i = 0; i < n; ++i )
	{
		buf_[ (size_t)i ] = std::complex< float >( mono[ i ] * window_[ (size_t)i ], 0.0f );
		sumW += window_[ (size_t)i ];
	}
	fft.forward( buf_.data(), n );

	// A sine of amplitude A peaks at |X| = A * sum(w) / 2.
	const double toAmplitude = 2.0 / sumW;
	const double binHz       = (double)fs / n;
	const int    lo          = std::max( 2, (int)std::ceil( kLowHz / binHz ) );
	const int    hi          = std::min( n / 2 - 2, (int)( 0.45 * fs / binHz ) );

	Partial best[ kMaxPartials ];
	int     have = 0;
	for( int k = lo; k <= hi; ++k )
	{
		const float m0 = std::norm( buf_[ (size_t)k ] );
		if( !( m0 > std::norm( buf_[ (size_t)( k - 1 ) ] ) && m0 >= std::norm( buf_[ (size_t)( k + 1 ) ] ) ) )
			continue;
		const double a = 0.5 * std::log( (double)std::norm( buf_[ (size_t)( k - 1 ) ] ) + 1e-40 );
		const double b = 0.5 * std::log( (double)m0 + 1e-40 );
		const double c = 0.5 * std::log( (double)std::norm( buf_[ (size_t)( k + 1 ) ] ) + 1e-40 );
		double delta = 0.0, peak = b;
		const double d = a - 2.0 * b + c;
		if( !mut.partialsNoInterp && d < 0.0 )
		{
			delta = 0.5 * ( a - c ) / d;
			peak  = b - 0.25 * ( a - c ) * delta;
		}
		Partial p;
		p.hz        = (float)( ( k + delta ) * binHz );
		p.amplitude = (float)( std::exp( peak ) * toAmplitude );
		p.db        = (float)dsp::dbFromAmplitude( p.amplitude );
		if( p.db < kFloorDb )
			continue;

		// Insert into the sorted top-K (loudest first).
		int at = have;
		while( at > 0 && best[ at - 1 ].amplitude < p.amplitude )
			--at;
		if( at >= kMaxPartials )
			continue;
		for( int j = std::min( have, kMaxPartials - 1 ); j > at; --j )
			best[ j ] = best[ j - 1 ];
		best[ at ] = p;
		have       = std::min( have + 1, kMaxPartials );
	}

	const double floorDb = have > 0 ? best[ 0 ].db - kRangeDb : 0.0;
	for( int i = 0; i < have; ++i )
	{
		if( best[ i ].db < floorDb )
			break;
		Partial p = best[ i ];
		if( pitch.voiced && pitch.hz > 0.0f )
		{
			const int h = (int)std::lround( p.hz / pitch.hz );
			if( h >= 1 && std::fabs( dsp::cents( p.hz, h * (double)pitch.hz ) ) <= kHarmonicCents )
				p.harmonic = h;
		}
		out[ count++ ] = p;
	}
}

} // namespace earshot
