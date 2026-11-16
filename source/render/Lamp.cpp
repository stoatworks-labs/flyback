#include "Lamp.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>

namespace flyback
{
namespace
{
/// Coverage of a disc by distance to its centre, one pixel of antialiasing;
/// the bezel is the same at 1.6 radii. Premultiplied out.
const char* const kLampFragment = R"(#version 410 core

uniform vec2 Centre;//window pixels
uniform float Radius;//the lit disc, pixels
uniform vec3 Colour;

out vec4 fragColour;

void main()
{
	float d     = length( gl_FragCoord.xy - Centre );
	float lit   = clamp( Radius - d + 0.5, 0.0, 1.0 );
	float bezel = clamp( 1.6 * Radius - d + 0.5, 0.0, 1.0 );
	fragColour  = vec4( Colour * lit + vec3( 0.03, 0.03, 0.035 ) * ( bezel - lit ), bezel );
}
)";
} // namespace

void LampPainter::Colour( sing::Lamp state, bool blinkOn, float rgb[ 3 ] )
{
	float r = 0.12f, g = 0.12f, b = 0.12f;// unlit
	switch( state )
	{
	case sing::Lamp::Running:
		r = 0.10f;
		g = 0.85f;
		b = 0.25f;
		break;
	case sing::Lamp::Silent:
		r = 0.90f;
		g = 0.08f;
		b = 0.05f;
		break;
	case sing::Lamp::Missing:
		if( blinkOn )
		{
			r = 0.90f;
			g = 0.08f;
			b = 0.05f;
		}
		break;
	case sing::Lamp::Waiting:
		if( blinkOn )
		{
			r = 1.00f;
			g = 0.55f;
			b = 0.05f;
		}
		break;
	case sing::Lamp::Off: break;
	}
	rgb[ 0 ] = r;
	rgb[ 1 ] = g;
	rgb[ 2 ] = b;
}

LampPainter::Placement LampPainter::Place( const GLint viewport[ 4 ] )
{
	// Never smaller than three pixels; 1.2% of the frame's height above 250
	// lines. One radius of margin outside the bezel.
	Placement p;
	p.radius = std::max( 3.0f, 0.012f * static_cast< float >( viewport[ 3 ] ) );
	p.x      = static_cast< float >( viewport[ 0 ] ) + 2.6f * p.radius;
	p.y      = static_cast< float >( viewport[ 1 ] + viewport[ 3 ] ) - 2.6f * p.radius;
	return p;
}

bool LampPainter::Ensure()
{
	if( tried )
		return ready;
	tried = true;
	ready = shader.Compile( shaders::kQuadVertex, kLampFragment ) && quad.Initialise();
	if( !ready )
		diag::error( "the singing lamp's shader failed to compile: the coil sings without it" );
	return ready;
}

bool LampPainter::Draw( GLuint hostFBO, const GLint viewport[ 4 ], sing::Lamp state, bool blinkOn )
{
	last.drawn = false;
	if( state == sing::Lamp::Off || viewport[ 2 ] <= 0 || viewport[ 3 ] <= 0 )
		return true;
	ScopedGLState saved;
	if( !Ensure() )
		return true;
	Placement p = Place( viewport );
	Colour( state, blinkOn, p.rgb );
	const float reach = 1.6f * p.radius + 1.0f;
	const int x0 = std::max( static_cast< int >( viewport[ 0 ] ), static_cast< int >( std::floor( p.x - reach ) ) );
	const int y0 = std::max( static_cast< int >( viewport[ 1 ] ), static_cast< int >( std::floor( p.y - reach ) ) );
	const int x1 = std::min( static_cast< int >( viewport[ 0 ] + viewport[ 2 ] ), static_cast< int >( std::ceil( p.x + reach ) ) );
	const int y1 = std::min( static_cast< int >( viewport[ 1 ] + viewport[ 3 ] ), static_cast< int >( std::ceil( p.y + reach ) ) );
	if( x1 <= x0 || y1 <= y0 )
		return true;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( x0, y0, x1 - x0, y1 - y0 );
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	glUseProgram( shader.GetGLID() );
	shader.Set( "Centre", p.x, p.y );
	shader.Set( "Radius", p.radius );
	shader.Set( "Colour", p.rgb[ 0 ], p.rgb[ 1 ], p.rgb[ 2 ] );
	quad.Draw();
	glUseProgram( 0 );
	p.drawn = true;
	last    = p;
	return true;
}

void LampPainter::DeInitGL()
{
	if( tried )
	{
		shader.FreeGLResources();
		quad.Release();
	}
	tried = ready = false;
	last  = Placement();
}
} // namespace flyback
