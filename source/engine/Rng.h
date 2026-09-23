#pragma once

#include <cstdint>

/**
    Randomness, all of it integer.

    Two generators, for two jobs, and neither of them is `fract( sin( x ) )`:

    - **`Pcg32`**, a stream: O'Neill's PCG-XSH-RR 64/32. The engine draws every
      growth choice from one of these, seeded from the Seed control, so the same
      seed and the same controls grow the same bolt, frame for frame. It is
      exact integer arithmetic, so it gives the same numbers on every CPU and in
      every build.
    - **`Hash`**, a stateless mix (Chris Wellons' lowbias32). The renderer uses
      it for the sub-lattice jitter, keyed by a node's lattice index, so a
      channel's detail belongs to the channel rather than to the order the
      segments happened to be emitted in. The same function is written out in
      GLSL where the shaders need one.

    `Unit()` takes the top 24 bits. That is the widest slice a float32 holds
    without rounding, so the float side of any comparison sees exactly the
    number the integer side made.
*/
namespace flyback
{
class Pcg32
{
public:
	explicit Pcg32( uint64_t seed = 0x853c49e6748fea9bULL, uint64_t stream = 0xda3e39cb94b95bdbULL )
	{
		Seed( seed, stream );
	}

	void Seed( uint64_t seed, uint64_t stream = 0xda3e39cb94b95bdbULL )
	{
		state = 0u;
		inc   = ( stream << 1u ) | 1u;
		Next();
		state += seed;
		Next();
	}

	uint32_t Next()
	{
		const uint64_t old = state;
		state              = old * 6364136223846793005ULL + inc;
		const uint32_t xorshifted = static_cast< uint32_t >( ( ( old >> 18u ) ^ old ) >> 27u );
		const uint32_t rot        = static_cast< uint32_t >( old >> 59u );
		return ( xorshifted >> rot ) | ( xorshifted << ( ( 32u - rot ) & 31u ) );
	}

	/// Uniform in [0, 1), 53 bits. For the growth choice, which is a draw
	/// against a double-precision cumulative weight.
	double Uniform()
	{
		const uint64_t hi = Next() >> 5;// 27 bits
		const uint64_t lo = Next() >> 6;// 26 bits
		return static_cast< double >( ( hi << 26 ) | lo ) * ( 1.0 / 9007199254740992.0 );
	}

private:
	uint64_t state = 0;
	uint64_t inc   = 0;
};

/// lowbias32 (Wellons). Mirrored in GLSL as `hash32` in render/Shaders.cpp.
inline uint32_t Hash( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

inline uint32_t Hash( uint32_t a, uint32_t b )
{
	return Hash( a ^ Hash( b + 0x9e3779b9U ) );
}

/// Top 24 bits into [0, 1): exact in float32.
inline float Unit( uint32_t h )
{
	return static_cast< float >( h >> 8 ) * ( 1.0f / 16777216.0f );
}

/// The same, centred: [-1, 1).
inline float Signed( uint32_t h )
{
	return 2.0f * Unit( h ) - 1.0f;
}
} // namespace flyback
