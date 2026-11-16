#include "Tempo.h"

#include <algorithm>
#include <cmath>

namespace earshot
{
namespace
{
// pattern v0.1.1's constants (source/tracker/Tracker.h there), unchanged.
constexpr int    kWindow       = 600;    ///< cells: six seconds
constexpr int    kLagMin       = 30;     ///< 200 BPM
constexpr int    kLagMax       = 100;    ///< 60 BPM
constexpr int    kMaxLag       = 302;    ///< autocorrelation kept (3 s)
constexpr int    kOwnLagMax    = 200;    ///< a register's own periodicity: 0.3 to 2 s
constexpr double kSettleR      = 0.2;    ///< normalised peak correlation to trust
constexpr double kLevelRatio   = 0.70;   ///< fastest member scoring this share of the best wins
constexpr double kPriorBpm     = 120.0;
constexpr double kPriorOctaves = 1.0;
constexpr double kAgree        = 0.02;   ///< two readings in a row within this ratio publish
constexpr int    kMinCells     = 300;    ///< three seconds before the first reading
constexpr int    kMinOnsets    = 6;      ///< onset hops in the window before a reading (earshot's gate)

/// Unbiased autocorrelation (each lag's sum over its own overlap), normalised
/// by the lag-0 power. pattern's `AutoCorr`.
double autoCorr( const double* x, int n, int lag, double power )
{
	double acc = 0.0;
	for( int i = 0; i + lag < n; ++i )
		acc += x[ i ] * x[ i + lag ];
	return acc / (double)( n - lag ) / power;
}

/// The largest linearly interpolated R within one cell of the fractional lag
/// t, so a period between grid samples is not penalised. pattern's `PeakNear`.
double peakNear( const double* r, int maxLag, double t )
{
	const int lo = (int)std::floor( t - 1.0 );
	const int hi = (int)std::ceil( t + 1.0 );
	if( lo < 0 || hi + 1 > maxLag )
		return 0.0;
	double best = -1e300;
	for( int q = 4 * lo; q <= 4 * hi; ++q )
	{
		const int    i = q >> 2;
		const double f = ( q & 3 ) * 0.25;
		best           = std::max( best, r[ i ] * ( 1.0 - f ) + r[ i + 1 ] * f );
	}
	return best;
}
} // namespace

void TempoTracker::reset() noexcept
{
	for( auto& e : env_ )
		std::fill( e, e + kEnvSize, 0.0 );
	pendingCell_ = -1;
	lastCell_    = -1;
	lastSecond_  = -1;
	detected_    = 0.0;
	candidate_   = 0.0;
	peakR_       = 0.0;
	readings_    = 0;
	onsetHead_   = 0;
	onsetCount_  = 0;
}

void TempoTracker::push( double t, const double* reg, double hopRate, bool onset ) noexcept
{
	if( !( hopRate > 0.0 ) || !( t > 0.0 ) )
		return;
	if( onset )
	{
		onsets_[ onsetHead_ ] = t;
		onsetHead_            = ( onsetHead_ + 1 ) % 64;
		onsetCount_           = std::min( onsetCount_ + 1, 64 );
	}
	const double dt = 1.0 / hopRate;
	const double t0 = std::max( 0.0, t - dt );

	// Resample by area: the hop's flux is the rise over [t0, t), shared among
	// the 10 ms cells it overlaps in proportion. Hops at 5.3 ms (48 kHz) or
	// 5.8 ms (44.1 kHz) then make the same 100 Hz envelope.
	const int64_t c0 = (int64_t)std::floor( t0 * kRate );
	const int64_t c1 = (int64_t)std::floor( t * kRate - 1e-9 );
	while( pendingCell_ < c1 )
	{
		++pendingCell_;
		for( int b = 0; b < 3; ++b )
			env_[ b ][ pendingCell_ % kEnvSize ] = 0.0;
	}
	for( int64_t c = c0; c <= c1; ++c )
	{
		if( c < 0 || c <= pendingCell_ - kEnvSize )
			continue;
		const double a = std::max( t0, (double)c / kRate );
		const double z = std::min( t, (double)( c + 1 ) / kRate );
		if( z <= a )
			continue;
		const double share = ( z - a ) / dt;
		for( int b = 0; b < 3; ++b )
			env_[ b ][ c % kEnvSize ] += reg[ b ] * share;
	}
	// Cells strictly before the one holding t are complete.
	lastCell_ = std::max( lastCell_, (int64_t)std::floor( t * kRate ) - 1 );

	const int64_t second = (int64_t)std::floor( t );
	if( second > lastSecond_ )
	{
		lastSecond_ = second;
		int recent = 0;
		for( int i = 0; i < onsetCount_; ++i )
			recent += onsets_[ i ] > t - kWindow / (double)kRate ? 1 : 0;
		if( lastCell_ + 1 >= kMinCells && recent < kMinOnsets && !( mut != nullptr && mut->tempoNoOnsetGate ) )
		{
			candidate_ = 0.0;   // nothing to read; the next two readings must agree afresh
			peakR_     = 0.0;
		}
		else if( lastCell_ + 1 >= kMinCells )
		{
			if( mut != nullptr && mut->tempoLegacy )
				detectLegacy();
			else
				detect();
		}
	}
}

void TempoTracker::detect() noexcept
{
	const int n = (int)std::min< int64_t >( lastCell_ + 1, kWindow );
	constexpr double kWeight[ 3 ] = { 1.0, 1.0, 0.5 };
	constexpr int    kSmooth      = 6;     ///< kernel half-width, cells
	constexpr double kSigma       = 2.0;   ///< cells: 20 ms

	double kernel[ 2 * kSmooth + 1 ];
	for( int k = -kSmooth; k <= kSmooth; ++k )
		kernel[ k + kSmooth ] = std::exp( -0.5 * ( k / kSigma ) * ( k / kSigma ) );

	double x[ kWindow ] = {};
	double raw[ kWindow ], z[ kWindow ];
	for( int b = 0; b < 3; ++b )
	{
		for( int i = 0; i < n; ++i )
			raw[ i ] = env_[ b ][ ( lastCell_ - n + 1 + i ) % kEnvSize ];
		double mean = 0.0;
		for( int i = 0; i < n; ++i )
		{
			double acc = 0.0, wsum = 0.0;
			for( int k = -kSmooth; k <= kSmooth; ++k )
				if( i + k >= 0 && i + k < n )
				{
					acc += kernel[ k + kSmooth ] * raw[ i + k ];
					wsum += kernel[ k + kSmooth ];
				}
			z[ i ] = acc / wsum;
			mean += z[ i ];
		}
		mean /= n;
		double power = 0.0;
		for( int i = 0; i < n; ++i )
		{
			z[ i ] -= mean;
			power += z[ i ] * z[ i ];
		}
		power /= n;
		if( !( power > 1e-18 ) )
			continue;
		double    own    = 0.0;
		const int ownMax = std::min( kOwnLagMax, n - 60 );
		for( int lag = kLagMin; lag <= ownMax; ++lag )
			own = std::max( own, autoCorr( z, n, lag, power ) );
		const double w = kWeight[ b ] * own / std::sqrt( power );
		for( int i = 0; i < n; ++i )
			x[ i ] += w * z[ i ];
	}

	double power = 0.0;
	for( int i = 0; i < n; ++i )
		power += x[ i ] * x[ i ];
	power /= n;
	if( !( power > 1e-18 ) )
		return;
	const int maxLag = std::min( kMaxLag, n - 60 );
	double    r[ kMaxLag + 2 ] = {};
	for( int lag = 0; lag <= maxLag; ++lag )
		r[ lag ] = autoCorr( x, n, lag, power );

	auto score = [ & ]( double t ) {
		return 0.5 * peakNear( r, maxLag, 0.5 * t ) + peakNear( r, maxLag, t ) + peakNear( r, maxLag, 2.0 * t );
	};
	double family = kLagMin, bestScore = -1e300;
	for( int q = 4 * kLagMin; q <= 4 * kLagMax; ++q )
	{
		const double t     = 0.25 * q;
		const double octs  = std::log2( 60.0 * kRate / t / kPriorBpm ) / kPriorOctaves;
		const double prior = std::exp( -0.5 * octs * octs );
		const double s     = score( t ) * prior;
		if( s > bestScore )
		{
			bestScore = s;
			family    = t;
		}
	}
	double members[ 5 ], scores[ 5 ], top = -1e300;
	int    count = 0;
	for( int j = -2; j <= 2; ++j )
	{
		const double t = family * std::ldexp( 1.0, j );
		if( t < kLagMin || t > kLagMax )
			continue;
		members[ count ] = t;
		scores[ count ]  = score( t );
		top              = std::max( top, scores[ count ] );
		++count;
	}
	double     best    = family;
	const bool slowest = mut != nullptr && mut->tempoSlowestLevel;   // the negative control
	for( int k = 0; k < count; ++k )   // ascending lag: the first to pass is the fastest
	{
		const int i = slowest ? count - 1 - k : k;
		if( scores[ i ] >= kLevelRatio * top )
		{
			best = members[ i ];
			break;
		}
	}

	const double peak = peakNear( r, maxLag, best );
	readings_++;
	if( peak < kSettleR )
	{
		candidate_ = 0.0;
		return;
	}

	const int    m  = std::max( 1, std::min( 4, (int)( ( maxLag - 3 ) / best ) ) );
	const double c  = m * best;
	const int    hw = std::max( 1, (int)( best / 8.0 ) );
	const int    lo = std::max( 1, (int)std::floor( c - hw ) );
	const int    hi = std::min( maxLag - 1, (int)std::ceil( c + hw ) );
	int          at = lo;
	for( int lag = lo; lag <= hi; ++lag )
		if( r[ lag ] > r[ at ] )
			at = lag;
	double lag = at;
	{
		const double a = r[ at - 1 ], b = r[ at ], cc = r[ at + 1 ];
		const double d = a - 2.0 * b + cc;
		if( d < 0.0 )
			lag += 0.5 * ( a - cc ) / d;
	}
	lag /= m;
	const double reading = 60.0 * kRate / lag;
	const auto   agrees  = []( double a, double b ) { return b > 0.0 && std::fabs( a / b - 1.0 ) <= kAgree; };
	const bool   publish = agrees( reading, candidate_ ) || agrees( reading, detected_ );
	candidate_ = reading;
	peakR_     = peak;
	if( publish )
		detected_ = reading;
}

/// pattern v0.1.0's detector, the negative control: the raw sum of the
/// registers, the best lag in range, the half of it at 70 %.
void TempoTracker::detectLegacy() noexcept
{
	const int n = (int)std::min< int64_t >( lastCell_ + 1, kWindow );
	double    x[ kWindow ];
	double    mean = 0.0;
	for( int i = 0; i < n; ++i )
	{
		const int64_t at = ( lastCell_ - n + 1 + i ) % kEnvSize;
		x[ i ]           = env_[ 0 ][ at ] + env_[ 1 ][ at ] + env_[ 2 ][ at ];
		mean += x[ i ];
	}
	mean /= n;
	double energy = 0.0;
	for( int i = 0; i < n; ++i )
	{
		x[ i ] -= mean;
		energy += x[ i ] * x[ i ];
	}
	readings_++;
	if( !( energy > 0.0 ) )
		return;
	double r[ kLagMax + 2 ] = {};
	for( int lag = kLagMin - 1; lag <= kLagMax + 1; ++lag )
	{
		double acc = 0.0;
		for( int i = 0; i + lag < n; ++i )
			acc += x[ i ] * x[ i + lag ];
		r[ lag ] = acc / energy;
	}
	int best = kLagMin;
	for( int lag = kLagMin; lag <= kLagMax; ++lag )
		if( r[ lag ] > r[ best ] )
			best = lag;
	for( int half : { best / 2, ( best + 1 ) / 2 } )
		if( half >= kLagMin && half < best && r[ half ] >= 0.7 * r[ best ] )
		{
			best = half;
			break;
		}
	if( r[ best ] < kSettleR )
		return;
	double lag = best;
	if( best > kLagMin - 1 && best < kLagMax + 1 )
	{
		const double a = r[ best - 1 ], b = r[ best ], c = r[ best + 1 ];
		const double d = a - 2.0 * b + c;
		if( d < 0.0 )
			lag += 0.5 * ( a - c ) / d;
	}
	peakR_     = r[ best ];
	candidate_ = 60.0 * kRate / lag;
	detected_  = candidate_;
}

void TempoTracker::publish( Tempo& out ) const noexcept
{
	out.valid      = readings_ > 0;
	out.settled    = detected_ > 0.0;
	out.bpm        = (float)( detected_ > 0.0 ? detected_ : candidate_ );
	out.confidence = (float)peakR_;
}

} // namespace earshot
