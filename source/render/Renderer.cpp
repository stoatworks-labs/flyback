#include "Renderer.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace flyback
{
namespace
{
constexpr float kExtent = 4.5f;

/// 1 / the integral over [-4.5 sigma, 4.5 sigma] of the Gaussian less its
/// value at the edge: erf(4.5/sqrt 2) - 2 * 4.5 * phi(4.5).
float AcrossNorm()
{
	const double e     = kExtent;
	const double inner = std::erf( e / std::sqrt( 2.0 ) );
	const double ped   = 2.0 * e * std::exp( -0.5 * e * e ) / std::sqrt( 2.0 * 3.14159265358979323846 );
	return static_cast< float >( 1.0 / ( inner - ped ) );
}

/// 1 / the integral over [-4.5 sigma, 4.5 sigma] of the Gaussian: the point
/// limit's quad cuts it there too, and without this it loses 6.8e-6.
float PointNorm()
{
	return static_cast< float >( 1.0 / std::erf( kExtent / std::sqrt( 2.0 ) ) );
}

/// Nine taps, sigma 1.5 texels, normalised over all nine.
void BlurWeights( float w[ 5 ] )
{
	const double sigma = 1.5;
	double sum         = 0.0;
	double raw[ 5 ];
	for( int k = 0; k < 5; ++k )
	{
		raw[ k ] = std::exp( -0.5 * k * k / ( sigma * sigma ) );
		sum += k == 0 ? raw[ k ] : 2.0 * raw[ k ];
	}
	for( int k = 0; k < 5; ++k )
		w[ k ] = static_cast< float >( raw[ k ] / sum );
}

void Uniform1fv( const ffglex::FFGLShader& shader, const char* name, int count, const float* values )
{
	glUniform1fv( glGetUniformLocation( shader.GetGLID(), name ), count, values );
}

void Uniform2fv( const ffglex::FFGLShader& shader, const char* name, int count, const float* values )
{
	glUniform2fv( glGetUniformLocation( shader.GetGLID(), name ), count, values );
}

void Uniform4fv( const ffglex::FFGLShader& shader, const char* name, int count, const float* values )
{
	glUniform4fv( glGetUniformLocation( shader.GetGLID(), name ), count, values );
}

void Bind( GLenum unit, GLuint texture )
{
	glActiveTexture( unit );
	glBindTexture( GL_TEXTURE_2D, texture );
}

/// Unbind units 0 .. count-1, by hand, highest first. The host gets its own
/// active unit back from ScopedGLState.
void ReleaseUnits( int count )
{
	for( int unit = count - 1; unit >= 0; --unit )
	{
		glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + unit ) );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
}
} // namespace

bool Renderer::InitGL()
{
	struct
	{
		ffglex::FFGLShader& shader;
		const char* vertex;
		const char* fragment;
		const char* name;
	} stages[] = {
		{ segmentShader, shaders::kSegmentVertex, shaders::kSegmentFragment, "segment" },
		{ decayShader, shaders::kQuadVertex, shaders::kDecayFragment, "decay" },
		{ downShader, shaders::kQuadVertex, shaders::kDownFragment, "down" },
		{ blurShader, shaders::kQuadVertex, shaders::kBlurFragment, "blur" },
		{ lightShader, shaders::kQuadVertex, shaders::kLightFragment, "light" },
		{ displayShader, shaders::kQuadVertex, shaders::kDisplayFragment, "display" },
		{ groundShader, shaders::kQuadVertex, shaders::kGroundFragment, "ground" },
	};
	for( auto& stage : stages )
		if( !stage.shader.Compile( stage.vertex, stage.fragment ) )
		{
			// The failure that actually happens, and from the operator's side
			// it is "the plugin does nothing". Which pass is the only clue.
			diag::error( std::string( "shader failed to compile: " ) + stage.name );
			return false;
		}

	if( !quad.Initialise() )
	{
		diag::error( "the screen quad would not initialise" );
		return false;
	}

	glGenVertexArrays( 1, &segmentVAO );
	glGenBuffers( 1, &segmentVBO );
	glBindVertexArray( segmentVAO );
	glBindBuffer( GL_ARRAY_BUFFER, segmentVBO );
	// Seven floats a segment: the two ends, then joules, radius, thermal. The
	// divisor is VAO state, so it is set with this VAO bound.
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, 7 * sizeof( float ), nullptr );
	glVertexAttribDivisor( 0, 1 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof( float ), reinterpret_cast< const GLvoid* >( 4 * sizeof( float ) ) );
	glVertexAttribDivisor( 1, 1 );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	glGenBuffers( 2, groundPbo );
	width = height = 0;
	return true;
}

void Renderer::DeInitGL()
{
	for( ffglex::FFGLShader* s : { &segmentShader, &decayShader, &downShader, &blurShader, &lightShader, &displayShader, &groundShader } )
		s->FreeGLResources();
	quad.Release();
	emission.Destroy();
	light.Destroy();
	for( PassBuffer& b : levelA )
		b.Destroy();
	for( PassBuffer& b : levelB )
		b.Destroy();
	levelA.clear();
	levelB.clear();
	groundMask.Destroy();
	if( groundPbo[ 0 ] != 0 )
		glDeleteBuffers( 2, groundPbo );
	groundPbo[ 0 ] = groundPbo[ 1 ] = 0;
	groundSize[ 0 ][ 0 ] = groundSize[ 1 ][ 0 ] = 0;
	if( segmentVBO != 0 )
		glDeleteBuffers( 1, &segmentVBO );
	if( segmentVAO != 0 )
		glDeleteVertexArrays( 1, &segmentVAO );
	segmentVBO = segmentVAO = 0;
	width = height = 0;
}

bool Renderer::Ensure( int w, int h )
{
	// The six reference octaves, placed in the frame: octave k of a 1080-line
	// frame is level k + offset here, its weight split between the two levels
	// either side (Renderer.h says why).
	const double offset = std::log2( h / 1080.0 );
	std::fill( levelWeight, levelWeight + 8, 0.0f );
	int wanted = 1;
	for( int k = 0; k < shaders::kGlowLevels; ++k )
	{
		const double x = std::clamp( k + offset, 0.0, 7.0 );
		const int lo   = static_cast< int >( std::floor( x ) );
		const int hi   = std::min( lo + 1, 7 );
		const double f = x - lo;
		levelWeight[ lo ] += static_cast< float >( shaders::kGlowWeights[ k ] * ( 1.0 - f ) );
		levelWeight[ hi ] += static_cast< float >( shaders::kGlowWeights[ k ] * f );
		wanted = std::max( wanted, ( f > 0.0 ? hi : lo ) + 1 );
	}

	bool ok = emission.Ensure( w, h, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	       && light.Ensure( w, h, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( static_cast< int >( levelA.size() ) != wanted )
	{
		for( PassBuffer& b : levelA )
			b.Destroy();
		for( PassBuffer& b : levelB )
			b.Destroy();
		levelA = std::vector< PassBuffer >( static_cast< size_t >( wanted ) );
		levelB = std::vector< PassBuffer >( static_cast< size_t >( wanted ) );
	}
	for( int k = 0; k < wanted && ok; ++k )
	{
		const int lw = std::max( 1, ( w + ( 1 << ( k + 1 ) ) - 1 ) >> ( k + 1 ) );
		const int lh = std::max( 1, ( h + ( 1 << ( k + 1 ) ) - 1 ) >> ( k + 1 ) );
		// 32-bit, not half: a pixel holds joules of order 1e-7, and half float's
		// smallest normal number is 6.1e-5. In half float the halo lost 10 to
		// 50% of its light to the subnormal range -- see AGENTS.md.
		ok = levelA[ static_cast< size_t >( k ) ].Ensure( lw, lh, GL_RGBA32F, PassBuffer::Sampling::Linear, PassBuffer::Wrap::Border )
		  && levelB[ static_cast< size_t >( k ) ].Ensure( lw, lh, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	}
	levels      = wanted;
	floatStores = wanted + 3;// every down to the coarsest, two blurs, the light pass
	if( ok && ( w != width || h != height ) )
	{
		width  = w;
		height = h;
	}
	return ok;
}

bool Renderer::Render( const Frame& frame, const Look& look, GLuint hostFBO, const GLint viewport[ 4 ], GLuint clip,
                       float maxU, float maxV )
{
	// Everything this changes goes back as it was, on every way out.
	ScopedGLState state;
	glDisable( GL_BLEND );

	const int w = std::max( 1, static_cast< int >( viewport[ 2 ] ) );
	const int h = std::max( 1, static_cast< int >( viewport[ 3 ] ) );
	const bool resized = w != width || h != height;
	// Allocate before binding anything: every ffglex Scoped* binding clears to
	// zero on its way out, and PassBuffer::Ensure allocates under one.
	if( !Ensure( w, h ) )
	{
		diag::error( "could not allocate the light buffers" );
		return false;
	}
	if( !segmentShader.IsReady() || !displayShader.IsReady() )
		return false;

	//-----------------------------------------------------------------------
	// Scene to pixels, and back.
	//-----------------------------------------------------------------------
	const double k  = h * look.scale / look.sceneHeight;
	const double c  = std::cos( look.rotation ), sn = std::sin( look.rotation );
	const double ox = h * look.posX + 0.5 * w;
	const double oy = h * look.posY + 0.5 * h;
	const float minSigma = static_cast< float >( 0.8 * std::max( 1.0, h / 1080.0 ) );

	//-----------------------------------------------------------------------
	// 1. Emission: the camera's persistence, then this frame's light.
	//-----------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, emission.GetGLID() );
	glViewport( 0, 0, w, h );
	if( look.clearHistory || resized || look.decay <= 0.0f )
	{
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
	}
	else if( look.decay < 1.0f )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ZERO, GL_CONSTANT_ALPHA );
		glBlendColor( 0.0f, 0.0f, 0.0f, look.decay );
		glUseProgram( decayShader.GetGLID() );
		quad.Draw();
	}

	if( !frame.segments.empty() )
	{
		packed.resize( frame.segments.size() * 7 );
		for( size_t i = 0; i < frame.segments.size(); ++i )
		{
			const Segment& s = frame.segments[ i ];
			float* p         = &packed[ i * 7 ];
			p[ 0 ] = s.x0;
			p[ 1 ] = s.y0;
			p[ 2 ] = s.x1;
			p[ 3 ] = s.y1;
			p[ 4 ] = s.joules;
			p[ 5 ] = s.radius;
			p[ 6 ] = s.thermal;
		}
		glBindBuffer( GL_ARRAY_BUFFER, segmentVBO );
		glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( packed.size() * sizeof( float ) ), packed.data(), GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );

		// Sum, not max: two segments crossing a pixel both put their light
		// there.
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		glUseProgram( segmentShader.GetGLID() );
		segmentShader.Set( "SceneToPixelX", static_cast< float >( k * c ), static_cast< float >( -k * sn ), static_cast< float >( ox ) );
		segmentShader.Set( "SceneToPixelY", static_cast< float >( k * sn ), static_cast< float >( k * c ), static_cast< float >( oy ) );
		segmentShader.Set( "Viewport", static_cast< float >( w ), static_cast< float >( h ) );
		segmentShader.Set( "PixelsPerMetre", static_cast< float >( k ) );
		segmentShader.Set( "MinSigma", minSigma );
		segmentShader.Set( "GasColour", look.gas.r, look.gas.g, look.gas.b );
		segmentShader.Set( "ArcColour", look.arc.r, look.arc.g, look.arc.b );
		segmentShader.Set( "AcrossNorm", AcrossNorm() );
		segmentShader.Set( "PointNorm", PointNorm() );
		glBindVertexArray( segmentVAO );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, static_cast< GLsizei >( frame.segments.size() ) );
		glBindVertexArray( 0 );
	}
	glDisable( GL_BLEND );

	//-----------------------------------------------------------------------
	// 2. The pyramid: sums all the way down, then each level blurred.
	//-----------------------------------------------------------------------
	glUseProgram( downShader.GetGLID() );
	downShader.Set( "Source", 0 );
	for( int lv = 0; lv < levels; ++lv )
	{
		const PassBuffer& from = lv == 0 ? emission : levelA[ static_cast< size_t >( lv - 1 ) ];
		PassBuffer& to         = levelA[ static_cast< size_t >( lv ) ];
		glBindFramebuffer( GL_FRAMEBUFFER, to.GetGLID() );
		glViewport( 0, 0, static_cast< GLsizei >( to.GetWidth() ), static_cast< GLsizei >( to.GetHeight() ) );
		Bind( GL_TEXTURE0, from.TextureID() );
		downShader.Set( "SourceSize", static_cast< float >( from.GetWidth() ), static_cast< float >( from.GetHeight() ) );
		quad.Draw();
	}
	float weights[ 5 ];
	BlurWeights( weights );
	glUseProgram( blurShader.GetGLID() );
	blurShader.Set( "Source", 0 );
	Uniform1fv( blurShader, "Weights", 5, weights );
	for( int lv = 0; lv < levels; ++lv )
	{
		if( levelWeight[ lv ] <= 0.0f && lv + 4 != levels - 1 )
			continue;// neither halo nor the apparatus's light reads it
		PassBuffer& a = levelA[ static_cast< size_t >( lv ) ];
		PassBuffer& b = levelB[ static_cast< size_t >( lv ) ];
		const float lw = static_cast< float >( a.GetWidth() ), lh = static_cast< float >( a.GetHeight() );
		glViewport( 0, 0, static_cast< GLsizei >( lw ), static_cast< GLsizei >( lh ) );
		blurShader.Set( "SourceSize", lw, lh );
		glBindFramebuffer( GL_FRAMEBUFFER, b.GetGLID() );
		Bind( GL_TEXTURE0, a.TextureID() );
		blurShader.Set( "Direction", 1.0f, 0.0f );
		quad.Draw();
		glBindFramebuffer( GL_FRAMEBUFFER, a.GetGLID() );
		Bind( GL_TEXTURE0, b.TextureID() );
		blurShader.Set( "Direction", 0.0f, 1.0f );
		quad.Draw();
	}

	//-----------------------------------------------------------------------
	// 3. Light: the core and the halo.
	//-----------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, light.GetGLID() );
	glViewport( 0, 0, w, h );
	glUseProgram( lightShader.GetGLID() );
	{
		float scale[ 16 ] = {}, share[ 8 ] = {};
		for( int lv = 0; lv < 8; ++lv )
		{
			const int use       = std::min( lv, levels - 1 );
			const PassBuffer& b = levelA[ static_cast< size_t >( use ) ];
			const double block  = static_cast< double >( 1 << ( use + 1 ) );
			scale[ 2 * lv ]     = static_cast< float >( 1.0 / ( block * b.GetWidth() ) );
			scale[ 2 * lv + 1 ] = static_cast< float >( 1.0 / ( block * b.GetHeight() ) );
			share[ lv ]         = lv < levels ? static_cast< float >( levelWeight[ lv ] / ( block * block ) ) : 0.0f;
			Bind( static_cast< GLenum >( GL_TEXTURE1 + lv ), b.TextureID() );
			lightShader.Set( ( "Level" + std::to_string( lv ) ).c_str(), 1 + lv );
		}
		Bind( GL_TEXTURE0, emission.TextureID() );
		lightShader.Set( "Emission", 0 );
		Uniform2fv( lightShader, "LevelScale", 8, scale );
		Uniform1fv( lightShader, "LevelShare", 8, share );
		lightShader.Set( "Glow", std::clamp( look.glow, 0.0f, 1.0f ) );
		quad.Draw();
	}

	//-----------------------------------------------------------------------
	// 4. The picture, into the host's framebuffer.
	//-----------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	glUseProgram( displayShader.GetGLID() );
	{
		// The light falling on the apparatus: a coarse octave, about a
		// twentieth of the frame across at any raster.
		const int ambientLevel     = std::clamp( static_cast< int >( std::lround( 4.0 + std::log2( h / 1080.0 ) ) ), 0, levels - 1 );
		const PassBuffer& ambient  = levelA[ static_cast< size_t >( ambientLevel ) ];
		const double block         = static_cast< double >( 1 << ( ambientLevel + 1 ) );
		Bind( GL_TEXTURE0, light.TextureID() );
		Bind( GL_TEXTURE1, ambient.TextureID() );
		// An unused sampler still wants a real texture behind it.
		Bind( GL_TEXTURE2, clip != 0 ? clip : light.TextureID() );
		displayShader.Set( "Light", 0 );
		displayShader.Set( "Ambient", 1 );
		displayShader.Set( "ClipTexture", 2 );
		displayShader.Set( "AmbientScale", static_cast< float >( 1.0 / ( block * ambient.GetWidth() ) ),
		                   static_cast< float >( 1.0 / ( block * ambient.GetHeight() ) ) );
		displayShader.Set( "AmbientShare", static_cast< float >( 1.0 / ( block * block ) ) );
		displayShader.Set( "ClipMaxUV", maxU, maxV );
		displayShader.Set( "HasClip", clip != 0 ? 1.0f : 0.0f );
		// Joules per pixel to joules per frame-height squared, then the camera.
		displayShader.Set( "Exposure", static_cast< float >( look.camera * static_cast< double >( h ) * h ) );
		displayShader.Set( "Background", look.background[ 0 ], look.background[ 1 ], look.background[ 2 ] );
		displayShader.Set( "Illumination", look.illumination );
		displayShader.Set( "Mix_", look.mix );
		displayShader.Set( "Apparatus", look.apparatus );
		// Pixel to scene: the inverse of the above.
		const double ik = 1.0 / k;
		displayShader.Set( "PixelToSceneX", static_cast< float >( ik * c ), static_cast< float >( ik * sn ),
		                   static_cast< float >( -ik * ( c * ox + sn * oy ) ) );
		displayShader.Set( "PixelToSceneY", static_cast< float >( -ik * sn ), static_cast< float >( ik * c ),
		                   static_cast< float >( ik * ( sn * ox - c * oy ) ) );
		displayShader.Set( "PixelsPerMetre", static_cast< float >( k ) );
		float a[ 64 ] = {}, b[ 64 ] = {};
		const int count = std::min( 16, static_cast< int >( frame.shapes.size() ) );
		for( int i = 0; i < count; ++i )
		{
			const Shape& s = frame.shapes[ static_cast< size_t >( i ) ];
			a[ 4 * i + 0 ] = static_cast< float >( s.kind );
			a[ 4 * i + 1 ] = s.x0;
			a[ 4 * i + 2 ] = s.y0;
			a[ 4 * i + 3 ] = s.x1;
			b[ 4 * i + 0 ] = s.y1;
			b[ 4 * i + 1 ] = s.r;
			b[ 4 * i + 2 ] = s.material;
		}
		displayShader.Set( "ShapeCount", static_cast< float >( count ) );
		Uniform4fv( displayShader, "ShapeA", 16, a );
		Uniform4fv( displayShader, "ShapeB", 16, b );
		quad.Draw();
	}

	glUseProgram( 0 );
	// By hand: the scoped bindings clear rather than restore, and three
	// interleaved units do not unwind (millpond's trap).
	ReleaseUnits( 9 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	return true;
}

bool Renderer::Ground( GLuint clip, float maxU, float maxV, int detect, float threshold, int w, int h,
                       std::vector< uint8_t >& mask )
{
	ScopedGLState state;
	glDisable( GL_BLEND );
	GLint host = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &host );

	if( !groundMask.Ensure( w, h, GL_RGBA8, PassBuffer::Sampling::Nearest ) || !groundShader.IsReady() )
		return false;

	glBindFramebuffer( GL_FRAMEBUFFER, groundMask.GetGLID() );
	glViewport( 0, 0, w, h );
	glUseProgram( groundShader.GetGLID() );
	Bind( GL_TEXTURE0, clip );
	groundShader.Set( "ClipTexture", 0 );
	groundShader.Set( "ClipMaxUV", maxU, maxV );
	groundShader.Set( "MaskSize", static_cast< float >( w ), static_cast< float >( h ) );
	groundShader.Set( "Detect", static_cast< float >( detect ) );
	groundShader.Set( "Threshold", threshold );
	quad.Draw();

	// This frame's mask into one buffer, last frame's out of the other: the
	// readback is asynchronous and nothing here waits on the GPU.
	const int write = groundWrite;
	const int read  = 1 - groundWrite;
	glBindBuffer( GL_PIXEL_PACK_BUFFER, groundPbo[ write ] );
	glBufferData( GL_PIXEL_PACK_BUFFER, static_cast< GLsizeiptr >( w ) * h * 4, nullptr, GL_STREAM_READ );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	groundSize[ write ][ 0 ] = w;
	groundSize[ write ][ 1 ] = h;

	bool got = false;
	if( groundSize[ read ][ 0 ] == w && groundSize[ read ][ 1 ] == h )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, groundPbo[ read ] );
		const uint8_t* data = static_cast< const uint8_t* >(
			glMapBufferRange( GL_PIXEL_PACK_BUFFER, 0, static_cast< GLsizeiptr >( w ) * h * 4, GL_MAP_READ_BIT ) );
		if( data != nullptr )
		{
			mask.resize( static_cast< size_t >( w ) * h );
			for( size_t i = 0; i < mask.size(); ++i )
				mask[ i ] = data[ i * 4 ] > 127 ? 1 : 0;
			glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
			got = true;
		}
	}
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	groundWrite = read;

	glUseProgram( 0 );
	ReleaseUnits( 1 );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( host ) );
	return got;
}
} // namespace flyback
