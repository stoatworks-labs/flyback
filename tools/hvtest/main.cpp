/**
    hvtest -- render Flyback offline, and measure what its discharges do.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic 60 fps clock; and where a claim needs no GPU it
    drives the real engine on its own. A test that exercises a
    reimplementation tests the reimplementation.

        hvtest --out /tmp/f.png          a frame (--machine N, --frames N, --set ...)
        hvtest --list                    every parameter, its default and its display
        hvtest --film N --script cues    N frames, raw RGBA on stdout
        hvtest --pipe                    raw frames in (the effect), raw frames out

    The claims, one flag each, physics first. The physics checks run on the
    engine in metres and seconds, on a lattice sized in metres -- they cannot
    depend on a rasteriser or a raster. The pixel checks (--light, --exposure)
    run at two rasters.

        --laplace      the solver on a coaxial electrode against ln(r/R2)/ln(R1/R2)
        --dimension    eta = 1 clusters have fractal dimension 1.70 +- 0.05; eta = 0
                       is compact, eta = 6 a line, and D falls with eta
        --ladder       the arc goes out at the closed-form L*, climbs at the rise
                       speed, restrikes at the bottom, and a stiffer supply climbs further
        --tesla        bangs are a clock: BPS x 60 in a minute, exactly; streamers
                       lengthen with BPS only while 1/BPS is inside the memory
        --vdg          sparks every C V_b / I_belt; half the belt, twice the wait
        --kirchhoff    current in = current out at every node of every tree
        --light        the frame's light is the events' energy x efficiency, at two
                       rasters, and does not grow with the number of branches
        --exposure     a spark lands in one frame at 360 degrees, one or none at
                       180, never two
        --determinism  the same seed renders the same frames; another does not
        --over         strikes go to a bright disc in the clip, and to the floor
                       when the clip is black
        --onset        the first hit after a clip trigger fires (primed)
        --defaults     preset 1 is the constructor's defaults
        --names        every name fits the host's 16 bytes, and is unique
        --state        the host's GL state comes back as it went in
        --negative     every check above, against a deliberately wrong model
        --bench        GPU ms/frame at 720p, 1080p and 4K; engine CPU ms/frame
*/

#include "Controls.h"
#include "Flyback.h"
#include "Presets.h"
#include "engine/Dbm.h"
#include "engine/Engine.h"
#include "engine/Lattice.h"
#include "engine/Physics.h"
#include "engine/Rng.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace flyback;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kFps = 60.0;

using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 2048 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	std::fflush( stdout );
	if( !condition )
		++g_failures;
}

void Note( const std::string& message )
{
	std::printf( "        %s\n", message.c_str() );
	std::fflush( stdout );
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Deliberate errors, for --negative. Each makes one check score the plugin
// against a model that is wrong by an amount the check claims it can see.
//---------------------------------------------------------------------------
struct Perturb
{
	double electrodeScale = 1.0;  ///< --laplace expects the inner electrode this much bigger
	double growEta        = -1.0; ///< --dimension grows its "eta = 1" clusters at this eta instead
	double ayrtonD        = 1.0;  ///< --ladder expects L* with D this much bigger
	double riseScale      = 1.0;  ///< --ladder expects the arc to climb this much faster
	double extraBangs     = 0.0;  ///< --tesla expects this many more bangs in the minute
	double memorySeconds  = -1.0; ///< --tesla runs its memory claim with this tau
	bool uniformBreakdown = false;///< --vdg expects V_b = 3 MV/m x gap, no enhancement, no Peek
	bool splitCurrents    = false;///< --kirchhoff checks a tree whose every node carries its root's current
	bool lightPerBranch   = false;///< --light expects the light to scale with the number of branches
	bool halfShutterAll   = false;///< --exposure expects every spark to land at 180 degrees too
	bool sameForOtherSeed = false;///< --determinism expects a different seed to render the same
	bool swapGround       = false;///< --over expects the strikes to go to the floor despite the disc
	bool deafOnset        = false;///< --onset runs an analyser that is not primed
};

//---------------------------------------------------------------------------
// PNG, top row first.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is bottom row first, as GL reads it.
bool writePng( const std::string& path, int width, int height, const Bytes& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf size = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( size );
	if( compress2( compressed.data(), &size, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( size );
	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint count              = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &count ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &count ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// A clip for the effect to draw over: a dim scene with a few bright things
/// in it -- a lamp, a window, a figure's edge -- so Over has highlights to
/// strike. Bottom row first.
Floats buildCard( int width, int height )
{
	Floats px( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double r = 0.05 + 0.06 * v, g = 0.06 + 0.05 * v, b = 0.09 + 0.08 * v;
			// A warm lamp, upper right.
			const double dl = std::hypot( ( u - 0.80 ) * width / height, v - 0.70 );
			const double lamp = std::exp( -dl * dl / 0.004 );
			r += 1.1 * lamp;
			g += 0.9 * lamp;
			b += 0.6 * lamp;
			// A cool window, left.
			if( u > 0.08 && u < 0.22 && v > 0.35 && v < 0.75 )
			{
				r += 0.35;
				g += 0.45;
				b += 0.65;
			}
			float* p = &px[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ]   = static_cast< float >( std::min( r, 1.0 ) );
			p[ 1 ]   = static_cast< float >( std::min( g, 1.0 ) );
			p[ 2 ]   = static_cast< float >( std::min( b, 1.0 ) );
			p[ 3 ]   = 1.0f;
		}
	return px;
}

/// Black, with one bright disc at (cx, cy) in frame coordinates, radius r in
/// frame heights. For --over.
Floats discCard( int width, int height, double cx, double cy, double r, bool disc )
{
	Floats px( static_cast< size_t >( width ) * height * 4, 0.0f );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* p = &px[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 3 ]   = 1.0f;
			const double du = ( ( x + 0.5 ) / width - cx ) * width / height, dv = ( y + 0.5 ) / height - cy;
			if( disc && du * du + dv * dv <= r * r )
				p[ 0 ] = p[ 1 ] = p[ 2 ] = 1.0f;
		}
	return px;
}

//---------------------------------------------------------------------------
// The audio the harness feeds: written into the Audio buffer's elements the
// way the host writes them.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses,///< a bass-heavy spectrum with a hit every half second
	Script ///< hits on the frames in `hits`
};

//---------------------------------------------------------------------------
// A rig: the real plugin, a float input texture, an 8-bit output, on a
// synthetic 60 fps clock.
//---------------------------------------------------------------------------
struct Rig
{
	std::unique_ptr< FlybackPlugin > plugin;
	bool effect = false;
	int width = 0, height = 0;
	GLuint clipTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame     = 0;
	double origin = 0.0;
	AudioFeed feed = AudioFeed::Silence;
	std::set< int > hits;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		if( plugin )
			plugin->DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( clipTexture )
			glDeleteTextures( 1, &clipTexture );
	}

	bool Init( int w, int h, bool over, const Floats* clip = nullptr )
	{
		width  = w;
		height = h;
		effect = over;
		plugin = std::make_unique< FlybackPlugin >( over );
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin->InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- the diagnostics log says which shader\n" );
			return false;
		}
		plugin->SetClockScaleForTest( 1.0 );

		glGenTextures( 1, &outputTexture );
		glBindTexture( GL_TEXTURE_2D, outputTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;

		process.HostFBO = outputFBO;
		if( over )
		{
			const Floats card = clip ? *clip : buildCard( w, h );
			clipTexture       = makeTexture( w, h, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
			inputStruct.Handle = clipTexture;
			inputs[ 0 ]        = &inputStruct;
			process.numInputTextures = 1;
			process.inputTextures    = inputs;
		}
		return true;
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, clipTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < PT_COUNT; ++i )
		{
			const char* n = plugin->GetParamName( i );
			if( n != nullptr && name == n )
			{
				plugin->SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	void Set( unsigned int id, float value )
	{
		plugin->SetFloatParameter( id, value );
	}

	void Press( unsigned int id )
	{
		plugin->SetFloatParameter( id, 1.0f );
		plugin->SetFloatParameter( id, 0.0f );
	}

	void Feed( int index )
	{
		double level = 0.0;
		if( feed == AudioFeed::Pulses )
		{
			const double t    = index / kFps;
			const double beat = std::fmod( t, 0.5 );
			level             = 0.15 + 1.5 * std::exp( -beat / 0.06 );
		}
		else if( feed == AudioFeed::Script )
			level = hits.count( index ) ? 1.6 : 0.3;// music already playing, and the hits over it
		for( int bin = 0; bin < kAudioBins; ++bin )
		{
			const float across = static_cast< float >( bin ) / static_cast< float >( kAudioBins - 1 );
			const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
			plugin->SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), static_cast< float >( shape * level ) );
		}
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			plugin->SetTime( origin + frame / kFps );
			Feed( frame );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin->ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Bytes Output() const
	{
		Bytes pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	/// The light buffer, summed: joules, all three channels, after the glow.
	/// Also how much of it is within `border` pixels of the frame's edge.
	/// `emission` sums the buffer before the glow instead.
	double LightSum( double* nearEdge = nullptr, int border = 0, bool emission = false ) const
	{
		Renderer& r = plugin->RendererForTest();
		Floats data( static_cast< size_t >( r.Width() ) * r.Height() * 4 );
		glBindTexture( GL_TEXTURE_2D, emission ? r.EmissionTexture() : r.LightTexture() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		double total = 0.0, edge = 0.0;
		for( int y = 0; y < r.Height(); ++y )
			for( int x = 0; x < r.Width(); ++x )
			{
				const float* p = &data[ ( static_cast< size_t >( y ) * r.Width() + x ) * 4 ];
				const double v = static_cast< double >( p[ 0 ] ) + p[ 1 ] + p[ 2 ];
				total += v;
				if( x < border || y < border || x >= r.Width() - border || y >= r.Height() - border )
					edge += v;
			}
		if( nearEdge )
			*nearEdge = edge;
		return total;
	}
};

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_XPOS: return "xpos";
	case FF_TYPE_YPOS: return "ypos";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	default: return "other";
	}
}

//===========================================================================
// Engine rigs: the engine on its own, at 60 fps, no GL.
//===========================================================================
Settings MachineSettings( Machine m )
{
	float p[ PT_COUNT ] = {};
	FlybackPlugin plugin( false );
	for( unsigned int i = 0; i < PT_COUNT; ++i )
		p[ i ] = plugin.GetFloatParameter( i );
	p[ PT_MACHINE ] = static_cast< float >( m );
	return Resolve( p, 0.0, 16.0 / 9.0 ).engine;
}

void RunEngine( Engine& engine, const Settings& s, double seconds, Frame& frame, double start = 0.0,
                std::function< void( int ) > each = nullptr )
{
	const int frames = static_cast< int >( std::lround( seconds * kFps ) );
	for( int i = 0; i <= frames; ++i )
	{
		engine.Configure( s );
		engine.Advance( start + i / kFps, 1.0 / kFps, frame );
		if( each )
			each( i );
	}
}

//===========================================================================
// --laplace
//===========================================================================
int runLaplace( const Perturb& perturb )
{
	std::printf( "\n=== laplace: the solver on a coaxial electrode, against ln(r/R2)/ln(R1/R2)\n" );
	// Two lattices, so the tolerance is seen to scale with the step and not to
	// be a number that happened to fit one.
	for( int cells : { 96, 192 } )
	{
		Field f;
		f.Reset( 1.0, 1.0, cells, 0.0f );
		const double R1 = 0.08, R2 = 0.45, h = f.Step();
		for( int j = 0; j < f.Ny(); ++j )
			for( int i = 0; i < f.Nx(); ++i )
			{
				const int c = f.Grid().Index( i, j );
				if( f.Kind( c ) == Cell::Wall )
					continue;
				const double r = std::hypot( f.X( i ), f.Y( j ) );
				if( r <= R1 )
					f.PaintSource( c );
				else if( r >= R2 )
					f.PaintGround( c, 0.0f );
			}
		f.GuessAll( 0.5f );
		const float tolerance = 1e-6f;
		const int its         = f.Grid().Solve( tolerance, 1000 );

		// The expected profile, with the electrode where the test claims it is.
		const double r1 = R1 * perturb.electrodeScale;
		const double L  = std::log( r1 / R2 );
		double worst = 0.0, worstRatio = 0.0;
		int n = 0;
		for( int j = 0; j < f.Ny(); ++j )
			for( int i = 0; i < f.Nx(); ++i )
			{
				const int c = f.Grid().Index( i, j );
				if( f.Kind( c ) != Cell::Free )
					continue;
				const double r        = std::hypot( f.X( i ), f.Y( j ) );
				const double expected = std::log( r / R2 ) / L;
				const double err      = std::fabs( f.Grid().Potential( c ) - expected );
				// The tolerance, from the lattice: a staircase electrode's
				// effective radius is uncertain by up to one step h, so the
				// profile is uncertain by h times its sensitivity to each radius,
				//   |dphi/dR1| = |ln(r/R2)| / (R1 L^2),  |dphi/dR2| = |ln(r/R1)| / (R2 L^2),
				// plus the solver's own residual bound (tol x the domain's
				// cell count squared over pi^2 -- far below it here).
				const double tol = h * ( std::fabs( std::log( r / R2 ) ) / R1 + std::fabs( std::log( r / R1 ) ) / R2 ) / ( L * L )
				                 + tolerance * std::pow( cells, 2 ) / ( kPi * kPi );
				worst      = std::max( worst, err );
				worstRatio = std::max( worstRatio, err / tol );
				++n;
			}
		Check( its >= 0, fmt( "%d sites a side (h = %.2f mm): the solve converged to |residual| < %.0e in %d iterations",
		                      f.Nx(), h * 1000.0, tolerance, its ) );
		Check( worstRatio <= 1.0, fmt( "every one of %d free sites within its one-step bound: worst error %.4f, worst %.2f of its bound",
		                               n, worst, worstRatio ) );
	}
	return Verdict();
}

//===========================================================================
// --dimension
//===========================================================================
using Site = std::pair< int, int >;

/// Radius of gyration against mass over the growth history, pooled over the
/// runs: log N = D log Rg + c, fitted from N = total/30 to N = total. The
/// ensemble average is taken before the fit (<Rg^2>(N) over the runs), which
/// is what keeps one lopsided cluster from dragging D about -- a single
/// cluster's own estimate has heavy tails (one of six read 1.89 while the
/// method was chosen).
double PooledDimension( const std::vector< std::vector< Site > >& runs, double* spread = nullptr )
{
	size_t n = 1u << 30;
	for( const auto& r : runs )
		n = std::min( n, r.size() );
	std::vector< double > rg2( n, 0.0 );
	std::vector< double > each;
	for( const auto& r : runs )
	{
		double sx = 0, sy = 0, sxx = 0;
		std::vector< double > xs, ys;
		for( size_t k = 0; k < n; ++k )
		{
			sx += r[ k ].first;
			sy += r[ k ].second;
			sxx += static_cast< double >( r[ k ].first ) * r[ k ].first + static_cast< double >( r[ k ].second ) * r[ k ].second;
			const double m  = static_cast< double >( k + 1 );
			const double mx = sx / m, my = sy / m;
			const double g  = sxx / m - mx * mx - my * my;
			rg2[ k ] += g / runs.size();
			if( k + 1 >= n / 30 && k + 1 >= 50 && ( ( k + 1 ) % std::max< size_t >( 1, n / 60 ) == 0 || k + 1 == n ) )
			{
				xs.push_back( 0.5 * std::log( g ) );
				ys.push_back( std::log( m ) );
			}
		}
		double mx = 0, my = 0;
		for( size_t i = 0; i < xs.size(); ++i )
		{
			mx += xs[ i ];
			my += ys[ i ];
		}
		mx /= xs.size();
		my /= ys.size();
		double sxy = 0, sxx2 = 0;
		for( size_t i = 0; i < xs.size(); ++i )
		{
			sxy += ( xs[ i ] - mx ) * ( ys[ i ] - my );
			sxx2 += ( xs[ i ] - mx ) * ( xs[ i ] - mx );
		}
		each.push_back( sxy / sxx2 );
	}
	std::vector< double > xs, ys;
	for( size_t k = 0; k < n; ++k )
		if( k + 1 >= n / 30 && k + 1 >= 50 && ( ( k + 1 ) % std::max< size_t >( 1, n / 60 ) == 0 || k + 1 == n ) )
		{
			xs.push_back( 0.5 * std::log( rg2[ k ] ) );
			ys.push_back( std::log( static_cast< double >( k + 1 ) ) );
		}
	double mx = 0, my = 0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		mx += xs[ i ];
		my += ys[ i ];
	}
	mx /= xs.size();
	my /= ys.size();
	double sxy = 0, sxx = 0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		sxy += ( xs[ i ] - mx ) * ( ys[ i ] - my );
		sxx += ( xs[ i ] - mx ) * ( xs[ i ] - mx );
	}
	if( spread )
	{
		double m = 0, v = 0;
		for( double d : each )
			m += d / each.size();
		for( double d : each )
			v += ( d - m ) * ( d - m ) / std::max< size_t >( 1, each.size() - 1 );
		*spread = std::sqrt( v );
	}
	return sxy / sxx;
}

/// Box counting, for the record: N(s) boxes of side s, for s = 2 .. L/4.
double BoxDimension( const std::vector< Site >& pts )
{
	int minx = 1 << 30, maxx = -( 1 << 30 ), miny = minx, maxy = maxx;
	for( const Site& p : pts )
	{
		minx = std::min( minx, p.first );
		maxx = std::max( maxx, p.first );
		miny = std::min( miny, p.second );
		maxy = std::max( maxy, p.second );
	}
	const int L = std::max( maxx - minx, maxy - miny ) + 1;
	std::vector< double > xs, ys;
	for( int s = 2; s <= L / 4; s *= 2 )
	{
		std::set< long long > boxes;
		for( const Site& p : pts )
			boxes.insert( static_cast< long long >( ( p.first - minx ) / s ) * 1000003LL + ( p.second - miny ) / s );
		xs.push_back( std::log( 1.0 / s ) );
		ys.push_back( std::log( static_cast< double >( boxes.size() ) ) );
	}
	double mx = 0, my = 0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		mx += xs[ i ];
		my += ys[ i ];
	}
	mx /= xs.size();
	my /= ys.size();
	double sxy = 0, sxx = 0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		sxy += ( xs[ i ] - mx ) * ( ys[ i ] - my );
		sxx += ( xs[ i ] - mx ) * ( xs[ i ] - mx );
	}
	return sxy / sxx;
}

/// Diffusion-limited aggregation by random walkers, on the lattice: the
/// reference whose dimension is known (1.71) and the calibration of the
/// estimator above. Walkers start on a circle outside the cluster, jump when
/// far away, and are relaunched when they wander off.
std::vector< Site > Dla( int n, uint64_t seed )
{
	Pcg32 rng( seed, 0x5bd1e995ULL );
	const int W = 1400, c = W / 2;
	std::vector< uint8_t > occupied( static_cast< size_t >( W ) * W, 0 );
	auto at = [ & ]( int x, int y ) -> uint8_t& { return occupied[ static_cast< size_t >( y ) * W + x ]; };
	at( c, c ) = 1;
	std::vector< Site > pts { { c, c } };
	double rmax = 1.0;
	while( static_cast< int >( pts.size() ) < n )
	{
		const double r0 = rmax + 5.0, a = rng.Uniform() * 2.0 * kPi;
		int x = c + static_cast< int >( std::lround( r0 * std::cos( a ) ) );
		int y = c + static_cast< int >( std::lround( r0 * std::sin( a ) ) );
		for( ;; )
		{
			const double d = std::hypot( x - c, y - c );
			if( d > 3.0 * rmax + 20.0 || d > c - 3 )
				break;
			if( d > rmax + 8.0 )
			{
				const double step = d - rmax - 5.0, b = rng.Uniform() * 2.0 * kPi;
				x += static_cast< int >( std::lround( step * std::cos( b ) ) );
				y += static_cast< int >( std::lround( step * std::sin( b ) ) );
				continue;
			}
			if( at( x + 1, y ) || at( x - 1, y ) || at( x, y + 1 ) || at( x, y - 1 ) )
			{
				at( x, y ) = 1;
				pts.push_back( { x, y } );
				rmax = std::max( rmax, d );
				break;
			}
			switch( rng.Next() & 3u )
			{
			case 0: ++x; break;
			case 1: --x; break;
			case 2: ++y; break;
			default: --y; break;
			}
		}
	}
	return pts;
}

/// A breakdown-model cluster from a point, inside a grounded circle, grown
/// exactly as the engine grows every discharge (GrowthSettings's defaults:
/// eight neighbours, a local relaxation per site, a full solve every 32).
std::vector< Site > Cluster( double eta, int n, uint64_t seed, int cells )
{
	Field f;
	f.Reset( 1.0, 1.0, cells, 0.0f );
	const int source = f.CellAt( 0.0, 0.0 );
	f.PaintSource( source );
	for( int j = 0; j < f.Ny(); ++j )
		for( int i = 0; i < f.Nx(); ++i )
		{
			const int c = f.Grid().Index( i, j );
			if( f.Kind( c ) != Cell::Wall && std::hypot( f.X( i ), f.Y( j ) ) >= 0.48 )
				f.PaintGround( c, 0.0f );
		}
	f.GuessAll( 0.5f );
	Tree t;
	Pcg32 rng( seed );
	GrowthSettings g;
	g.eta    = eta;
	int grown = 0;
	while( grown < n )
	{
		const GrowthResult r = f.Grow( t, std::min( 256, n - grown ), 1e30, rng, 0.0, g );
		grown += r.grown;
		if( r.reachedGround || r.grown == 0 )
			break;
	}
	std::vector< Site > pts { { source % f.Nx(), source / f.Nx() } };
	for( const Node& node : t.Nodes() )
		pts.push_back( { node.cell % f.Nx(), node.cell / f.Nx() } );
	return pts;
}

int runDimension( const Perturb& perturb )
{
	std::printf( "\n=== dimension: breakdown-model clusters against DLA's 1.71\n" );
	constexpr int kSites = 3000, kSeeds = 6, kCells = 320;

	// 1. The estimator, on the reference: lattice DLA, whose dimension is
	// known. This is what licenses the number below -- and what shows box
	// counting cannot be used at this size.
	std::vector< std::vector< Site > > dla;
	double dlaBox = 0.0;
	for( int s = 1; s <= kSeeds; ++s )
	{
		dla.push_back( Dla( kSites, static_cast< uint64_t >( s ) ) );
		dlaBox += BoxDimension( dla.back() ) / kSeeds;
	}
	double dlaSpread   = 0.0;
	const double dDla  = PooledDimension( dla, &dlaSpread );
	const double stErr = dlaSpread / std::sqrt( static_cast< double >( kSeeds ) );
	Check( std::fabs( dDla - 1.71 ) <= 0.05,
	       fmt( "the estimator on %d DLA clusters of %d: D = %.3f (the literature's 1.71; per-cluster spread %.3f)", kSeeds,
	            kSites, dDla, dlaSpread ) );
	Note( fmt( "box counting on the same clusters reads %.3f -- at %d sites it under-reads DLA by %.2f, so it is"
	           " reported and not asserted", dlaBox, kSites, 1.71 - dlaBox ) );

	// 2. The breakdown model at eta = 1: DLA's universality class.
	auto grow = [ & ]( double eta, int sites, int seeds, double* spread, double* box ) {
		std::vector< std::vector< Site > > runs;
		double b = 0.0;
		for( int s = 1; s <= seeds; ++s )
		{
			runs.push_back( Cluster( eta, sites, static_cast< uint64_t >( s ), kCells ) );
			b += BoxDimension( runs.back() ) / seeds;
		}
		if( box )
			*box = b;
		size_t smallest = runs.front().size();
		for( const auto& r : runs )
			smallest = std::min( smallest, r.size() );
		const double d = PooledDimension( runs, spread );
		Note( fmt( "eta %.1f: %d clusters, the smallest %zu sites: D = %.3f", eta, seeds, smallest, d ) );
		return d;
	};
	const auto start = std::chrono::steady_clock::now();
	double spread1 = 0.0, box1 = 0.0;
	const double d1 = grow( perturb.growEta >= 0.0 ? perturb.growEta : 1.0, kSites, kSeeds, &spread1, &box1 );
	// The tolerance is the spec's +-0.05, the literature's band (NPW report
	// 1.75 in 1984, DLA's modern value is 1.71). It is also three standard
	// errors of the mean as measured on the DLA reference above (not on these
	// clusters), which is why six seeds.
	Check( std::fabs( d1 - 1.70 ) <= 0.05,
	       fmt( "eta = 1: D = %.3f, within 1.70 +- 0.05 (per-cluster spread %.3f; 3 SE on DLA = %.3f; box counting %.3f)", d1,
	            spread1, 3.0 * stErr, box1 ) );
	Check( std::fabs( d1 - dDla ) <= 0.05, fmt( "and within 0.05 of the same estimator on DLA (%.3f vs %.3f)", d1, dDla ) );

	// 3. The controls: compact at eta = 0, a line at eta = 6, and between.
	const double d0 = grow( 0.0, 2000, 3, nullptr, nullptr );
	const double dh = grow( 0.5, 2000, 3, nullptr, nullptr );
	const double d2 = grow( 2.0, 2000, 3, nullptr, nullptr );
	const double d6 = grow( 6.0, 2000, 3, nullptr, nullptr );
	Check( d0 > 1.9, fmt( "eta = 0 (Eden growth) is compact: D = %.3f > 1.9", d0 ) );
	Check( d6 < 1.25, fmt( "eta = 6 is a line: D = %.3f < 1.25", d6 ) );
	Check( d0 > dh && dh > d1 && d1 > d2 && d2 > d6,
	       fmt( "D falls as eta rises: %.3f > %.3f > %.3f > %.3f > %.3f at eta 0, 0.5, 1, 2, 6", d0, dh, d1, d2, d6 ) );
	Note( fmt( "%.1f s of growth", std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() ) );
	return Verdict();
}

//===========================================================================
// --ladder
//===========================================================================
int runLadder( const Perturb& perturb )
{
	std::printf( "\n=== ladder: Ayrton's arc on a flyback supply\n" );
	Settings s = MachineSettings( Machine::Ladder );
	physics::Ayrton ayrton;
	ayrton.D *= perturb.ayrtonD;
	// L*, found here by bisection on the discriminant of the load line
	// against Ayrton's equation, written out afresh:
	//   V_oc - I R_s = A + B L + (C + D L) / I  has a real I  iff
	//   (V_oc - A - B L)^2 >= 4 R_s (C + D L).
	// Not the engine's closed form: the mutation test showed that comparing
	// the engine with its own function catches nothing wrong in the function.
	auto arcExists = [ & ]( const physics::Supply& supply, double L ) {
		const double b = supply.openVolts - ayrton.A - ayrton.B * L;
		return b > 0.0 && b * b >= 4.0 * supply.sourceOhms * ( ayrton.C + ayrton.D * L );
	};
	auto bisect = [ & ]( const physics::Supply& supply ) {
		double lo = 0.0, hi = 10.0;
		for( int k = 0; k < 200; ++k )
		{
			const double mid = 0.5 * ( lo + hi );
			( arcExists( supply, mid ) ? lo : hi ) = mid;
		}
		return lo;
	};
	const double lStar = bisect( s.supply );
	Check( std::fabs( lStar - physics::ExtinctionLength( physics::Ayrton(), s.supply ) ) <= 1e-9,
	       fmt( "the closed form agrees with bisection on the discriminant: %.9f m against %.9f m", physics::ExtinctionLength( physics::Ayrton(), s.supply ),
	            lStar ) );
	const double h     = physics::kAirBreakdown > 0.0 ? SceneHeight( Machine::Ladder ) / s.cellsHigh : 0.0;
	const double rise  = s.rise * perturb.riseScale;
	Note( fmt( "supply %.1f kV behind %.2f MOhm: closed-form L* = %.4f m; strike gap %.2f mm; lattice step %.2f mm",
	           s.supply.openVolts / 1000.0, s.supply.sourceOhms / 1e6, lStar, physics::StrikeGap( s.supply ) * 1000.0, h * 1000.0 ) );

	// 1. Straight: roots at the column's own speed, so the arc stays level and
	// its length is the gap, g0 + 2 y tan(alpha/2). The climb to L* then has a
	// closed form too. On rods long enough to reach it (0.6 m, 20 degrees): on
	// the default 0.4 m at 14 degrees a LEVEL arc would run off the top first,
	// which the plugin does too, and which is not what this measures.
	{
		Settings tall  = s;
		tall.rodLength = 0.60;
		tall.rodSpread = 20.0 * kPi / 180.0;
		Engine engine;
		engine.Ladder().rootSpeed = 1.0;
		Frame frame;
		RunEngine( engine, tall, 6.0, frame );
		const LadderProbe& p = engine.Ladder();
		const double g0      = 0.003;
		const double climb   = ( lStar - g0 ) / ( 2.0 * rise * std::tan( 0.5 * tall.rodSpread ) );
		int good = 0, n = 0, timed = 0;
		double worstL = 0.0, worstT = 0.0;
		for( size_t k = 0; k < p.extinctions.size(); ++k )
		{
			const double over = p.lengthsAt[ k ] - lStar;
			worstL            = std::max( worstL, std::fabs( over ) );
			if( over >= -1e-9 && over <= h )
				++good;
			++n;
			const double t = p.extinctions[ k ] - p.strikes[ k ];
			worstT         = std::max( worstT, std::fabs( t - climb ) );
			if( std::fabs( t - climb ) <= 1.0 / kFps )
				++timed;
		}
		Check( n >= 5 && good == n, fmt( "%d of %d extinctions at L* to one lattice step (worst %.2f mm past it)", good, n, worstL * 1000.0 ) );
		Check( n >= 5 && timed == n, fmt( "%d of %d climbs take (L* - g0) / (2 v tan(a/2)) = %.4f s to one frame (worst %.2f ms off)",
		                                   timed, n, climb, worstT * 1000.0 ) );
		int bottom = 0;
		for( double hgt : p.strikeHeights )
			if( std::fabs( hgt ) <= h )
				++bottom;
		Check( bottom == static_cast< int >( p.strikeHeights.size() ),
		       fmt( "all %zu strikes at the bottom of the rods", p.strikeHeights.size() ) );
		int period = 0;
		for( size_t k = 1; k < p.strikes.size() && k < p.extinctions.size() + 1; ++k )
		{
			const double gap  = p.strikes[ k ] - p.strikes[ k - 1 ];
			const double took = p.extinctions[ k - 1 ] - p.strikes[ k - 1 ];
			if( std::fabs( gap - took ) <= 1.0 / kFps )
				++period;
		}
		Check( period >= 5 && period == static_cast< int >( std::min( p.strikes.size() - 1, p.extinctions.size() ) ),
		       fmt( "each restrike follows its extinction: %d periods equal their climb to one frame", period ) );
	}

	// 2. As shipped: the roots lag and the arc bows. On the tall rods it still
	// goes out at L*, sooner; and its apex rises at the rise speed.
	{
		Settings tall  = s;
		tall.rodLength = 0.60;
		tall.rodSpread = 20.0 * kPi / 180.0;
		Engine engine;
		Frame frame;
		std::vector< std::pair< double, double > > apex;
		RunEngine( engine, tall, 6.0, frame, 0.0, [ & ]( int i ) {
			const LadderProbe& p = engine.Ladder();
			if( p.lit && !p.strikes.empty() )
				apex.push_back( { i / kFps - p.strikes.back(), p.apex } );
		} );
		const LadderProbe& p = engine.Ladder();
		int good = 0;
		for( size_t k = 0; k < p.lengthsAt.size(); ++k )
			if( p.lengthsAt[ k ] - lStar >= -1e-9 && p.lengthsAt[ k ] - lStar <= h )
				++good;
		Check( p.lengthsAt.size() >= 5 && good == static_cast< int >( p.lengthsAt.size() ),
		       fmt( "bowed: %d of %zu extinctions at L* to one lattice step", good, p.lengthsAt.size() ) );
		// The apex's height after 0.1 s of climb, in time: it is where the free
		// column put it, v t, to one frame.
		int onTime = 0, samples = 0;
		for( const auto& a : apex )
			if( a.first > 0.05 && a.first < 0.15 )
			{
				++samples;
				const double tExpected = a.second / rise;
				if( std::fabs( tExpected - a.first ) <= 1.0 / kFps )
					++onTime;
			}
		Check( samples > 10 && onTime == samples,
		       fmt( "the apex rises at %.2f m/s: %d of %d samples at height v t to one frame", rise, onTime, samples ) );
		const double straight = ( lStar - 0.003 ) / ( 2.0 * rise * std::tan( 0.5 * tall.rodSpread ) );
		const double took     = p.extinctions.empty() ? 0.0 : p.extinctions[ 0 ] - p.strikes[ 0 ];
		Check( took > 0.0 && took < straight, fmt( "and the bowed arc goes out sooner than the straight one: %.3f s < %.3f s", took, straight ) );
	}

	// 3. The shipped geometry: 0.4 m rods at 14 degrees. Every arc goes out
	// either at L* or by running off the top of the rods -- nothing else ends
	// one -- and those at L* are at it to a step.
	{
		Engine engine;
		Frame frame;
		RunEngine( engine, s, 6.0, frame );
		const LadderProbe& p = engine.Ladder();
		int atStar = 0, atTop = 0, other = 0;
		for( size_t k = 0; k < p.lengthsAt.size(); ++k )
		{
			if( p.overTheTop[ k ] )
				++atTop;
			else if( p.lengthsAt[ k ] - lStar >= -1e-9 && p.lengthsAt[ k ] - lStar <= h )
				++atStar;
			else
				++other;
		}
		Check( p.lengthsAt.size() >= 5 && other == 0,
		       fmt( "default rods (%.2f m, %.0f deg): %zu extinctions, %d at L*, %d off the top, %d anything else", s.rodLength,
		            s.rodSpread * 180.0 / kPi, p.lengthsAt.size(), atStar, atTop, other ) );
	}

	// 4. A softer supply: R_s doubled gives a shorter L*, and the arc goes out
	// lower.
	{
		Settings soft = s;
		soft.supply.sourceOhms *= 2.0;
		const double lSoft = bisect( soft.supply );
		Check( lSoft < lStar, fmt( "R_s doubled: L* %.4f m < %.4f m", lSoft, lStar ) );
		Engine engine;
		engine.Ladder().rootSpeed = 1.0;
		Frame frame;
		RunEngine( engine, soft, 3.0, frame );
		const LadderProbe& p = engine.Ladder();
		Check( !p.lengthsAt.empty() && p.lengthsAt[ 0 ] - lSoft >= -1e-9 && p.lengthsAt[ 0 ] - lSoft <= h,
		       fmt( "and the engine's arc goes out there: %.4f m", p.lengthsAt.empty() ? 0.0 : p.lengthsAt[ 0 ] ) );
	}
	return Verdict();
}

//===========================================================================
// --tesla
//===========================================================================
int runTesla( const Perturb& perturb )
{
	std::printf( "\n=== tesla: the interrupter is a clock, and the channel remembers\n" );
	// 1. The clock.
	for( double bps : { 120.0, 37.5 } )
	{
		Settings s = MachineSettings( Machine::Tesla );
		s.bps      = bps;
		s.reach    = 0.25;// keep the growth cheap: this counts bangs
		Engine engine;
		Frame frame;
		RunEngine( engine, s, 60.0, frame );
		// The first frame anchors the clock; the minute is the 3600 frames after.
		const double expected = bps * 60.0 + perturb.extraBangs;
		Check( std::fabs( static_cast< double >( engine.Events().size() ) - expected ) < 0.5,
		       fmt( "%.1f BPS for 60 s: %zu bangs, %.0f expected", bps, engine.Events().size(), expected ) );
	}

	// 2. Memory. Mean streamer length per bang, over BPS, at tau = 20 ms.
	const double tau = perturb.memorySeconds >= 0.0 ? perturb.memorySeconds : 0.020;
	struct Row
	{
		double bps, mean, err;
	};
	std::vector< Row > rows;
	for( double bps : { 4.0, 8.0, 60.0, 120.0, 240.0 } )
	{
		Settings s = MachineSettings( Machine::Tesla );
		s.bps      = bps;
		s.memory   = tau;
		s.reach    = 0.5;
		s.target   = 0;// streamers into the air: nothing to strike, so nothing ends a streamer early
		Engine engine;
		Frame frame;
		const double seconds = std::max( 4.0, 250.0 / bps );
		RunEngine( engine, s, seconds, frame );
		double sum = 0, sq = 0;
		int n      = 0;
		for( const Event& e : engine.Events() )
		{
			if( e.time < 0.5 )
				continue;// let it settle
			sum += e.length;
			sq += e.length * e.length;
			++n;
		}
		const double mean = sum / n;
		const double sd   = std::sqrt( std::max( 0.0, sq / n - mean * mean ) );
		// Successive bangs share channel, so they are not independent; the
		// error is widened by the square root of the bangs one memory spans.
		const double corr = std::max( 1.0, tau * bps );
		rows.push_back( { bps, mean, sd / std::sqrt( n / corr ) } );
		Note( fmt( "%6.0f BPS (1/BPS = %6.1f ms, tau %.0f ms): mean streamer %.3f m +- %.3f over %d bangs", bps, 1000.0 / bps,
		           tau * 1000.0, mean, rows.back().err, n ) );
	}
	const Row& a = rows[ 0 ];
	const Row& b = rows[ 1 ];
	Check( std::fabs( a.mean - b.mean ) <= 3.0 * std::hypot( a.err, b.err ),
	       fmt( "flat while 1/BPS >> tau: %.3f m at 4 BPS, %.3f m at 8, within 3 standard errors", a.mean, b.mean ) );
	bool rising = true;
	for( size_t k = 2; k < rows.size(); ++k )
		rising = rising && rows[ k ].mean - rows[ k - 1 ].mean > 3.0 * std::hypot( rows[ k ].err, rows[ k - 1 ].err );
	Check( rows[ 2 ].mean - b.mean > 3.0 * std::hypot( rows[ 2 ].err, b.err ) && rising,
	       fmt( "longer with every step in BPS once 1/BPS < tau: %.3f < %.3f < %.3f < %.3f m", b.mean, rows[ 2 ].mean,
	            rows[ 3 ].mean, rows[ 4 ].mean ) );
	return Verdict();
}

//===========================================================================
// --vdg
//===========================================================================
int runVdg( const Perturb& perturb )
{
	std::printf( "\n=== vdg: the belt charges the sphere until the gap breaks\n" );
	// The breakdown model, stated: the two-sphere field by images, Peek's
	// sphere field. The image charges are not trusted: they are checked
	// against the boundary conditions -- the driven sphere's surface at 1 V,
	// the grounded one's at 0, everywhere -- and by uniqueness a set that
	// meets them IS the solution. C, the facing fields and V_b are then worked
	// out here from those charges, and Peek's law is written out afresh.
	const double k0 = 4.0 * kPi * physics::kEpsilon0;
	auto peek = []( double r ) { return 27.2e5 * ( 1.0 + 0.54 / std::sqrt( r * 100.0 ) ); };// V/m, r in m
	struct Expect
	{
		double C, Vb;
		bool ok;
		double worst;
	};
	auto expect = [ & ]( double a, double gap ) {
		const double b = physics::kVdgGroundRatio * a;
		const double sep = a + gap + b;
		const physics::TwoSpheres ts = physics::SolveTwoSpheres( a, b, gap );
		double worst = 0.0;
		for( int n = 0; n < 64; ++n )
		{
			const double th = kPi * n / 63.0;
			for( int sphere = 0; sphere < 2; ++sphere )
			{
				const double R  = sphere == 0 ? a : b;
				const double cx = sphere == 0 ? 0.0 : sep;
				const double px = cx + R * std::cos( th ), py = R * std::sin( th );
				double v = 0.0;
				for( const auto& q : ts.charges )
					v += q.first / ( k0 * std::hypot( px - q.second, py ) );
				worst = std::max( worst, std::fabs( v - ( sphere == 0 ? 1.0 : 0.0 ) ) );
			}
		}
		double C = 0.0, ea = 0.0, eb = 0.0;
		for( const auto& q : ts.charges )
		{
			if( q.second < a )
				C += q.first;
			const double da = a - q.second, db = ( sep - b ) - q.second;
			ea += q.first / ( k0 * da * std::fabs( da ) );
			eb += q.first / ( k0 * db * std::fabs( db ) );
		}
		double vb = std::min( peek( a ) / std::fabs( ea ), peek( b ) / std::fabs( eb ) );
		if( perturb.uniformBreakdown )
			vb = physics::kAirBreakdown * gap;
		return Expect { C, vb, worst < 1e-9, worst };
	};
	{
		const double a = 0.12;
		const physics::TwoSpheres far = physics::SolveTwoSpheres( a, 0.4 * a, 1e4 );
		Check( std::fabs( far.capacitance / ( k0 * a ) - 1.0 ) < 1e-3 && std::fabs( far.fieldDriven * a - 1.0 ) < 1e-3,
		       fmt( "images: a sphere alone has C = 4 pi eps0 a (%.5f of it) and E = V/a (%.5f of it)", far.capacitance / ( k0 * a ),
		            far.fieldDriven * a ) );
	}
	auto interval = [ & ]( const Settings& s ) {
		const Expect e = expect( s.sphere, s.gap );
		return e.C * e.Vb / s.belt;
	};
	{
		const Settings s = MachineSettings( Machine::VanDeGraaff );
		const Expect e   = expect( s.sphere, s.gap );
		Check( e.ok, fmt( "the %zu image charges hold both spheres at their potentials to %.1e V at 128 surface points",
		                  physics::SolveTwoSpheres( s.sphere, physics::kVdgGroundRatio * s.sphere, s.gap ).charges.size(), e.worst ) );
	}

	double first = 0.0;
	for( double belt : { 10e-6, 5e-6 } )
	{
		Settings s = MachineSettings( Machine::VanDeGraaff );
		s.belt     = belt;
		const double T = interval( s );
		Engine engine;
		Frame frame;
		std::vector< int > frames;
		RunEngine( engine, s, 12.0, frame, 0.0, [ & ]( int i ) {
			if( frame.events > 0 && !engine.Events().empty() && engine.Events().back().frame >= 0 )
				frames.push_back( i );
		} );
		const std::vector< Event >& ev = engine.Events();
		double worst = 0.0;
		for( size_t k = 1; k < ev.size(); ++k )
			worst = std::max( worst, std::fabs( ( ev[ k ].time - ev[ k - 1 ].time ) - T ) );
		Check( ev.size() >= 10 && worst <= 1.0 / kFps,
		       fmt( "%.0f uA: %zu sparks, every interval C V_b / I = %.4f s to one frame (worst %.3g s off; V_b %.1f kV, C %.2f pF)",
		            belt * 1e6, ev.size(), T, worst, engine.VdgBreakdown() / 1000.0, engine.VdgCapacitance() * 1e12 ) );
		// And as a camera sees them: the frames they land in.
		int onFrame = 0;
		for( size_t k = 1; k < frames.size(); ++k )
			if( std::fabs( ( frames[ k ] - frames[ k - 1 ] ) - T * kFps ) <= 1.0 )
				++onFrame;
		Check( frames.size() >= 10 && onFrame == static_cast< int >( frames.size() ) - 1,
		       fmt( "and on screen: %d of %zu frame gaps are %.2f frames to one frame", onFrame, frames.size() - 1, T * kFps ) );
		if( first == 0.0 )
			first = ev.size() >= 2 ? ev[ 1 ].time - ev[ 0 ].time : 0.0;
		else
		{
			const double second = ev.size() >= 2 ? ev[ 1 ].time - ev[ 0 ].time : 0.0;
			Check( std::fabs( second - 2.0 * first ) <= 1.0 / kFps,
			       fmt( "half the belt current, twice the interval: %.4f s against %.4f s", second, 2.0 * first ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --kirchhoff
//===========================================================================
int runKirchhoff( const Perturb& perturb )
{
	std::printf( "\n=== kirchhoff: current in = current out, at every node of every tree\n" );
	for( int m = 0; m < static_cast< int >( Machine::Count ); ++m )
	{
		Settings s = MachineSettings( static_cast< Machine >( m ) );
		Engine engine;
		Frame frame;
		double worst = 0.0, spread = 0.0;
		int trees = 0, nodes = 0;
		RunEngine( engine, s, 3.0, frame, 0.0, [ & ]( int ) {
			Tree t = engine.LastEmitted();
			if( t.Empty() )
				return;
			if( perturb.splitCurrents )
			{
				// The wrong model: every node carries its root's full current,
				// as if a branch did not split it.
				t.Currents( 1.0, 20.0 );
				std::vector< Node >& ns = t.Nodes();
				for( Node& n : ns )
					n.current = ns[ static_cast< size_t >( n.root ) ].current;
			}
			worst  = std::max( worst, t.WorstKirchhoff() );
			spread = std::max( spread, t.TipSpread() );
			++trees;
			nodes += static_cast< int >( t.Size() );
		} );
		// Float: a node's current is a float, and its children's sum is
		// accumulated in double from floats, so each child brings at most one
		// float rounding (2^-24 relative); 8 children at most on this lattice.
		const double tol = 8.0 * std::ldexp( 1.0, -24 ) * 2.0;
		Check( trees > 0 && worst <= tol && spread <= tol,
		       fmt( "%-15s %4d trees, %6d nodes: worst |in - out| / in = %.2g, free tips' spread %.2g (bound %.1g)",
		            MachineName( static_cast< Machine >( m ) ), trees, nodes, worst, spread, tol ) );
	}
	return Verdict();
}

//===========================================================================
// --light
//===========================================================================
int runLight( const Perturb& perturb )
{
	std::printf( "\n=== light: the frame holds the events' energy x efficiency, however it branches\n" );
	for( const auto& size : { std::pair< int, int >( 640, 360 ), std::pair< int, int >( 1920, 1080 ) } )
	{
		// A Van de Graaff at half scale: a compact spark in the middle of the
		// frame, so the glow's widest octave stays inside it.
		for( double branching : { 0.85, 0.15 } )
		{
			Rig rig;
			if( !rig.Init( size.first, size.second, false ) )
				return 1;
			rig.Set( PT_MACHINE, static_cast< float >( Machine::VanDeGraaff ) );
			rig.Set( PT_PERSISTENCE, 0.0f );
			rig.Set( PT_SCALE, ParamFromRelative( 0.5, 4.0 ) );
			rig.Set( PT_BRANCHING, branching );
			rig.Set( PT_BELT, ParamFromBelt( 2e-6 ) );// 2 uA: the next natural spark is 1.8 s off, so only the one we fire
			rig.Set( PT_APPARATUS, 0.0f );
			if( !rig.Render( 30 ) )
				return 1;
			// Fire, then read the frame it landed in.
			Engine& engine = rig.plugin->EngineForTest();
			engine.ClearEvents();
			rig.Press( PT_FIRE );
			rig.Render( 1 );
			const std::vector< Event >& ev = engine.Events();
			double expected = 0.0;
			int branches    = 0;
			for( const Event& e : ev )
				if( e.exposed )
				{
					expected += e.light;
					branches += e.branches;
				}
			const Frame& f = rig.plugin->LastFrameForTest();
			// Corona adds its own light every frame: account for it by what the
			// engine says it emitted this frame.
			const double emitted = f.joules;
			double edge          = 0.0;
			const double measured = rig.LightSum( &edge, 2 );
			const double core     = rig.LightSum( nullptr, 0, true );
			const int stores       = rig.plugin->RendererForTest().FloatStores();
			// Tolerance from the arithmetic, not from a run. Summing a profile
			// at pixel centres instead of integrating it errs by the sum of its
			// Fourier transform at the non-zero integers (Poisson summation):
			//  - along a segment, the Gaussian part: 2 exp(-2 pi^2 sigma^2);
			//  - across it, the same, plus the pedestal-subtracted profile's
			//    slope jump J = 4.5 phi(4.5) / sigma^2 where it meets zero at
			//    +-4.5 sigma, whose transform falls as J / (2 pi k)^2 at each of
			//    two edges: summed over k != 0 that is at most J / 6;
			// at the narrowest sigma, 0.8 px. Plus Abramowitz & Stegun's erf,
			// 1.5e-7, and 2^-24 for each float32 store on the way, all taken
			// worst-case in one direction. (The slope term was missed in the
			// first derivation and a re-tuned spark exceeded the bound by it --
			// AGENTS.md.)
			const double sigma = 0.8;
			const double phi45 = std::exp( -0.5 * 4.5 * 4.5 ) / std::sqrt( 2.0 * kPi );
			const double tol   = 2.0 * 2.0 * std::exp( -2.0 * kPi * kPi * sigma * sigma ) + 4.5 * phi45 / ( 6.0 * sigma * sigma )
			                 + 1.5e-7 + ( stores + 1 ) * std::ldexp( 1.0, -24 ) * 16.0;
			const double claim = perturb.lightPerBranch ? expected * std::max( 1, branches ) : expected;
			const double rel   = std::fabs( measured - ( emitted - expected + claim ) ) / std::max( emitted, 1e-30 );
			Check( expected > 0.0 && rel <= tol,
			       fmt( "%4dx%-4d branching %.2f: %d free tips; light %.6g J against %.6g J emitted (spark %.4g J): %.2g of it, "
			            "bound %.2g",
			            size.first, size.second, branching, branches, measured, emitted, expected, rel, tol ) );
			Note( fmt( "before the glow the emission holds %.6g J (%.2g off); within 2 px of the frame's edge: %.2g of the total",
			           core, std::fabs( core - emitted ) / std::max( emitted, 1e-30 ), edge / std::max( measured, 1e-30 ) ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --exposure
//===========================================================================
int runExposure( const Perturb& perturb )
{
	std::printf( "\n=== exposure: a spark is in one frame, or none, never two\n" );
	for( const auto& size : { std::pair< int, int >( 480, 270 ), std::pair< int, int >( 1280, 720 ) } )
		for( double shutter : { 1.0, 0.5 } )
		{
			Rig rig;
			if( !rig.Init( size.first, size.second, false ) )
				return 1;
			rig.Set( PT_MACHINE, static_cast< float >( Machine::Tesla ) );
			rig.Set( PT_PERSISTENCE, 0.0f );
			rig.Set( PT_SHUTTER, static_cast< float >( ( shutter - 0.02 ) / 0.98 ) );
			// 23.7 BPS: bangs at times that never line up with the frames.
			rig.Set( PT_BPS, ParamFromBps( 23.7 ) );
			rig.Set( PT_REACH, 0.2f );
			rig.Set( PT_APPARATUS, 0.0f );
			rig.Set( PT_SCALE, ParamFromRelative( 0.5, 4.0 ) );// every streamer and its halo inside the frame
			Engine& engine = rig.plugin->EngineForTest();
			rig.Render( 2 );
			engine.ClearEvents();
			int litFrames = 0, doubles = 0, framesChecked = 0, lightMismatch = 0;
			std::map< int, double > perFrame;
			for( int i = 0; i < 240; ++i )
			{
				const size_t before = engine.Events().size();
				rig.Render( 1 );
				const double lit = rig.LightSum();
				double expected  = 0.0;
				int landed       = 0;
				for( size_t k = before; k < engine.Events().size(); ++k )
					if( engine.Events()[ k ].exposed )
					{
						expected += engine.Events()[ k ].light;
						++landed;
					}
				if( landed > 0 )
					++litFrames;
				if( landed > 1 )
					++doubles;
				// Every bang's light is in the frame the engine says, and no
				// other: the pixels agree with the clock.
				if( std::fabs( lit - expected ) > 0.01 * std::max( expected, 1e-9 ) + 1e-12 )
					++lightMismatch;
				++framesChecked;
			}
			int bangs = 0, exposed = 0, twice = 0;
			std::map< double, int > seen;
			for( const Event& e : engine.Events() )
			{
				++bangs;
				if( e.exposed )
					++exposed;
				if( ++seen[ e.time ] > 1 )
					++twice;
			}
			const bool full = shutter >= 0.999;
			if( full || perturb.halfShutterAll )
				Check( exposed == bangs && twice == 0 && doubles == 0 && lightMismatch == 0,
				       fmt( "%4dx%-4d %3.0f deg: %d of %d bangs seen, each in one frame; the pixels agree in all %d frames",
				            size.first, size.second, shutter * 360.0, exposed, bangs, framesChecked - lightMismatch ) );
			else
				Check( exposed > 0 && exposed < bangs && twice == 0 && doubles == 0 && lightMismatch == 0,
				       fmt( "%4dx%-4d %3.0f deg: %d of %d bangs seen (the rest fell while it was shut), none twice; the pixels"
				            " agree in all %d frames",
				            size.first, size.second, shutter * 360.0, exposed, bangs, framesChecked - lightMismatch ) );
		}
	return Verdict();
}

//===========================================================================
// --determinism
//===========================================================================
int runDeterminism( const Perturb& perturb )
{
	std::printf( "\n=== determinism: the same seed renders the same frames\n" );
	for( int m : { 1, 0, 3 } )
	{
		auto film = [ & ]( float seed ) {
			Rig rig;
			rig.Init( 480, 270, false );
			rig.Set( PT_MACHINE, static_cast< float >( m ) );
			rig.Set( PT_SEED, seed );
			rig.Render( 90 );
			return rig.Output();
		};
		const Bytes a = film( 0.3f ), b = film( 0.3f ), c = film( 0.7f );
		const bool same    = a == b;
		const bool differs = a != c;
		Check( same && ( perturb.sameForOtherSeed ? !differs : differs ),
		       fmt( "%-15s 90 frames twice with seed 0.3: %s; with seed 0.7: %s", MachineName( static_cast< Machine >( m ) ),
		            same ? "bit-identical" : "DIFFERENT", differs ? "different" : "IDENTICAL" ) );
	}
	return Verdict();
}

//===========================================================================
// --over
//===========================================================================
int runOver( const Perturb& perturb )
{
	std::printf( "\n=== over: the clip is the ground\n" );
	const int w = 640, h = 360;
	const double cx = 0.68, cy = 0.52, r = 0.10;
	for( bool disc : { true, false } )
	{
		const Floats card = discCard( w, h, cx, cy, r, disc );
		Rig rig;
		if( !rig.Init( w, h, true, &card ) )
			return 1;
		rig.Set( PT_MACHINE, static_cast< float >( Machine::Tesla ) );
		rig.Set( PT_TARGET, 1.0f );
		rig.Set( PT_BPS, ParamFromBps( 60.0 ) );
		rig.Set( PT_REACH, 0.75f );
		// A stiffer supply than nominal, so the floor is within a streamer's
		// reach and the black clip's control has strikes to place.
		rig.Set( PT_VOLTAGE, 0.8f );
		Engine& engine = rig.plugin->EngineForTest();
		rig.Render( 3 );
		engine.ClearEvents();
		rig.Render( 600 );
		int strikes = 0, inDisc = 0, onFloor = 0, onRail = 0;
		const double H = SceneHeight( Machine::Tesla );
		// The coil's floor is 0.72 m below the middle of the frame, its base
		// and strike rail 0.62 to 0.68 m below and 0.2 m either side
		// (Engine.cpp); a contact is placed at the centre of the ground site it
		// touched, so within a step of either.
		const double step = H / 136.0;
		for( const Event& e : engine.Events() )
		{
			if( !e.connected )
				continue;
			++strikes;
			// Scene to frame, at the default layout.
			const double fx = ( e.groundX / H ) / ( static_cast< double >( w ) / h ) + 0.5;
			const double fy = e.groundY / H + 0.5;
			const double du = ( fx - cx ) * w / h, dv = fy - cy;
			if( std::hypot( du, dv ) <= r + 2.0 * step / H )
				++inDisc;
			else if( e.groundY <= -0.72 + step )
				++onFloor;
			else if( e.groundY <= -0.62 + step && std::fabs( e.groundX ) <= 0.20 + step )
				++onRail;
		}
		// Chance: the disc's share of the frame's area.
		const double chance = kPi * r * r / ( static_cast< double >( w ) / h );
		const double frac   = strikes > 0 ? static_cast< double >( inDisc ) / strikes : 0.0;
		if( disc != perturb.swapGround )
			Check( strikes >= 20 && frac >= 0.25 && frac >= 10.0 * chance,
			       fmt( "a bright disc in the clip: %d of %d strikes end in it (%.0f%%; chance, its share of the frame, is "
			            "%.1f%%; asked for 10x chance and at least 25%%)",
			            inDisc, strikes, 100.0 * frac, 100.0 * chance ) );
		else
			// "To the floor" in the spec's words: the grounded base standing on
			// it is nearer than the floor and takes them, as a strike rail does.
			Check( strikes >= 20 && inDisc == 0 && onFloor + onRail == strikes,
			       fmt( "a black clip: %d strikes, all to the coil's own grounds (%d to the floor, %d to the strike rail), "
			            "none where the disc was", strikes, onFloor, onRail ) );
	}
	return Verdict();
}

//===========================================================================
// --onset
//===========================================================================
int runOnset( const Perturb& perturb )
{
	std::printf( "\n=== onset: the first hit after a clip trigger fires\n" );
	auto rig = [ & ]( Rig& r ) {
		r.Init( 320, 180, false );
		r.Set( PT_MACHINE, static_cast< float >( Machine::Tesla ) );
		r.Set( PT_BPS, 0.0f );// no clock: only the audio fires
		r.Set( PT_AUDIO_FIRES, 0.6f );
		r.Set( PT_REACH, 0.2f );
		r.plugin->AnalyserForTest().primeFirstFrame = !perturb.deafOnset;
		r.feed = AudioFeed::Script;
	};
	// A clip triggered: a fresh instance, and the first hit a few frames in.
	{
		Rig r;
		rig( r );
		r.hits = { 3 };
		r.Render( 12 );
		int bangs = 0, when = -1;
		for( const Event& e : r.plugin->EngineForTest().Events() )
		{
			++bangs;
			when = e.frame;
		}
		Check( bangs == 1 && when == 3, fmt( "a fresh instance: the hit on frame 3 fires, and only it (%d bang%s, on frame %d)", bangs,
		                                     bangs == 1 ? "" : "s", when ) );
	}
	// The same instance, the clock jumped back to the clip's start.
	{
		Rig r;
		rig( r );
		r.origin = 100.0;
		r.hits   = { 40, 50 };
		r.Render( 30 );
		r.plugin->EngineForTest().ClearEvents();
		r.origin = 50.0 - 30.0 / kFps;// SetTime goes back fifty seconds: a retrigger
		r.Render( 25 );// frames 30 to 54
		int bangs = 0;
		std::string when;
		for( const Event& e : r.plugin->EngineForTest().Events() )
		{
			++bangs;
			when += fmt( " %d", e.frame );
		}
		Check( bangs == 2, fmt( "after the clock jumps back, both hits fire (%d bangs, on frames%s)", bangs, when.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --defaults and --names
//===========================================================================
int runDefaults( const Perturb& )
{
	std::printf( "\n=== defaults: preset 1 is what the constructor sets\n" );
	FlybackPlugin plugin( false );
	int count                  = 0;
	const unsigned int* column = FlybackPlugin::PresetColumnsForTest( count );
	int wrong                  = 0;
	for( int c = 0; c < count; ++c )
	{
		const float want = presets::kPresets[ 0 ].v[ c ];
		const float got  = plugin.GetFloatParameter( column[ c ] );
		if( std::fabs( want - got ) > 1e-4f )
		{
			++wrong;
			Note( fmt( "%s: preset 1 says %.4f, the constructor %.4f", plugin.GetParamName( column[ c ] ), want, got ) );
		}
	}
	Check( wrong == 0, fmt( "all %d preset columns agree with the defaults", count ) );
	// Discrete columns hold whole numbers, or they round to their floor
	// without a word.
	int fractional = 0;
	for( int p = 0; p < presets::kCount; ++p )
		for( int c = 0; c < count; ++c )
		{
			const unsigned int type = plugin.GetParamType( column[ c ] );
			const float v           = presets::kPresets[ p ].v[ c ];
			if( ( type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN ) && v != std::floor( v ) )
				++fractional;
			if( type == FF_TYPE_STANDARD && ( v < 0.0f || v > 1.0f ) )
				++fractional;
		}
	Check( fractional == 0, fmt( "every preset's discrete columns are whole and its sliders in 0..1 (%d presets)", presets::kCount ) );
	// And selecting each preset makes it what the engine runs on.
	for( int p = 1; p <= presets::kCount; ++p )
	{
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( p ) );
		Check( Option( plugin.Effective( PT_MACHINE ), static_cast< int >( Machine::Count ) ) == static_cast< int >( presets::kPresets[ p - 1 ].v[ presets::kMachine ] ),
		       fmt( "preset %d, %s, is the machine the engine runs", p, presets::kPresets[ p - 1 ].name ) );
	}
	return Verdict();
}

int runNames( const Perturb& )
{
	std::printf( "\n=== names: every one fits the host's field, and is unique\n" );
	FlybackPlugin plugin( true );
	std::set< std::string > seen;
	int tooLong = 0, repeated = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string n = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( n.size() > 16 )
		{
			++tooLong;
			Note( fmt( "'%s' is %zu characters", n.c_str(), n.size() ) );
		}
		if( !seen.insert( n ).second )
			++repeated;
	}
	Check( tooLong == 0 && repeated == 0, fmt( "%u parameters: none over 16 characters, none repeated", plugin.GetNumParams() ) );
	for( const char* name : { "SW Flyback", "SW Flyback Over" } )
		Check( std::strlen( name ) <= 16, fmt( "plugin name '%s' is %zu of 16 bytes", name, std::strlen( name ) ) );
	return Verdict();
}

//===========================================================================
// --state
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: what the host hands over is what it gets back\n" );
	for( bool over : { false, true } )
	{
		Rig rig;
		if( !rig.Init( 320, 180, over ) )
			return 1;
		GLuint hostArray = 0;
		glGenVertexArrays( 1, &hostArray );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 3; ++frame )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, 320, 180 );
			glBindVertexArray( hostArray );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE3 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin->SetTime( frame / kFps );
			if( rig.plugin->ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0, pack = 0, arrayBuffer = 0;
			GLfloat clear[ 4 ] = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &pack );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &arrayBuffer );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 2 ] == 320 && viewport[ 3 ] == 180, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( pack == 0 && arrayBuffer == 0, "buffers" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glDeleteVertexArrays( 1, &hostArray );
		Check( problems == 0, fmt( "%s, three frames: viewport, vertex array, program, active unit, framebuffer, blend, scissor, clear "
		                           "colour, buffers and eight texture units as the host left them (%d wrong:%s)",
		                           over ? "effect" : "source", problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --negative
//===========================================================================
int runNegative()
{
	struct Case
	{
		const char* name;
		int ( *check )( const Perturb& );
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, int ( *check )( const Perturb& ), std::function< void( Perturb& ) > set, const char* what ) {
		Perturb p;
		set( p );
		cases.push_back( { name, check, p, what } );
	};
	add( "laplace", runLaplace, []( Perturb& p ) { p.electrodeScale = 1.25; }, "expect the inner electrode 25% bigger" );
	add( "dimension", runDimension, []( Perturb& p ) { p.growEta = 1.5; }, "grow the eta = 1 clusters at eta = 1.5" );
	add( "ladder", runLadder, []( Perturb& p ) { p.ayrtonD = 1.1; }, "expect L* with Ayrton's D 10% larger" );
	add( "ladder", runLadder, []( Perturb& p ) { p.riseScale = 1.2; }, "expect the arc to climb 20% faster" );
	add( "tesla", runTesla, []( Perturb& p ) { p.extraBangs = 1.0; }, "expect one bang more in the minute" );
	add( "tesla", runTesla, []( Perturb& p ) { p.memorySeconds = 1e-5; }, "a coil with no channel memory" );
	add( "vdg", runVdg, []( Perturb& p ) { p.uniformBreakdown = true; }, "expect V_b = 3 MV/m x gap: no images, no Peek" );
	add( "kirchhoff", runKirchhoff, []( Perturb& p ) { p.splitCurrents = true; }, "every node carries its root's current" );
	add( "light", runLight, []( Perturb& p ) { p.lightPerBranch = true; }, "expect the light to scale with the branches" );
	add( "exposure", runExposure, []( Perturb& p ) { p.halfShutterAll = true; }, "expect every bang seen at 180 degrees too" );
	add( "determinism", runDeterminism, []( Perturb& p ) { p.sameForOtherSeed = true; }, "expect another seed to render the same" );
	add( "over", runOver, []( Perturb& p ) { p.swapGround = true; }, "expect the strikes on the floor despite the disc" );
	add( "onset", runOnset, []( Perturb& p ) { p.deafOnset = true; }, "an analyser that is not primed" );

	int undetected = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++undetected;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), undetected );
	std::printf( "\n  %s\n", undetected == 0 ? "PASS" : "FAIL" );
	return undetected == 0 ? 0 : 1;
}

//===========================================================================
// --bench
//===========================================================================
int runBench( const std::vector< std::string >& settings, int only )
{
	std::printf( "\n=== bench: GPU ms/frame through ProcessOpenGL, and the engine's CPU ms/frame\n" );
	std::printf( "  (this machine was shared with other work while it ran: the load average is printed)\n" );
	double load[ 3 ] = {};
	getloadavg( load, 3 );
	std::printf( "  load average %.1f %.1f %.1f\n\n", load[ 0 ], load[ 1 ], load[ 2 ] );
	std::printf( "  %-15s %-6s %9s %9s %9s %9s\n", "machine", "size", "GPU ms", "frame ms", "engine ms", "engine max" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( int m = 0; m < static_cast< int >( Machine::Count ); ++m )
		for( const auto& size : { std::pair< int, int >( 1280, 720 ), std::pair< int, int >( 1920, 1080 ), std::pair< int, int >( 3840, 2160 ) } )
		{
			if( only >= 0 && m != only )
				continue;
			Rig rig;
			if( !rig.Init( size.first, size.second, false ) )
				return 1;
			rig.Set( PT_MACHINE, static_cast< float >( m ) );
			for( const std::string& setting : settings )
			{
				const size_t eq = setting.find( '=' );
				if( eq == std::string::npos || !rig.Set( setting.substr( 0, eq ), std::strtof( setting.substr( eq + 1 ).c_str(), nullptr ) ) )
					return 2;
			}
			rig.Render( 90 );
			glFinish();
			constexpr int kTimed = 120;
			double engineSum = 0.0, engineMax = 0.0, gpuSum = 0.0;
			const auto start = std::chrono::steady_clock::now();
			for( int i = 0; i < kTimed; ++i )
			{
				// The GPU's own clock around the whole ProcessOpenGL: what the
				// passes cost the GPU, whatever the CPU was doing.
				glBeginQuery( GL_TIME_ELAPSED, query );
				rig.Render( 1 );
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				gpuSum += static_cast< double >( ns ) * 1e-6;
				const double e = rig.plugin->EngineForTest().LastMillis();
				engineSum += e;
				engineMax = std::max( engineMax, e );
			}
			glFinish();
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / kTimed;
			std::printf( "  %-15s %4dp  %9.2f %9.2f %9.2f %9.2f\n", MachineName( static_cast< Machine >( m ) ), size.second, gpuSum / kTimed,
			             ms, engineSum / kTimed, engineMax );
		}
	glDeleteQueries( 1, &query );
	std::printf( "\n  GPU ms: GL_TIME_ELAPSED around ProcessOpenGL. frame ms: wall clock per frame, the engine and the query's\n"
	             "  wait included. engine: the CPU engine's own time (Engine::LastMillis), mean and worst of 120 frames.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --script: one `frame  Parameter Name  value` per line, the fleet's format.
// A track is held before its first key and after its last and interpolated
// between: a button press is three keys, `29 Fire 0 / 30 Fire 1 / 31 Fire 0`.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame <= b.first )
		{
			if( b.first == a.first )
				return b.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

int runPipe( int width, int height, bool effect, const std::string& scriptPath, int filmFrames, bool beat,
             const std::vector< std::string >& settings )
{
	Rig rig;
	if( !rig.Init( width, height, effect ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;
	for( const std::string& s : settings )
	{
		const size_t eq = s.find( '=' );
		if( eq == std::string::npos || !rig.Set( s.substr( 0, eq ), std::strtof( s.substr( eq + 1 ).c_str(), nullptr ) ) )
			return 2;
	}
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const auto tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( unsigned int i = 0; i < PT_COUNT; ++i )
				if( rig.plugin->GetParamName( i ) && entry.first == rig.plugin->GetParamName( i ) )
				{
					automation[ i ] = entry.second;
					found           = true;
				}
			if( !found )
			{
				// A misspelling that silently did nothing would film a take that
				// looks deliberate and is wrong.
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}
	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; filmFrames < 0 || index < filmFrames; ++index )
	{
		if( filmFrames < 0 )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < in.size() )
				break;
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		for( const auto& track : automation )
		{
			const float v = valueAt( track.second, index );
			if( track.first == PT_FIRE )
				rig.plugin->SetFloatParameter( track.first, v >= 0.5f ? 1.0f : 0.0f );
			else
				rig.plugin->SetFloatParameter( track.first, v );
		}
		if( !rig.Render( 1 ) )
			return 1;
		const Bytes out = rig.Output();
		Bytes flipped( out.size() );
		for( int y = 0; y < height; ++y )
			std::memcpy( &flipped[ static_cast< size_t >( y ) * width * 4 ], &out[ static_cast< size_t >( height - 1 - y ) * width * 4 ],
			             static_cast< size_t >( width ) * 4 );
		size_t written = 0;
		while( written < flipped.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, flipped.data() + written, flipped.size() - written );
			if( put <= 0 )
				return 1;
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/flyback.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 120;
	bool effect = false, beat = false;
	int machine = -1, preset = -1;
	std::string mode, scriptPath;
	int filmFrames = -1;
	std::vector< int > fireFrames;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		const bool next     = i + 1 < argc;
		if( a == "--help" || a == "-h" )
		{
			std::printf( "hvtest -- render Flyback offline and measure its discharges\n\n"
			             "  --out PATH   --size WxH   --frames N   --machine N   --preset N   --effect\n"
			             "  --fire N (press Fire on frame N; repeatable)   --beat   --set \"Name=V\"\n"
			             "  --list   --film N --script CUES   --pipe\n\n"
			             "  --laplace --dimension --ladder --tesla --vdg --kirchhoff --light --exposure\n"
			             "  --determinism --over --onset --defaults --names --state --negative --bench\n" );
			return 0;
		}
		else if( a == "--out" && next )
			outPath = argv[ ++i ];
		else if( a == "--set" && next )
			settings.push_back( argv[ ++i ] );
		else if( a == "--frames" && next )
			frames = std::atoi( argv[ ++i ] );
		else if( a == "--machine" && next )
			machine = std::atoi( argv[ ++i ] );
		else if( a == "--preset" && next )
			preset = std::atoi( argv[ ++i ] );
		else if( a == "--fire" && next )
			fireFrames.push_back( std::atoi( argv[ ++i ] ) );
		else if( a == "--effect" )
			effect = true;
		else if( a == "--beat" )
			beat = true;
		else if( a == "--pipe" )
			mode = "pipe";
		else if( a == "--film" && next )
		{
			mode       = "pipe";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( a == "--script" && next )
			scriptPath = argv[ ++i ];
		else if( a == "--size" && next )
		{
			const std::string v = argv[ ++i ];
			const size_t x      = v.find( 'x' );
			if( x != std::string::npos )
			{
				width  = std::atoi( v.substr( 0, x ).c_str() );
				height = std::atoi( v.substr( x + 1 ).c_str() );
			}
		}
		else if( a == "--list" || a == "--laplace" || a == "--dimension" || a == "--ladder" || a == "--tesla" || a == "--vdg"
		         || a == "--kirchhoff" || a == "--light" || a == "--exposure" || a == "--determinism" || a == "--over"
		         || a == "--onset" || a == "--defaults" || a == "--names" || a == "--state" || a == "--negative" || a == "--bench" )
			mode = a.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", a.c_str() );
			return 2;
		}
	}
	if( machine >= 0 )
		settings.insert( settings.begin(), "Machine=" + std::to_string( machine ) );
	if( preset >= 0 )
		settings.insert( settings.begin(), "Preset=" + std::to_string( preset ) );

	const Perturb none;
	// No GL needed for these.
	if( mode == "list" )
	{
		FlybackPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %-8s %s\n", "id", "name", "kind", "default", "display" );
		for( unsigned int i = 0; i < PT_COUNT; ++i )
		{
			const char* n = plugin.GetParamName( i );
			std::printf( "%-3u %-18s %-9s %-8.4f %s\n", i, n ? n : "?", kindName( plugin.GetParamType( i ) ), plugin.GetFloatParameter( i ),
			             plugin.GetParamType( i ) == FF_TYPE_BUFFER ? "" : plugin.GetParameterDisplay( i ) );
		}
		return 0;
	}
	if( mode == "laplace" )
		return runLaplace( none );
	if( mode == "dimension" )
		return runDimension( none );
	if( mode == "ladder" )
		return runLadder( none );
	if( mode == "tesla" )
		return runTesla( none );
	if( mode == "vdg" )
		return runVdg( none );
	if( mode == "kirchhoff" )
		return runKirchhoff( none );
	if( mode == "defaults" )
		return runDefaults( none );
	if( mode == "names" )
		return runNames( none );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	int result = 0;
	if( mode == "light" )
		result = runLight( none );
	else if( mode == "exposure" )
		result = runExposure( none );
	else if( mode == "determinism" )
		result = runDeterminism( none );
	else if( mode == "over" )
		result = runOver( none );
	else if( mode == "onset" )
		result = runOnset( none );
	else if( mode == "state" )
		result = runState( none );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench( settings, machine );
	else if( mode == "pipe" )
		result = runPipe( width, height, effect, scriptPath, filmFrames, beat, settings );
	else
	{
		Rig rig;
		if( !rig.Init( width, height, effect ) )
			result = 1;
		else
		{
			for( const std::string& s : settings )
			{
				const size_t eq = s.find( '=' );
				if( eq == std::string::npos || !rig.Set( s.substr( 0, eq ), std::strtof( s.substr( eq + 1 ).c_str(), nullptr ) ) )
					return 2;
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			for( int f = 0; f < std::max( frames, 1 ) && result == 0; ++f )
			{
				if( std::find( fireFrames.begin(), fireFrames.end(), f ) != fireFrames.end() )
					rig.Press( PT_FIRE );
				if( !rig.Render( 1 ) )
					result = 1;
			}
			if( result == 0 )
			{
				if( writePng( outPath, width, height, rig.Output() ) )
					std::printf( "wrote %s -- %dx%d, %d frames, %zu segments, %.4g J of light in the last\n", outPath.c_str(), width,
					             height, frames, rig.plugin->LastFrameForTest().segments.size(), rig.plugin->LastFrameForTest().joules );
				else
					result = 1;
			}
		}
	}
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
