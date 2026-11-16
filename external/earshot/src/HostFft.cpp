#include "HostFft.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace earshot
{
namespace
{
constexpr double kPrimeFraction = 0.125;   ///< pattern v0.1.1
constexpr double kFloorSeconds  = 1.0;
constexpr double kAbsoluteFloor = 1e-4;    ///< pattern's: flux below this is never an onset
constexpr double kGapSeconds    = 1.0;     ///< a longer frame interval re-primes
constexpr double kHostRatio     = 3.0;     ///< pattern's: flux over three times its floor is an onset
constexpr double kLogFloorHz    = 20.0;
} // namespace

double HostAnalyser::binHz( int j, int count, const HostBins& law ) noexcept
{
	const double nyquist = 0.5 * law.sampleRate;
	const double u       = ( (double)j + 0.5 ) / (double)std::max( 1, count );
	if( law.layout == HostBins::Layout::Log )
		return kLogFloorHz * std::pow( nyquist / kLogFloorHz, u );
	return u * nyquist;
}

void HostAnalyser::reset() noexcept
{
	primed_ = false;
	for( State& s : bands_ )
	{
		s.onset    = false;
		s.lastFire = -1e9;
	}
}

void HostAnalyser::configure( const Config& config ) noexcept
{
	const int count = std::max( 1, std::min( config.bandCount, kMaxBands ) );
	bool      moved = count != bandCount_;
	for( int b = 0; b < count; ++b )
	{
		moved = moved || bands_[ b ].loHz != config.bandEdges[ b ] || bands_[ b ].hiHz != config.bandEdges[ b + 1 ];
		bands_[ b ].loHz = config.bandEdges[ b ];
		bands_[ b ].hiHz = config.bandEdges[ b + 1 ];
	}
	bandCount_ = count;
	if( moved )
		primed_ = false;   // bins change bands: prime again rather than read the move as flux
}

void HostAnalyser::frame( double hostSeconds, const float* bins, int count, const Config& config,
						  const testing::Mutations& mut ) noexcept
{
	count = std::max( 0, std::min( count, kMaxBins ) );
	double m[ kMaxBins ];
	double loudest = -1.0;
	for( int j = 0; j < count; ++j )
	{
		const float v = bins != nullptr ? bins[ j ] : 0.0f;
		// A host may hand back a negative or a NaN in a buffer it has not filled
		// yet; either would poison the floors for good (pattern, needle).
		const double clean = v > 0.0f ? (double)v : 0.0;
		m[ j ] = config.hostBins.value == HostBins::Value::Power && !mut.hostPowerAsMagnitude ? std::sqrt( clean ) : clean;
		if( m[ j ] > loudest )
		{
			loudest    = m[ j ];
			loudestHz_ = binHz( j, count, config.hostBins );
		}
	}

	double energy[ kMaxBands ] = {}, flux[ kMaxBands ] = {}, level[ kMaxBands ] = {};
	for( int j = 0; j < count; ++j )
	{
		const double hz = binHz( j, count, config.hostBins );
		for( int b = 0; b < bandCount_; ++b )
			if( hz >= bands_[ b ].loHz && hz < bands_[ b ].hiHz )
			{
				energy[ b ] += m[ j ] * m[ j ];
				level[ b ] += m[ j ];
				flux[ b ] += std::max( 0.0, m[ j ] - prev_[ j ] );
				break;
			}
	}

	double dt = hostSeconds - last_;
	if( primed_ && dt > kGapSeconds )
		primed_ = false;

	if( !primed_ )
	{
		for( int j = 0; j < count; ++j )
			prev_[ j ] = mut.onsetNoPrime ? 0.0 : m[ j ];
		for( int b = 0; b < bandCount_; ++b )
		{
			State& s = bands_[ b ];
			s.floor      = mut.onsetNoPrime ? 0.0 : ( mut.onsetFullSeed ? 1.0 : kPrimeFraction ) * level[ b ];
			s.prevFlux   = 0.0;
			s.prevEnergy = mut.onsetNoPrime ? 0.0 : energy[ b ];
			s.level      = std::sqrt( energy[ b ] );
			s.envelope   = s.level;
			s.above      = false;
		}
		if( origin_ == 0.0 && now_ == 0.0 )
			origin_ = hostSeconds;
		last_   = hostSeconds;
		now_    = hostSeconds;
		primed_ = true;
		if( !mut.onsetNoPrime )
			return;
		// The negative control goes on to judge this frame against silence:
		// every bin has risen by all of itself.
		dt = 0.0;
		for( int b = 0; b < bandCount_; ++b )
			flux[ b ] = level[ b ];
	}

	if( hostSeconds > now_ )
		now_ = hostSeconds;
	const double t = now_ - origin_;

	double maxRise = 0.0, rise[ kMaxBands ];
	for( int b = 0; b < bandCount_; ++b )
	{
		rise[ b ] = std::max( 0.0, energy[ b ] - bands_[ b ].prevEnergy );
		maxRise   = std::max( maxRise, rise[ b ] );
	}

	// dt <= 0 (a repeated or backward host time) moves nothing. The negative
	// control snaps instead, which is what the fleet's one-poles did.
	const double alpha     = ( dt <= 0.0 && mut.onsetSnapZeroDt ) ? 1.0 : dsp::onePoleAlpha( dt, kFloorSeconds );
	const double attack    = dsp::onePoleAlpha( dt, config.envelopeAttack );
	const double release   = dsp::onePoleAlpha( dt, config.envelopeRelease );
	const double maskRatio = std::pow( 10.0, -config.onsetMaskDb / 10.0 );

	for( int b = 0; b < bandCount_; ++b )
	{
		State& s = bands_[ b ];
		s.level  = std::sqrt( energy[ b ] );
		const double threshold = std::max( kAbsoluteFloor, s.floor * kHostRatio );
		const bool   over      = flux[ b ] > threshold;
		const bool   rising    = mut.onsetNoRisingEdge || !s.above;   // a crossing, as on the audio path
		const bool   armed     = t - s.lastFire >= (double)config.onsetRefractory;
		const bool   masked    = !mut.onsetNoMask && rise[ b ] < maxRise * maskRatio;
		if( dt > 0.0 || mut.onsetSnapZeroDt )
			s.above = over;   // a repeated frame is not a new hop
		if( over && rising && armed && !masked )
		{
			s.onset = true;
			s.count++;
			s.strength = flux[ b ] / threshold;
			s.lastTime = t;
			s.lastFire = t;
		}
		s.floor += ( flux[ b ] - s.floor ) * alpha;
		s.prevFlux   = flux[ b ];
		s.prevEnergy = energy[ b ];
		s.envelope += ( s.level - s.envelope ) * ( s.level > s.envelope ? attack : release );
	}
	for( int j = 0; j < count; ++j )
		prev_[ j ] = m[ j ];
	last_ = hostSeconds;
}

void HostAnalyser::publish( Band* out, int& count ) noexcept
{
	count = bandCount_;
	for( int b = 0; b < bandCount_; ++b )
	{
		State& s = bands_[ b ];
		Band&  o = out[ b ];
		o.loHz          = s.loHz;
		o.hiHz          = s.hiHz;
		o.level         = (float)s.level;
		o.db            = (float)dsp::dbFromAmplitude( s.level );
		o.envelope      = (float)s.envelope;
		o.onset         = s.onset;
		o.onsetCount    = s.count;
		o.onsetStrength = (float)s.strength;
		o.lastOnsetTime = s.lastTime;
		s.onset         = false;
	}
}

} // namespace earshot
