#include "Shaders.h"

namespace flyback::shaders
{
// Octave weights for the halo, finest first. They sum to one, so the halo
// holds exactly the light Glow sends into it.
const float kGlowWeights[ kGlowLevels ] = { 0.34f, 0.24f, 0.16f, 0.12f, 0.08f, 0.06f };

//---------------------------------------------------------------------------
const char* const kQuadVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// Segments. One instanced quad each; the fragment evaluates the closed form.
//---------------------------------------------------------------------------
const char* const kSegmentVertex = R"(#version 410 core

layout( location = 0 ) in vec4 ends;   //x0 y0 x1 y1, scene metres
layout( location = 1 ) in vec3 carries;//joules of light, luminous radius (m), thermal 0..1

uniform vec3 SceneToPixelX;//pixel x = dot( SceneToPixelX, vec3( scene, 1 ) )
uniform vec3 SceneToPixelY;
uniform vec2 Viewport;     //pixels
uniform float PixelsPerMetre;
uniform float MinSigma;    //pixels
uniform vec3 GasColour;    //energy weights, summing to 1
uniform vec3 ArcColour;    //likewise

flat out float segLength;
flat out float segSigma;
flat out vec3 segEnergy;
out vec2 segUV;

const float Extent = 4.5;

//Not isnan(): a fast-math compiler may fold that to false. A comparison chain
//has no intrinsic to fold.
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

void main()
{
	vec2 a = vec2( dot( SceneToPixelX, vec3( ends.xy, 1.0 ) ), dot( SceneToPixelY, vec3( ends.xy, 1.0 ) ) );
	vec2 b = vec2( dot( SceneToPixelX, vec3( ends.zw, 1.0 ) ), dot( SceneToPixelY, vec3( ends.zw, 1.0 ) ) );

	float sigma  = max( carries.y * PixelsPerMetre, MinSigma );
	float energy = carries.x;
	vec3 weights = mix( GasColour, ArcColour, clamp( carries.z, 0.0, 1.0 ) );

	vec2 delta = b - a;
	float span = length( delta );
	vec2 dir   = span > 1e-6 ? delta / span : vec2( 1.0, 0.0 );
	float len  = span;

	bool ok = usable( a.x ) && usable( a.y ) && usable( b.x ) && usable( b.y ) && usable( energy )
	       && energy > 0.0 && sigma > 0.0;
	if( !ok )
	{
		segLength   = 0.0;
		segSigma    = 1.0;
		segEnergy   = vec3( 0.0 );
		segUV       = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = 0.5 * ( a + b );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength = len;
	segSigma  = sigma;
	segEnergy = energy * weights;
	segUV     = vec2( sx * halfAlong, sy * halfAcross );

	gl_Position = vec4( pos / Viewport * 2.0 - 1.0, 0.0, 1.0 );
}
)";

const char* const kSegmentFragment = R"(#version 410 core

flat in float segLength;
flat in float segSigma;
flat in vec3 segEnergy;
in vec2 segUV;

uniform float AcrossNorm;//1 / the integral of the pedestal-subtracted profile
uniform float PointNorm; //1 / the integral of a Gaussian over +-4.5 sigma

out vec4 deposit;

const float Extent     = 4.5;
const float InvSqrt2Pi = 0.39894228040143268;

//Abramowitz & Stegun 7.1.26: |error| < 1.5e-7.
float erfAS( float x )
{
	float s = x < 0.0 ? -1.0 : 1.0;
	x       = abs( x );
	float t = 1.0 / ( 1.0 + 0.3275911 * x );
	float y = 1.0 - ( ( ( ( ( 1.061405429 * t - 1.453152027 ) * t ) + 1.421413741 ) * t - 0.284496736 ) * t + 0.254829592 ) * t * exp( -x * x );
	return s * y;
}

float ncdf( float x )
{
	return 0.5 * ( 1.0 + erfAS( x * 0.70710678118654752 ) );
}

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across: the normalised Gaussian, less its value at the quad's edge so it
	//meets zero there, renormalised so it still integrates to one.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across         = max( across - pedestal, 0.0 ) * AcrossNorm;

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, where the length cancels: exact, and no difference
		//of two nearly equal CDFs to lose digits in.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv ) * PointNorm;
	}
	else
	{
		float halfLen = 0.5 * segLength;//`half` is reserved
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	deposit = vec4( segEnergy * ( across * along ), 0.0 );
}
)";

//---------------------------------------------------------------------------
// The camera's persistence. Drawn with glBlendFunc( GL_ZERO, GL_CONSTANT_ALPHA )
// and the decay in the blend colour's alpha, so the buffer scales in place.
//---------------------------------------------------------------------------
const char* const kDecayFragment = R"(#version 410 core

out vec4 nothing;

void main()
{
	nothing = vec4( 0.0 );
}
)";

//---------------------------------------------------------------------------
// The glow pyramid.
//---------------------------------------------------------------------------
const char* const kDownFragment = R"(#version 410 core

uniform sampler2D Source;
uniform vec2 SourceSize;

out vec4 total;

//The 2x2 SUM, not the average: every level holds the same joules. A child
//past the edge of an odd-sized level is zero, which is what it holds.
void main()
{
	ivec2 base = ivec2( gl_FragCoord.xy ) * 2;
	ivec2 size = ivec2( SourceSize );
	vec4 t     = vec4( 0.0 );
	for( int dy = 0; dy < 2; ++dy )
		for( int dx = 0; dx < 2; ++dx )
		{
			ivec2 p = base + ivec2( dx, dy );
			if( p.x < size.x && p.y < size.y )
				t += texelFetch( Source, p, 0 );
		}
	total = t;
}
)";

const char* const kBlurFragment = R"(#version 410 core

uniform sampler2D Source;
uniform vec2 SourceSize;
uniform vec2 Direction;   //(1,0) or (0,1)
uniform float Weights[ 5 ];//centre, then each side; they sum to one over all nine

out vec4 blurred;

//Zero outside the level: light blurred past the frame's edge leaves the frame.
void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 d    = ivec2( Direction );
	ivec2 size = ivec2( SourceSize );
	vec4 t     = Weights[ 0 ] * texelFetch( Source, p, 0 );
	for( int k = 1; k < 5; ++k )
	{
		ivec2 a = p + d * k;
		ivec2 b = p - d * k;
		if( a.x < size.x && a.y < size.y )
			t += Weights[ k ] * texelFetch( Source, a, 0 );
		if( b.x >= 0 && b.y >= 0 )
			t += Weights[ k ] * texelFetch( Source, b, 0 );
	}
	blurred = t;
}
)";

const char* const kLightFragment = R"(#version 410 core

uniform sampler2D Emission;
uniform sampler2D Level0;
uniform sampler2D Level1;
uniform sampler2D Level2;
uniform sampler2D Level3;
uniform sampler2D Level4;
uniform sampler2D Level5;
uniform sampler2D Level6;
uniform sampler2D Level7;
uniform vec2 LevelScale[ 8 ]; //fine pixel -> that level's UV
uniform float LevelShare[ 8 ];//the level's share of the halo / 4^(k+1): a level holds sums over that many pixels
uniform float Glow;

out vec4 light;

void main()
{
	vec2 f = gl_FragCoord.xy;
	vec4 e = texelFetch( Emission, ivec2( f ), 0 );
	vec4 g = LevelShare[ 0 ] * texture( Level0, f * LevelScale[ 0 ] )
	       + LevelShare[ 1 ] * texture( Level1, f * LevelScale[ 1 ] )
	       + LevelShare[ 2 ] * texture( Level2, f * LevelScale[ 2 ] )
	       + LevelShare[ 3 ] * texture( Level3, f * LevelScale[ 3 ] )
	       + LevelShare[ 4 ] * texture( Level4, f * LevelScale[ 4 ] )
	       + LevelShare[ 5 ] * texture( Level5, f * LevelScale[ 5 ] )
	       + LevelShare[ 6 ] * texture( Level6, f * LevelScale[ 6 ] )
	       + LevelShare[ 7 ] * texture( Level7, f * LevelScale[ 7 ] );
	light = ( 1.0 - Glow ) * e + Glow * g;
}
)";

//---------------------------------------------------------------------------
// The picture. Everything the operator sees, from the light and the scene.
//---------------------------------------------------------------------------
const char* const kDisplayFragment = R"(#version 410 core

in vec2 uv;

uniform sampler2D Light;       //joules per pixel, linear
uniform sampler2D Ambient;     //a coarse glow level: the light falling on things
uniform vec2 AmbientScale;
uniform float AmbientShare;    //1 / 4^k for that level
uniform sampler2D ClipTexture;
uniform vec2 ClipMaxUV;
uniform float HasClip;

uniform float Exposure;        //display units per joule per pixel
uniform vec3 Background;       //sRGB, the operator's colour
uniform float Illumination;    //Over: how much the flash lights the clip
uniform float Mix_;
uniform float Apparatus;       //0 bare lightning, 1 the machine too
uniform vec3 PixelToSceneX;
uniform vec3 PixelToSceneY;
uniform float PixelsPerMetre;

uniform float ShapeCount;
uniform vec4 ShapeA[ 16 ];//kind, x0, y0, x1
uniform vec4 ShapeB[ 16 ];//y1, r, metal, unused

out vec4 colour;

//A camera's three channels are not three wavelengths: each picks up a little
//of its neighbours'. It is why a violet streamer bright enough to clip reads
//as white at its core on film, and why nothing here can be pure blue.
const mat3 Crosstalk = mat3( 0.86, 0.09, 0.05,
                             0.08, 0.84, 0.08,
                             0.05, 0.09, 0.86 );

vec3 toLinear( vec3 c )
{
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( 0.04045, c ) );
}

vec3 toSrgb( vec3 c )
{
	c = clamp( c, 0.0, 1.0 );
	return mix( c * 12.92, 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055, step( 0.0031308, c ) );
}

float luminance( vec3 c )
{
	return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
}

float boxDistance( vec2 p, vec2 lo, vec2 hi )
{
	vec2 c = 0.5 * ( lo + hi ), e = 0.5 * ( hi - lo );
	vec2 d = abs( p - c ) - e;
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 );
}

void main()
{
	vec2 f       = gl_FragCoord.xy;
	vec3 light   = texelFetch( Light, ivec2( f ), 0 ).rgb * Exposure;
	vec3 ambient = texture( Ambient, f * AmbientScale ).rgb * AmbientShare * Exposure;

	//The film: crosstalk, then exposure to saturation. 1 - exp(-x) is the
	//response of an ideal emulsion, and what lets a hot core clip to white
	//while its halo keeps its colour.
	vec3 shown = 1.0 - exp( -( Crosstalk * light ) );

	vec3 clip = vec3( 0.0 );
	float clipAlpha = 1.0;
	if( HasClip > 0.5 )
	{
		vec4 c    = texture( ClipTexture, uv * ClipMaxUV );
		clip      = toLinear( c.rgb );
		clipAlpha = c.a;
	}

	vec3 base = HasClip > 0.5 ? clip * ( 1.0 + Illumination * luminance( ambient ) ) : toLinear( Background );

	//The apparatus, lit by the discharge. Each shape is shaded as the solid it
	//stands for -- a sphere or a rod round, a coil as a cylinder -- by two
	//lights: a dim room light from the upper left, and the discharge itself,
	//whose glow at this pixel (Ambient) is the light falling here. Polished
	//metal throws a highlight of both back at the camera.
	if( Apparatus > 0.0 )
	{
		vec2 p         = vec2( dot( PixelToSceneX, vec3( f, 1.0 ) ), dot( PixelToSceneY, vec3( f, 1.0 ) ) );
		const vec3 key = normalize( vec3( -0.45, 0.60, 0.66 ) );
		const vec3 hv  = normalize( key + vec3( 0.0, 0.0, 1.0 ) );
		for( int i = 0; i < 16; ++i )
		{
			if( float( i ) >= ShapeCount )
				break;
			vec4 A = ShapeA[ i ];
			vec4 B = ShapeB[ i ];
			float material = B.z;
			float d;
			vec3 n = vec3( 0.0, 0.0, 1.0 );
			if( A.x < 0.5 )
			{
				vec2 a = A.yz, b = vec2( A.w, B.x );
				vec2 pa = p - a, ba = b - a;
				float h = clamp( dot( pa, ba ) / max( dot( ba, ba ), 1e-12 ), 0.0, 1.0 );
				vec2 v  = pa - ba * h;
				d       = length( v ) - B.y;
				vec2 q  = v / max( B.y, 1e-6 );
				n       = vec3( q, sqrt( max( 0.0, 1.0 - dot( q, q ) ) ) );
			}
			else if( A.x < 1.5 )
			{
				vec2 v = p - A.yz;
				d      = length( v ) - B.y;
				vec2 q = v / max( B.y, 1e-6 );
				n      = vec3( q, sqrt( max( 0.0, 1.0 - dot( q, q ) ) ) );
			}
			else if( A.x < 2.5 )
				d = abs( length( p - A.yz ) - B.y ) - 0.5 * A.w;
			else
			{
				d = boxDistance( p, A.yz, vec2( A.w, B.x ) );
				if( material > 1.5 && material < 2.5 )
				{
					//A wound secondary: a cylinder across x.
					float halfWidth = 0.5 * ( A.w - A.y );
					float q    = clamp( ( p.x - 0.5 * ( A.y + A.w ) ) / max( halfWidth, 1e-6 ), -1.0, 1.0 );
					n          = vec3( q, 0.0, sqrt( max( 0.0, 1.0 - q * q ) ) );
				}
			}
			float cover = clamp( 0.5 - d * PixelsPerMetre, 0.0, 1.0 ) * Apparatus;
			if( cover <= 0.0 )
				continue;

			vec3 albedo;
			float shine;
			if( material < 0.5 )
			{
				albedo = vec3( 0.045, 0.045, 0.05 );//a painted base
				shine  = 0.1;
			}
			else if( material < 1.5 )
			{
				albedo = vec3( 0.62, 0.64, 0.68 );//polished aluminium
				shine  = 1.0;
			}
			else if( material < 2.5 )
			{
				//Copper, and its windings: 2 mm wire, faded out where a turn is
				//narrower than a pixel or two, so it never beats against the grid.
				float turns = p.y / 0.002;
				float fade  = 1.0 - smoothstep( 0.25, 0.5, fwidth( turns ) );
				float wire  = 1.0 - fade * 0.28 * ( 1.0 - smoothstep( 0.2, 0.5, abs( fract( turns ) - 0.5 ) * 2.0 ) ) - ( 1.0 - fade ) * 0.14;
				albedo     = vec3( 0.55, 0.30, 0.16 ) * wire;
				shine      = 0.6;
			}
			else if( material < 3.5 )
			{
				albedo = vec3( 0.22, 0.25, 0.30 );//glass
				shine  = 0.8;
			}
			else if( material < 4.5 )
			{
				albedo = vec3( 0.018, 0.018, 0.02 );//the floor
				shine  = 0.0;
			}
			else
			{
				albedo = vec3( 0.03, 0.035, 0.04 );//acrylic, seen edge-lit
				shine  = 0.2;
			}
			float room    = 0.035 * max( dot( n, key ), 0.0 ) + 0.006;
			vec3 fromArc  = ambient * ( 0.35 + 0.65 * n.z ) * 3.0;
			float spec    = pow( max( dot( n, hv ), 0.0 ), 48.0 );
			vec3 lit      = albedo * ( room + fromArc ) + shine * ( 0.06 * spec + 0.9 * spec * fromArc );
			base          = mix( base, lit, cover );
		}
	}

	//Light adds to whatever it falls in front of: the screen of the two.
	vec3 composite = 1.0 - ( 1.0 - base ) * ( 1.0 - shown );
	if( HasClip > 0.5 )
		composite = mix( clip, composite, Mix_ );
	colour = vec4( toSrgb( composite ), HasClip > 0.5 ? clipAlpha : 1.0 );
}
)";

//---------------------------------------------------------------------------
// Over: the clip as ground, on the lattice's resolution. 1 where the clip is
// bright (or opaque, or edged) past the threshold.
//---------------------------------------------------------------------------
const char* const kGroundFragment = R"(#version 410 core

in vec2 uv;

uniform sampler2D ClipTexture;
uniform vec2 ClipMaxUV;
uniform vec2 MaskSize;
uniform float Detect;   //0 luma, 1 alpha, 2 edges
uniform float Threshold;

out vec4 mask;

float lumaAt( vec2 p )
{
	vec3 c = texture( ClipTexture, clamp( p, vec2( 0.0 ), vec2( 1.0 ) ) * ClipMaxUV ).rgb;
	return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
}

void main()
{
	vec2 t = 0.5 / MaskSize;
	float v;
	if( Detect < 0.5 )
		v = 0.25 * ( lumaAt( uv + vec2( -t.x, -t.y ) * 0.5 ) + lumaAt( uv + vec2( t.x, -t.y ) * 0.5 )
		           + lumaAt( uv + vec2( -t.x, t.y ) * 0.5 ) + lumaAt( uv + vec2( t.x, t.y ) * 0.5 ) );
	else if( Detect < 1.5 )
		v = texture( ClipTexture, uv * ClipMaxUV ).a;
	else
	{
		//Sobel on the luma, one mask texel apart.
		vec2 s = 2.0 * t;
		float tl = lumaAt( uv + vec2( -s.x, s.y ) ), tc = lumaAt( uv + vec2( 0.0, s.y ) ), tr = lumaAt( uv + vec2( s.x, s.y ) );
		float ml = lumaAt( uv + vec2( -s.x, 0.0 ) ), mr = lumaAt( uv + vec2( s.x, 0.0 ) );
		float bl = lumaAt( uv + vec2( -s.x, -s.y ) ), bc = lumaAt( uv + vec2( 0.0, -s.y ) ), br = lumaAt( uv + vec2( s.x, -s.y ) );
		float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
		float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );
		v = 0.25 * length( vec2( gx, gy ) );
	}
	mask = vec4( v >= Threshold ? 1.0 : 0.0 );
}
)";
} // namespace flyback::shaders
