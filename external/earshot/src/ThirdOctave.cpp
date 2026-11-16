#include "ThirdOctave.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace earshot
{
namespace
{
using cd = std::complex< double >;

constexpr double kFastSeconds = 0.125;   ///< IEC 61672-1 time weighting F

/// Section magnitude |(1 - z^-2) / (1 + a1 z^-1 + a2 z^-2)| at angle w.
double sectionMagnitude( double a1, double a2, double w )
{
	const cd z1  = std::polar( 1.0, -w );
	const cd z2  = z1 * z1;
	const cd num = 1.0 - z2;
	const cd den = 1.0 + a1 * z1 + a2 * z2;
	return std::abs( num / den );
}
} // namespace

float ThirdOctaveBank::nominalFor( int x ) noexcept
{
	// ISO 266's R10 preferred numbers, one decade, repeated.
	static const float kDecade[ 10 ] = { 100.0f, 125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f, 630.0f, 800.0f };
	const int k      = x + 20;   // x = -20 is 10 Hz
	const int decade = k / 10;
	const int step   = k % 10;
	return (float)( kDecade[ step ] * std::pow( 10.0, decade ) / 10.0 );
}

void ThirdOctaveBank::BandFilter::design( double fm, double fs, int order, bool prewarp, double G ) noexcept
{
	exactHz = fm;
	loHz    = fm * std::pow( G, -1.0 / 6.0 );
	hiHz    = fm * std::pow( G, 1.0 / 6.0 );

	const double k  = 2.0 * fs;
	const double w1 = prewarp ? k * std::tan( dsp::kPi * loHz / fs ) : dsp::kTwoPi * loHz;
	const double w2 = prewarp ? k * std::tan( dsp::kPi * hiHz / fs ) : dsp::kTwoPi * hiHz;
	const double w0 = std::sqrt( w1 * w2 );
	const double B  = w2 - w1;
	const double wc = 2.0 * std::atan( w0 / k );   // where the analog centre lands

	order    = std::max( 1, std::min( order, kMaxOrder ) );
	sections = 0;
	for( int i = 1; i <= order; ++i )
	{
		// Butterworth prototype pole, left half plane, unit radius.
		const cd p    = std::polar( 1.0, dsp::kPi * ( 2.0 * i + order - 1.0 ) / ( 2.0 * order ) );
		const cd pb   = p * B;
		const cd root = std::sqrt( pb * pb - 4.0 * w0 * w0 );
		for( const cd s : { ( pb + root ) * 0.5, ( pb - root ) * 0.5 } )
		{
			const cd z = ( k + s ) / ( k - s );
			if( z.imag() <= 0.0 || sections >= kMaxOrder )
				continue;   // the conjugate builds the same section
			Section& q = sec[ sections ];
			q.a1 = -2.0 * z.real();
			q.a2 = std::norm( z );
			const double g = 1.0 / sectionMagnitude( q.a1, q.a2, wc );
			q.b0 = g;
			q.b1 = 0.0;
			q.b2 = -g;
			++sections;
		}
	}
	clear();
}

int ThirdOctaveBank::orderFor( double fm, double fs, double G, bool prewarp ) noexcept
{
	// The analog prototype the design will use.
	const double loHz = fm * std::pow( G, -1.0 / 6.0 ), hiHz = fm * std::pow( G, 1.0 / 6.0 );
	auto analog = [ & ]( double f ) { return prewarp ? std::tan( dsp::kPi * f / fs ) : dsp::kPi * f / fs; };
	const double w1 = analog( loHz ), w2 = analog( hiHz );
	const double w0 = std::sqrt( w1 * w2 ), B = w2 - w1;
	// IEC 61260 class 1 minimum attenuation at the octave breakpoints G^1..G^4,
	// mapped to a third octave (see estest's CheckThirds.cpp for the source).
	const double ratio  = ( std::pow( G, 1.0 / 6.0 ) - 1.0 ) / ( std::pow( G, 0.5 ) - 1.0 );
	const double need[] = { 17.5, 42.0, 61.0, 70.0 };
	int order = 3;
	for( int x = 1; x <= 4; ++x )
	{
		const double om = 1.0 + ratio * ( std::pow( G, (double)x ) - 1.0 );
		for( double f : { fm / om, fm * om } )
		{
			if( f >= 0.5 * fs )
				continue;   // above Nyquist: the transform's zero at Nyquist covers it
			const double w = analog( f );
			const double X = std::fabs( w / w0 - w0 / w ) * w0 / B;
			while( order < kMaxOrder && 10.0 * std::log10( 1.0 + std::pow( X, 2.0 * order ) ) < need[ x - 1 ] + 1.0 )
				++order;
		}
	}
	return order;
}

void ThirdOctaveBank::BandFilter::clear() noexcept
{
	for( Section& s : sec )
		s.s1 = s.s2 = 0.0;
}

void ThirdOctaveBank::setRate( int fs, const testing::Mutations& mut ) noexcept
{
	fs_ = fs;
	count_ = 0;
	const double G     = mut.thirdBase2 ? 2.0 : std::pow( 10.0, 0.3 );
	for( int i = 0; i < kMaxThirds; ++i )
	{
		const int    x  = kFirstX + i;
		const double fm = mut.thirdBase2 ? 1000.0 * std::pow( 2.0, x / 3.0 ) : 1000.0 * std::pow( 10.0, x / 10.0 );
		if( fm * std::pow( G, 1.0 / 6.0 ) >= kTopEdge * 0.5 * fs )
			break;
		BandFilter& b     = bands_[ count_ ];
		const int   order = mut.thirdOrder2 ? 2 : ( mut.thirdFixedOrder ? 3 : orderFor( fm, (double)fs, G, !mut.thirdNoPrewarp ) );
		b.design( fm, (double)fs, order, !mut.thirdNoPrewarp, G );
		b.single    = mut.thirdFloatArithmetic;
		b.nominalHz = nominalFor( x );
		++count_;
	}
	alpha_ = 1.0 - std::exp( -1.0 / ( ( mut.thirdSlowWeighting ? 1.0 : kFastSeconds ) * fs ) );
	noHold_ = mut.thirdNoHold;
	reset();
}

void ThirdOctaveBank::reset() noexcept
{
	for( int i = 0; i < kMaxThirds; ++i )
	{
		bands_[ i ].clear();
		ms_[ i ]      = 0.0;
		hold_[ i ]    = -200.0;
		held_[ i ]    = 0.0;
		blockSq_[ i ] = 0.0;
	}
	blockN_ = 0;
	primed_ = false;
}

void ThirdOctaveBank::push( const float* mono, int n ) noexcept
{
	for( int b = 0; b < count_; ++b )
	{
		BandFilter& f  = bands_[ b ];
		double      ms = ms_[ b ];
		double      sq = 0.0;
		for( int i = 0; i < n; ++i )
		{
			const double y  = f.process( (double)mono[ i ] );
			const double y2 = y * y;
			sq += y2;
			ms += ( y2 - ms ) * alpha_;
		}
		ms_[ b ] = ms;
		blockSq_[ b ] += sq;
	}
	blockN_ += n;
}

void ThirdOctaveBank::publish( Third* out, int& count, double dt, const Config& config ) noexcept
{
	if( !primed_ && blockN_ > 0 )
	{
		// The first block: start the weighting at its mean square, so the bars
		// do not climb out of silence for 125 ms on the first frame.
		for( int b = 0; b < count_; ++b )
		{
			ms_[ b ]   = blockSq_[ b ] / blockN_;
			hold_[ b ] = dsp::dbFromPower( 2.0 * ms_[ b ] );
			held_[ b ] = 0.0;
		}
		primed_ = true;
		dt      = 0.0;
	}
	for( int b = 0; b < count_; ++b )
	{
		const BandFilter& f  = bands_[ b ];
		Third&            o  = out[ b ];
		const double      db = dsp::dbFromPower( 2.0 * ms_[ b ] );   // 0 dB = a full-scale sine
		if( db >= hold_[ b ] )
		{
			hold_[ b ] = db;
			held_[ b ] = 0.0;
		}
		else if( dt > 0.0 )
		{
			held_[ b ] += dt;
			const double over = held_[ b ] - ( noHold_ ? 0.0 : (double)config.thirdHoldSeconds );
			if( over > 0.0 )
				hold_[ b ] = std::max( db, hold_[ b ] - (double)config.thirdFallDbPerSecond * std::min( dt, over ) );
		}
		o.nominalHz = f.nominalHz;
		o.exactHz   = (float)f.exactHz;
		o.loHz      = (float)f.loHz;
		o.hiHz      = (float)f.hiHz;
		o.db        = (float)db;
		o.holdDb    = (float)hold_[ b ];
		blockSq_[ b ] = 0.0;
	}
	blockN_ = 0;
	count   = count_;
}

} // namespace earshot
