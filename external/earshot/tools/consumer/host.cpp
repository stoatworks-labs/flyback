// Loads the FakePlugin module the way a host loads a plugin, and asks it for
// the pitch of a 440 Hz sine fed through earshot inside it.

#include <cmath>
#include <cstdio>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int main( int argc, char** argv )
{
	const char* path = argc > 1 ? argv[ 1 ] : PLUGIN_PATH;
#if defined( _WIN32 )
	HMODULE m = LoadLibraryA( path );
	auto    fn = m ? (double ( * )())GetProcAddress( m, "fakePluginPitch" ) : nullptr;
#else
	void* m  = dlopen( path, RTLD_NOW | RTLD_LOCAL );
	auto  fn = m ? (double ( * )())dlsym( m, "fakePluginPitch" ) : nullptr;
#endif
	if( fn == nullptr )
	{
		std::printf( "consumer: could not load %s\n", path );
		return 2;
	}
	const double hz = fn();
	const bool   ok = std::fabs( 1200.0 * std::log2( hz / 440.0 ) ) < 1.0;
	std::printf( "consumer: the module reports %.3f Hz for a 440 Hz sine: %s\n", hz, ok ? "ok" : "FAIL" );
	return ok ? 0 : 1;
}
