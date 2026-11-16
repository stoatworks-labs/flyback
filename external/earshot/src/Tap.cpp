#include "Tap.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace earshot
{
namespace
{
constexpr double kBeta = 8.0;

/// Modified Bessel function of the first kind, order zero (power series).
double besselI0( double x )
{
	double sum = 1.0, term = 1.0;
	const double q = 0.25 * x * x;
	for( int k = 1; k < 64; ++k )
	{
		term *= q / ( (double)k * k );
		sum += term;
		if( term < sum * 1e-17 )
			break;
	}
	return sum;
}
} // namespace

bool WaveTap::init() noexcept
{
	try
	{
		table_.assign( (size_t)kZeroCrossings * kTableRes + 2, 0.0f );
		histL_.assign( kHistory, 0.0f );
		histR_.assign( kHistory, 0.0f );
		outL_.assign( kQueue, 0.0f );
		outR_.assign( kQueue, 0.0f );
	}
	catch( ... )
	{
		return false;
	}
	// sinc( x ) * kaiser( x / Z ), x in zero crossings. Rate independent, so it
	// is built once.
	const double norm = besselI0( kBeta );
	for( int i = 0; i <= kZeroCrossings * kTableRes + 1; ++i )
	{
		const double x    = (double)i / kTableRes;
		const double r    = x / kZeroCrossings;
		const double sinc = x == 0.0 ? 1.0 : std::sin( dsp::kPi * x ) / ( dsp::kPi * x );
		const double win  = r >= 1.0 ? 0.0 : besselI0( kBeta * std::sqrt( 1.0 - r * r ) ) / norm;
		table_[ (size_t)i ] = (float)( sinc * win );
	}
	return true;
}

void WaveTap::setRates( int fs, int tapRate, const testing::Mutations& mut ) noexcept
{
	fs_      = fs;
	tapRate_ = std::max( kMinRate, std::min( tapRate, std::min( kMaxRate, fs ) ) );
	// The negative control puts the cutoff at the INPUT's Nyquist: the kernel
	// then only interpolates, and everything above the tap's Nyquist folds in.
	const double fc = mut.tapNoAntiAlias ? 0.45 * fs : ( mut.tapCutoffQuarter ? 0.25 : 0.4 ) * tapRate_;
	scale_  = 2.0 * fc / fs;
	halfIn_ = kZeroCrossings / scale_;
	reset();
}

void WaveTap::reset() noexcept
{
	pushed_   = 0;
	next_     = 0;
	produced_ = 0;
	consumed_ = 0;
	std::fill( histL_.begin(), histL_.end(), 0.0f );
	std::fill( histR_.begin(), histR_.end(), 0.0f );
}

void WaveTap::push( const float* left, const float* right, int n, uint64_t firstSample ) noexcept
{
	if( fs_ <= 0 || tapRate_ <= 0 )
		return;
	(void)firstSample;   // the tap keeps its own count; the engine resets it on a gap
	const int mask = kHistory - 1;
	for( int i = 0; i < n; ++i )
	{
		histL_[ (size_t)( ( pushed_ + (uint64_t)i ) & (uint64_t)mask ) ] = left[ i ];
		histR_[ (size_t)( ( pushed_ + (uint64_t)i ) & (uint64_t)mask ) ] = right[ i ];
	}
	pushed_ += (uint64_t)n;

	const int qmask = kQueue - 1;
	for( ;; )
	{
		// Output k sits at input position u = k fs / R, kept exact as q + rem / R.
		const uint64_t whole = next_ * (uint64_t)fs_;
		const uint64_t q     = whole / (uint64_t)tapRate_;
		const double   u     = (double)q + (double)( whole % (uint64_t)tapRate_ ) / tapRate_;
		const int64_t  lo    = (int64_t)std::ceil( u - halfIn_ );
		const int64_t  hi    = (int64_t)std::floor( u + halfIn_ );
		if( hi >= (int64_t)pushed_ )
			break;   // not enough input yet
		double sl = 0.0, sr = 0.0, sw = 0.0;
		for( int64_t j = std::max< int64_t >( lo, (int64_t)pushed_ - kHistory ); j <= hi; ++j )
		{
			if( j < 0 )
				continue;
			const double x   = std::fabs( u - (double)j ) * scale_ * kTableRes;
			const int    at  = (int)x;
			if( at >= kZeroCrossings * kTableRes )
				continue;
			const double f   = x - at;
			const double w   = table_[ (size_t)at ] * ( 1.0 - f ) + table_[ (size_t)at + 1 ] * f;
			const size_t idx = (size_t)( (uint64_t)j & (uint64_t)mask );
			sl += w * histL_[ idx ];
			sr += w * histR_[ idx ];
			sw += w;
		}
		const float l = sw != 0.0 ? (float)( sl / sw ) : 0.0f;
		const float r = sw != 0.0 ? (float)( sr / sw ) : 0.0f;
		outL_[ (size_t)( produced_ & (uint64_t)qmask ) ] = l;
		outR_[ (size_t)( produced_ & (uint64_t)qmask ) ] = r;
		++produced_;
		++next_;
		if( produced_ - consumed_ > (uint64_t)kQueue )
		{
			dropped_ += produced_ - consumed_ - kQueue;
			consumed_ = produced_ - kQueue;
		}
	}
}

int WaveTap::read( float* left, float* right, int maxSamples ) noexcept
{
	const int n     = std::min( available(), std::max( 0, maxSamples ) );
	const int qmask = kQueue - 1;
	for( int i = 0; i < n; ++i )
	{
		const size_t at = (size_t)( ( consumed_ + (uint64_t)i ) & (uint64_t)qmask );
		if( left != nullptr )
			left[ i ] = outL_[ at ];
		if( right != nullptr )
			right[ i ] = outR_[ at ];
	}
	consumed_ += (uint64_t)n;
	return n;
}

int WaveTap::peekNewest( float* left, float* right, int n ) const noexcept
{
	n = (int)std::min< uint64_t >( (uint64_t)std::max( 0, n ), std::min< uint64_t >( produced_, kQueue ) );
	const int qmask = kQueue - 1;
	for( int i = 0; i < n; ++i )
	{
		const size_t at = (size_t)( ( produced_ - (uint64_t)n + (uint64_t)i ) & (uint64_t)qmask );
		if( left != nullptr )
			left[ i ] = outL_[ at ];
		if( right != nullptr )
			right[ i ] = outR_[ at ];
	}
	return n;
}

} // namespace earshot
