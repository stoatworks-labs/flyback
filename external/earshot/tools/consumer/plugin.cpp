// A stand-in for an FFGL plugin: one Engine per instance, one exported entry.

#include "earshot/earshot.h"

#include <cmath>
#include <vector>

#if defined( _WIN32 )
#define PLUGIN_EXPORT extern "C" __declspec( dllexport )
#else
#define PLUGIN_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#endif

namespace
{
class Instance
{
public:
	Instance() : engine_( earshot::Config() ) {}

	double run()
	{
		// What a plugin with its own source would do; a real one calls
		// selectDevice( name ) instead and never feeds.
		const int            fs = 48000;
		std::vector< float > block( 256 * 2 );
		engine_.feedBegin( fs, 2 );
		double phase = 0.0, t = 0.0;
		for( int b = 0; b < fs / 256; ++b )
		{
			for( int i = 0; i < 256; ++i, phase += 2.0 * 3.14159265358979 * 440.0 / fs )
				block[ (size_t)i * 2 ] = block[ (size_t)i * 2 + 1 ] = (float)( 0.5 * std::sin( phase ) );
			engine_.feed( block.data(), 256 );
			if( b % 3 == 0 )
				engine_.update( t += 1.0 / 60.0, nullptr, 0 );
		}
		const earshot::Frame& f = engine_.update( t + 1.0 / 60.0, nullptr, 0 );
		return f.pitch.voiced ? f.pitch.hz : -1.0;
	}

private:
	earshot::Engine engine_;
};
} // namespace

PLUGIN_EXPORT double fakePluginPitch()
{
	Instance a, b;   // two instances share nothing
	const double x = a.run(), y = b.run();
	return x == y ? x : -2.0;
}
