/**
 * Flyback — browser demo.
 *
 * High-voltage discharges grown by the dielectric breakdown model: a Jacob's
 * ladder, a Tesla coil, a Van de Graaff, a plasma globe and a Lichtenberg
 * figure. The one idea, from `AGENTS.md`: **air is an insulator until the field
 * tears it, and where it tears is a Laplace problem.** Nothing on the screen is
 * drawn as a shape of lightning; every channel is where the solved field put it.
 *
 * Two halves, and they are not equally faithful:
 *
 *   The GPU half is the plugin's. The nine shader constants below are the nine
 *   `R"( ... )"` bodies in `source/render/Shaders.cpp`, copied across unedited,
 *   and `FlybackRenderer` runs them in the same passes, in the same order, with
 *   the same uniforms as `Renderer::Render` and `Renderer::Ground`.
 *   `demo/tools/check_shaders.py` compares them to the C++ character for
 *   character and `tools/verify.sh` runs it, because two copies of a shader is
 *   exactly the arrangement that drifts.
 *
 *   The CPU half -- the Laplace solve, the growth, the five machines' circuits,
 *   Kirchhoff and the light's accounting, every control's conversion -- is a
 *   hand port in `engine.js`, and **nothing checks it but a reader.** It is a
 *   complete port, not a reduced one: the plugin's lattice sizes, its own
 *   Detail control, all five machines. Where it is not the plugin, `engine.js`
 *   says so at its top, and the page says so in its disclosure.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page.** `SW Flyback` (HV01) is a source and `SW Flyback
 * Over` (HV02) is an effect, and they are one class with a constructor flag
 * (downpour's shape). The kit's `variants` switch picks which one, and the
 * page hides the clip picker while the source is chosen, because a source
 * reads no clip. Switching is a new instance, as it would be in Resolume: the
 * engine and the camera's history start again.
 *
 * **Presets are the plugin's override, not the kit's.** `Preset` is declared
 * as the plugin declares it, and `effective()` in engine.js is
 * `FlybackPlugin::Effective`: while it is on anything but Custom, its row is
 * laid over the panel at read time and those sliders are not the truth. The
 * readouts show what the plugin's `GetParameterDisplay` shows, which is the
 * effective value, so a slider under a preset can read a number it is not set
 * to. That is the plugin's behaviour and the reason is Resolume's.
 *
 * **Fire is an event, shown as a toggle the renderer releases.** The kit has no
 * event type (millpond's and readout's answer). The plugin fires on the rising
 * edge; so does this.
 *
 * **Nothing audio.** `Audio` is an FFT buffer Resolume fills; `Audio Fires` and
 * `Audio Drive` read it. A browser has no Resolume FFT, and asking for a
 * microphone to demonstrate a video source is not a trade worth making, so all
 * three are left off the panel rather than shown dead. The removal is exact:
 * with no spectrum the plugin's analyser reports a level of 0 and never fires,
 * so the drive is x1 and nothing fires by itself -- here as there.
 *
 * **The About block is not a parameter here.** A text line and four link
 * buttons exist so a host has somewhere to put them; a page has links of its
 * own, at the top.
 *
 * **The engine runs synchronously.** The plugin runs it on a worker thread a
 * frame late; the harness's `SetSynchronousForTest` runs it in the frame it is
 * for, and so does this page, on the main thread. A statistics line under the
 * picture says what the engine cost, so a slow frame reads as a slow CPU and
 * not as a broken page.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';
import * as E from './engine.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/render/Shaders.cpp. Do not edit here.
//
// The one backtick inside a comment is escaped, because a template literal has
// nowhere else to put it; check_shaders.py decodes that escape before comparing
// and rejects any other backslash, so the escape cannot hide a difference.
//---------------------------------------------------------------------------

// kQuadVertex, source/render/Shaders.cpp
const QUAD_VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kSegmentVertex, source/render/Shaders.cpp
const SEGMENT_VERTEX = `#version 410 core

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
`;

// kSegmentFragment, source/render/Shaders.cpp
const SEGMENT_FRAGMENT = `#version 410 core

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
		float halfLen = 0.5 * segLength;//\`half\` is reserved
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	deposit = vec4( segEnergy * ( across * along ), 0.0 );
}
`;

// kDecayFragment, source/render/Shaders.cpp
const DECAY_FRAGMENT = `#version 410 core

out vec4 nothing;

void main()
{
	nothing = vec4( 0.0 );
}
`;

// kDownFragment, source/render/Shaders.cpp
const DOWN_FRAGMENT = `#version 410 core

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
`;

// kBlurFragment, source/render/Shaders.cpp
const BLUR_FRAGMENT = `#version 410 core

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
`;

// kLightFragment, source/render/Shaders.cpp
const LIGHT_FRAGMENT = `#version 410 core

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
`;

// kDisplayFragment, source/render/Shaders.cpp
const DISPLAY_FRAGMENT = `#version 410 core

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
			//Over the clip, the clip is the room: no floor of our own over it.
			if( HasClip > 0.5 && material > 3.5 && material < 4.5 )
				continue;
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
`;

// kGroundFragment, source/render/Shaders.cpp
const GROUND_FRAGMENT = `#version 410 core

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
`;

//===========================================================================
// Renderer.cpp, over WebGL2.
//===========================================================================

/** Shaders.cpp's kGlowWeights: the six octaves' shares of the halo. */
const kGlowWeights = [0.34, 0.24, 0.16, 0.12, 0.08, 0.06].map(Math.fround);
const kGlowLevels = 6;
const kExtent = 4.5;

/** erf, for AcrossNorm/PointNorm (std::erf in the C++): A&S 7.1.26 in double. */
function erf(x) {
  const s = x < 0 ? -1 : 1;
  const a = Math.abs(x);
  const t = 1 / (1 + 0.3275911 * a);
  const y = 1 - ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-a * a);
  return s * y;
}

function acrossNorm() {
  const e = kExtent;
  const inner = erf(e / Math.sqrt(2.0));
  const ped = (2.0 * e * Math.exp(-0.5 * e * e)) / Math.sqrt(2.0 * Math.PI);
  return Math.fround(1.0 / (inner - ped));
}

function pointNorm() {
  return Math.fround(1.0 / erf(kExtent / Math.sqrt(2.0)));
}

function blurWeights() {
  const sigma = 1.5;
  const raw = [];
  let sum = 0;
  for (let k = 0; k < 5; k += 1) {
    raw[k] = Math.exp((-0.5 * k * k) / (sigma * sigma));
    sum += k === 0 ? raw[k] : 2 * raw[k];
  }
  return new Float32Array(raw.map((r) => r / sum));
}

class FlybackRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    // The glow pyramid is RGBA32F and the light pass reads it bilinearly. Core
    // on desktop GL; an extension in WebGL2. Without it a LINEAR float texture
    // is incomplete and samples as zero -- no halo and an unlit apparatus, a
    // plausible wrong picture -- so the page refuses instead.
    if (!gl.getExtension('OES_texture_float_linear')) {
      throw new GLError('OES_texture_float_linear is missing. The glow pyramid is 32-bit float and is read bilinearly; without filtering it would silently read as zero.');
    }

    this.segment = new Program(gl, SEGMENT_VERTEX, SEGMENT_FRAGMENT, 'segment', { attribs: { ends: 0, carries: 1 } });
    this.decay = new Program(gl, QUAD_VERTEX, DECAY_FRAGMENT, 'decay');
    this.down = new Program(gl, QUAD_VERTEX, DOWN_FRAGMENT, 'down');
    this.blur = new Program(gl, QUAD_VERTEX, BLUR_FRAGMENT, 'blur');
    this.lightPass = new Program(gl, QUAD_VERTEX, LIGHT_FRAGMENT, 'light');
    this.display = new Program(gl, QUAD_VERTEX, DISPLAY_FRAGMENT, 'display');
    this.groundPass = new Program(gl, QUAD_VERTEX, GROUND_FRAGMENT, 'ground');

    // Seven floats a segment: the two ends, then joules, radius, thermal. One
    // instanced quad each, as Renderer::InitGL sets it up.
    this.vao = gl.createVertexArray();
    this.vbo = gl.createBuffer();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 28, 0);
    gl.vertexAttribDivisor(0, 1);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 28, 16);
    gl.vertexAttribDivisor(1, 1);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);

    this.emission = new PassBuffer(gl, { filter: 'nearest' });
    this.lightBuffer = new PassBuffer(gl, { filter: 'nearest' });
    this.levelA = [];
    this.levelB = [];
    this.levels = 0;
    this.levelWeight = new Float32Array(8);
    this.width = 0;
    this.height = 0;
    this.groundMask = new PassBuffer(gl, { filter: 'nearest' });
    this.groundPending = null;
    this.acrossNorm = acrossNorm();
    this.pointNorm = pointNorm();
    this.weights = blurWeights();

    this.variant = null;
    this.frame = new E.Frame();
    this.stats = { engineMs: 0, segments: 0, events: 0, lattice: [0, 0], machine: 1 };
    this.reset();
  }

  /** A fresh plugin instance: what switching bundle means in a host. */
  reset() {
    this.engine = new E.Engine();
    this.frame = new E.Frame();
    this.lastNow = -1;
    this.firePending = false;
    this.fireHeld = false;
    this.groundMaskData = null;
    this.groundPending = null;
    this.groundStamp = 0;
    this.lastLook = null;
    this.forceClear = true;
  }

  ensure(w, h) {
    const gl = this.gl;
    const offset = Math.log2(h / 1080.0);
    this.levelWeight.fill(0);
    let wanted = 1;
    for (let k = 0; k < kGlowLevels; k += 1) {
      const x = Math.min(7, Math.max(0, k + offset));
      const lo = Math.floor(x);
      const hi = Math.min(lo + 1, 7);
      const f = x - lo;
      this.levelWeight[lo] += kGlowWeights[k] * (1.0 - f);
      this.levelWeight[hi] += kGlowWeights[k] * f;
      wanted = Math.max(wanted, (f > 0.0 ? hi : lo) + 1);
    }
    this.emission.ensure(w, h, gl.RGBA32F);
    this.lightBuffer.ensure(w, h, gl.RGBA32F);
    while (this.levelA.length < wanted) {
      this.levelA.push(new PassBuffer(gl, { filter: 'linear' }));
      this.levelB.push(new PassBuffer(gl, { filter: 'nearest' }));
    }
    for (let k = 0; k < wanted; k += 1) {
      const lw = Math.max(1, (w + (1 << (k + 1)) - 1) >> (k + 1));
      const lh = Math.max(1, (h + (1 << (k + 1)) - 1) >> (k + 1));
      this.levelA[k].ensure(lw, lh, gl.RGBA32F);
      this.levelB[k].ensure(lw, lh, gl.RGBA32F);
    }
    this.levels = wanted;
  }

  /**
   * Renderer::Ground: the clip thresholded onto a small mask, handed back a
   * frame late. The plugin reads it through a two-PBO ring; WebGL2 reads it
   * with a synchronous readPixels, and this keeps the frame of latency.
   */
  ground(clipTexture, detect, threshold, mw, mh) {
    const gl = this.gl;
    this.groundMask.ensure(mw, mh, gl.RGBA8);
    this.groundMask.bind();
    gl.disable(gl.BLEND);
    this.groundPass.use();
    bindTexture(gl, 0, clipTexture);
    this.groundPass.setSampler('ClipTexture', 0);
    this.groundPass.set('ClipMaxUV', 1.0, 1.0);
    this.groundPass.set('MaskSize', mw, mh);
    this.groundPass.set('Detect', detect);
    this.groundPass.set('Threshold', threshold);
    this.quad.draw();

    const pixels = new Uint8Array(mw * mh * 4);
    gl.readPixels(0, 0, mw, mh, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    const previous = this.groundPending;
    this.groundPending = { pixels, w: mw, h: mh };
    if (!previous || previous.w !== mw || previous.h !== mh) return null;
    const mask = new Uint8Array(mw * mh);
    for (let i = 0; i < mask.length; i += 1) mask[i] = previous.pixels[i * 4] > 127 ? 1 : 0;
    return mask;
  }

  /** Renderer::Render. `clipTexture` is null for the source. */
  draw(frame, look, w, h, clipTexture) {
    const gl = this.gl;
    gl.disable(gl.BLEND);
    const resized = w !== this.width || h !== this.height;
    this.ensure(w, h);
    this.width = w;
    this.height = h;

    const k = (h * look.scale) / look.sceneHeight;
    const c = Math.cos(look.rotation), sn = Math.sin(look.rotation);
    const ox = h * look.posX + 0.5 * w;
    const oy = h * look.posY + 0.5 * h;
    const minSigma = 0.8 * Math.max(1.0, h / 1080.0);

    // 1. Emission: the camera's persistence, then this frame's light.
    this.emission.bind();
    if (look.clearHistory || resized || look.decay <= 0.0) {
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
    } else if (look.decay < 1.0) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ZERO, gl.CONSTANT_ALPHA);
      gl.blendColor(0, 0, 0, look.decay);
      this.decay.use();
      this.quad.draw();
    }

    const count = frame.segmentCount;
    if (count > 0) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(frame.segments), gl.STREAM_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, null);

      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE);
      const s = this.segment.use();
      s.set('SceneToPixelX', k * c, -k * sn, ox);
      s.set('SceneToPixelY', k * sn, k * c, oy);
      s.set('Viewport', w, h);
      s.set('PixelsPerMetre', k);
      s.set('MinSigma', minSigma);
      s.set('GasColour', look.gas.r, look.gas.g, look.gas.b);
      s.set('ArcColour', look.arc.r, look.arc.g, look.arc.b);
      s.set('AcrossNorm', this.acrossNorm);
      s.set('PointNorm', this.pointNorm);
      gl.bindVertexArray(this.vao);
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
      gl.bindVertexArray(null);
    }
    gl.disable(gl.BLEND);

    // 2. The pyramid: sums all the way down, then each level blurred.
    const down = this.down.use();
    down.setSampler('Source', 0);
    for (let lv = 0; lv < this.levels; lv += 1) {
      const from = lv === 0 ? this.emission : this.levelA[lv - 1];
      const to = this.levelA[lv];
      to.bind();
      bindTexture(gl, 0, from.texture);
      down.set('SourceSize', from.width, from.height);
      this.quad.draw();
    }
    const blur = this.blur.use();
    blur.setSampler('Source', 0);
    blur.setArray('Weights', this.weights, 1);
    for (let lv = 0; lv < this.levels; lv += 1) {
      if (this.levelWeight[lv] <= 0.0 && lv + 4 !== this.levels - 1) continue;
      const a = this.levelA[lv], b = this.levelB[lv];
      blur.set('SourceSize', a.width, a.height);
      b.bind();
      bindTexture(gl, 0, a.texture);
      blur.set('Direction', 1.0, 0.0);
      this.quad.draw();
      a.bind();
      bindTexture(gl, 0, b.texture);
      blur.set('Direction', 0.0, 1.0);
      this.quad.draw();
    }

    // 3. Light: the core and the halo.
    this.lightBuffer.bind();
    const light = this.lightPass.use();
    const scale = new Float32Array(16), share = new Float32Array(8);
    for (let lv = 0; lv < 8; lv += 1) {
      const use = Math.min(lv, this.levels - 1);
      const b = this.levelA[use];
      const block = 1 << (use + 1);
      scale[2 * lv] = 1.0 / (block * b.width);
      scale[2 * lv + 1] = 1.0 / (block * b.height);
      share[lv] = lv < this.levels ? this.levelWeight[lv] / (block * block) : 0.0;
      bindTexture(gl, 1 + lv, b.texture);
      light.setSampler(`Level${lv}`, 1 + lv);
    }
    bindTexture(gl, 0, this.emission.texture);
    light.setSampler('Emission', 0);
    gl.uniform2fv(light.location('LevelScale[0]') ?? light.location('LevelScale'), scale);
    light.setArray('LevelShare', share, 1);
    light.set('Glow', Math.min(1, Math.max(0, look.glow)));
    this.quad.draw();

    // 4. The picture.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, w, h);
    const d = this.display.use();
    const ambientLevel = Math.min(this.levels - 1, Math.max(0, E.lround(4.0 + Math.log2(h / 1080.0))));
    const ambient = this.levelA[ambientLevel];
    const block = 1 << (ambientLevel + 1);
    bindTexture(gl, 0, this.lightBuffer.texture);
    bindTexture(gl, 1, ambient.texture);
    bindTexture(gl, 2, clipTexture ?? this.lightBuffer.texture);
    d.setSampler('Light', 0);
    d.setSampler('Ambient', 1);
    d.setSampler('ClipTexture', 2);
    d.set('AmbientScale', 1.0 / (block * ambient.width), 1.0 / (block * ambient.height));
    d.set('AmbientShare', 1.0 / (block * block));
    d.set('ClipMaxUV', 1.0, 1.0);
    d.set('HasClip', clipTexture ? 1.0 : 0.0);
    d.set('Exposure', look.camera * h * h);
    d.set('Background', look.background[0], look.background[1], look.background[2]);
    d.set('Illumination', look.illumination);
    d.set('Mix_', look.mix);
    d.set('Apparatus', look.apparatus);
    const ik = 1.0 / k;
    d.set('PixelToSceneX', ik * c, ik * sn, -ik * (c * ox + sn * oy));
    d.set('PixelToSceneY', -ik * sn, ik * c, ik * (sn * ox - c * oy));
    d.set('PixelsPerMetre', k);
    const A = new Float32Array(64), B = new Float32Array(64);
    const shapes = Math.min(16, frame.shapes.length);
    for (let i = 0; i < shapes; i += 1) {
      const sh = frame.shapes[i];
      A[4 * i] = sh.kind; A[4 * i + 1] = sh.x0; A[4 * i + 2] = sh.y0; A[4 * i + 3] = sh.x1;
      B[4 * i] = sh.y1; B[4 * i + 1] = sh.r; B[4 * i + 2] = sh.material;
    }
    d.set('ShapeCount', shapes);
    d.setArray('ShapeA', A, 4);
    d.setArray('ShapeB', B, 4);
    this.quad.draw();

    for (let unit = 8; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
  }

  /**
   * FlybackPlugin::ProcessOpenGL, synchronously: the clock, the params through
   * the preset override, Over's ground mask, the engine, the picture.
   */
  render({ input, params, width, height, time, variant }) {
    if (variant !== this.variant) {
      this.variant = variant;
      this.reset();
    }
    const over = variant === 'over';
    const aspect = width / height;

    // Fire: an event, taken on its rising edge and released here.
    if (params.get('fire') > 0.5) {
      if (!this.fireHeld) this.firePending = true;
      this.fireHeld = true;
      params.set('fire', 0);
    } else {
      this.fireHeld = false;
    }

    const eff = effectiveParams(params);
    const r = E.resolve(eff, 0.0, aspect);

    // Paused, and a control moved: the host would not render the same instant
    // twice, so the engine is not stepped. The last frame is drawn again with
    // the new look and without its persistence trail; engine controls take
    // effect on Play or Step.
    const still = this.lastNow >= 0 && time === this.lastNow;

    let period = this.lastNow >= 0 ? time - this.lastNow : 1.0 / 60.0;
    const jumped = this.lastNow >= 0 && (period < 0.0 || period > 0.5);
    if (jumped || period <= 0.0) period = 1.0 / 60.0;
    period = Math.min(0.25, Math.max(1.0 / 240.0, period));

    if (over && !still) {
      const mw = 160;
      const mh = Math.max(16, E.lround(mw / aspect));
      const mask = this.ground(input.texture, r.detect, r.threshold, mw, mh);
      if (mask) {
        this.groundStamp += 1;
        this.groundMaskData = { mask, mw, mh };
      }
    }
    if (over && this.groundMaskData) {
      r.engine.clip = this.groundMaskData.mask;
      r.engine.clipW = this.groundMaskData.mw;
      r.engine.clipH = this.groundMaskData.mh;
      r.engine.clipStamp = this.groundStamp;
    }

    r.look.decay = r.persistenceSeconds > 0.0 ? Math.fround(Math.exp(-period / r.persistenceSeconds)) : 0.0;
    r.look.clearHistory = jumped || this.forceClear;
    this.forceClear = false;

    if (still) {
      r.look.clearHistory = true;
    } else {
      this.engine.configure(r.engine);
      if (this.firePending) this.engine.fire();
      this.firePending = false;
      this.engine.advance(time, period, this.frame);
      this.lastNow = time;
    }

    this.draw(this.frame, r.look, width, height, over ? input.texture : null);

    this.stats.engineMs = still ? this.stats.engineMs : this.engine.lastMillis;
    this.stats.segments = this.frame.segmentCount;
    this.stats.events = this.frame.events;
    this.stats.lattice = this.engine.lattice();
    this.stats.machine = r.engine.machine;
    this.stats.frames = (this.stats.frames ?? 0) + 1;
  }
}

//===========================================================================
// The parameters, in the constructor's order, groups, names and defaults
// (FlybackPlugin::Declare).
//===========================================================================

/** Page id -> the plugin's ParamId. Audio and About are absent (see above). */
const IDS = {
  preset: 'PRESET', machine: 'MACHINE', fire: 'FIRE', seed: 'SEED',
  supply: 'SUPPLY', voltage: 'VOLTAGE', impedance: 'IMPEDANCE',
  branching: 'BRANCHING', detail: 'DETAIL', memory: 'MEMORY', reach: 'REACH',
  rodSpread: 'ROD_SPREAD', rodLength: 'ROD_LENGTH', rise: 'RISE', wind: 'WIND',
  bps: 'BPS', topload: 'TOPLOAD', target: 'TARGET', targetX: 'TARGET_X', targetY: 'TARGET_Y',
  belt: 'BELT', sphere: 'SPHERE', gap: 'GAP', finish: 'FINISH',
  globe: 'GLOBE', finger: 'FINGER', fingerX: 'FINGER_X', fingerY: 'FINGER_Y',
  origin: 'ORIGIN',
  efficiency: 'EFFICIENCY', glow: 'GLOW', gas: 'GAS', shutter: 'SHUTTER', persistence: 'PERSISTENCE',
  apparatus: 'APPARATUS', backR: 'BACK_R', backG: 'BACK_G', backB: 'BACK_B',
  detect: 'DETECT', threshold: 'THRESHOLD', illumination: 'ILLUMINATION', mix: 'MIX',
  posX: 'POS_X', posY: 'POS_Y', scale: 'SCALE', rotation: 'ROTATION',
};

/** The raw params as the plugin's float array, then Effective() over them. */
function effectiveParams(source) {
  const raw = new Array(E.PT_COUNT).fill(0);
  for (const [id, pt] of Object.entries(IDS)) raw[E.PT[pt]] = source.get(id);
  return E.effective(raw);
}

// GetParameterDisplay resolves the WHOLE parameter set, through the preset,
// for one readout -- Branching's eta is the machine's nominal times the trim,
// and a column under a preset reads the preset's value. The kit hands a
// display function only its own value, so the live set is captured from what
// mountDemo returns and read here; before that, the declared defaults.
let live = null;
const PARAMS = [];

function readout(id) {
  return () => {
    const source = live ?? { get: (key) => PARAMS.find((p) => p.id === key)?.default ?? 0 };
    const eff = effectiveParams(source);
    const index = E.PT[IDS[id]];
    const text = E.display(index, eff);
    return text || eff[index].toFixed(3);
  };
}

const std = (id, name, def, group, hint) => ({ id, name, type: 'standard', default: def, group, hint, display: readout(id) });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const col = (id, name, def, group) => ({ id, name, type: 'colour', default: def, group });

const PAD = 'FF_TYPE_XPOS / FF_TYPE_YPOS in the plugin, which a host may draw as a pad; the kit has no pad, so it is a slider here, 0..1 across the frame.';

PARAMS.push(
  opt('preset', 'Preset', ['Custom', ...E.PRESETS.map((p) => p.name)], 0, 'Machine',
    'An OVERRIDE, not a write. While this is on anything but Custom, its row is laid over the machine, supply, discharge and light columns every frame, and those sliders are not the truth -- their readouts show what the preset says. Resolume does not consume value events, so a plugin cannot push values back into the inspector. Custom gives the sliders back.'),
  opt('machine', 'Machine', ["Jacob's Ladder", 'Tesla Coil', 'Van de Graaff', 'Plasma Globe', 'Lichtenberg'], 1, 'Machine',
    'Five circuits around one growth model. Each machine decides when the air breaks and how much energy the channel carries; the dielectric breakdown model decides where.'),
  bool('fire', 'Fire', 0, 'Machine',
    'FF_TYPE_EVENT in the plugin; a toggle the page releases on the frame it acts, which is why it blinks. A bang, a spark, a restrike, a surge, a new figure.'),
  std('seed', 'Seed', 0.0, 'Machine', 'The random stream every growth choice is drawn from: the same seed and the same controls grow the same discharge.'),

  opt('supply', 'Supply', ['ZVS', 'NST', 'Flyback'], 1, 'Supply',
    'Named load lines: ZVS 20 kV behind 0.8 MOhm, a neon-sign transformer 15 kV behind 0.5 MOhm, a flyback driver 25 kV behind 4 MOhm.'),
  std('voltage', 'Voltage', 0.5, 'Supply', 'x0.5 to x2 on the supply’s open-circuit voltage.'),
  std('impedance', 'Source Impedance', 0.5, 'Supply', 'x0.25 to x4 on the source resistance. It decides the ladder’s extinction length: double it and the arc goes out at half the height.'),

  std('branching', 'Branching', 0.5, 'Discharge', 'The breakdown model’s exponent eta, x0.5 to x2 around the machine’s own: 1 is a Lichtenberg figure (DLA’s dimension, 1.71), 2 to 3 lightning, large a nearly straight arc.'),
  std('detail', 'Detail', 0.5, 'Discharge', 'Lattice sites across the scene’s height, x0.5 to x2. A cost, not a look: the Laplace solve is over every one of them, here on the page’s own thread. Turn it down on a slow machine.'),
  std('memory', 'Channel Memory', 0.5, 'Discharge', 'How long a channel stays hot enough to be re-used. A segment carrying a fraction f of its root’s current cools in f x this, so the tips go first and the trunk last.'),
  std('reach', 'Reach', 0.5, 'Discharge', 'x0.25 to x4 on each machine’s nominal growth per event.'),

  std('rodSpread', 'Rod Spread', 0.3750, "Jacob's Ladder", 'The angle between the rods.'),
  std('rodLength', 'Rod Length', 0.4286, "Jacob's Ladder"),
  std('rise', 'Rise Speed', 0.5943, "Jacob's Ladder", 'The hot column’s buoyant rise. The roots slide up the rods more slowly than the middle, which is what bows the arc.'),
  std('wind', 'Wind', 0.5, "Jacob's Ladder", 'Sideways air, -0.5 to +0.5 m/s.'),

  std('bps', 'BPS', 0.6078, 'Tesla Coil', 'The interrupter: off at the very bottom, then 5 to 1000 bangs a second. Streamers lengthen once 1/BPS is shorter than the channel memory.'),
  std('topload', 'Topload Size', 0.3103, 'Tesla Coil', 'The toroid’s major radius. The bang’s voltage is V_oc sqrt(C_primary / C_top), and no streamer is longer than that over 5 kV/cm.'),
  opt('target', 'Target', ['None', 'Floor', 'Point'], 1, 'Tesla Coil', 'What the streamers can strike: nothing, the floor, or a grounded point at Target X/Y.'),
  std('targetX', 'Target X', 0.78, 'Tesla Coil', PAD),
  std('targetY', 'Target Y', 0.24, 'Tesla Coil', PAD),

  std('belt', 'Belt Current', 0.5886, 'Van de Graaff', '1 to 50 microamps. The interval between sparks is C V_b / I, exactly, on a polished sphere.'),
  std('sphere', 'Sphere Size', 0.4000, 'Van de Graaff'),
  std('gap', 'Gap', 0.7686, 'Van de Graaff', 'Surface to surface, to the discharge ball. The breakdown voltage is where the facing surface field reaches Peek’s value, from Kelvin’s image series.'),
  std('finish', 'Sphere Finish', 1.0, 'Van de Graaff', 'Peek’s surface factor, 1 polished to 0.82 rough. A rough sphere coronas first, and the corona clamps its voltage: rough enough and it glows instead of sparking.'),

  std('globe', 'Globe Size', 0.7500, 'Plasma Globe'),
  bool('finger', 'Finger', 0, 'Plasma Globe', 'A finger on the glass: that patch is grounded, and the filaments find it.'),
  std('fingerX', 'Finger X', 0.72, 'Plasma Globe', PAD),
  std('fingerY', 'Finger Y', 0.66, 'Plasma Globe', PAD),

  opt('origin', 'Origin', ['Point', 'Edge'], 0, 'Lichtenberg', 'Grown from a point in the middle of the slab, or from its bottom edge.'),

  std('efficiency', 'Efficiency', 0.5, 'Light', 'The fraction of the electrical energy that leaves as light, 1% nominal, x0.1 to x10.'),
  std('glow', 'Glow', 0.45, 'Light', 'The fraction of the light that goes into the halo. It redistributes the light; it adds none.'),
  opt('gas', 'Gas', ['Air', 'Neon', 'Argon', 'Neon-Xenon'], 0, 'Light', 'The streamer colour, from the gas’s strongest spectral lines through the CIE observer. A hot arc moves toward a 6500 K blackbody as its current rises.'),
  std('shutter', 'Shutter', 1.0, 'Light', 'The fraction of the frame the shutter is open. A spark lasts microseconds, so at 180 degrees about half of them land between frames and are never seen.'),
  std('persistence', 'Persistence', 0.30, 'Light', 'The camera’s persistence, in seconds, not a per-frame factor.'),
  bool('apparatus', 'Show Apparatus', 1, 'Light', 'The machine itself, lit by its own discharge.'),
  col('backR', 'Background', 0.010, 'Light'),
  col('backG', 'Background Green', 0.012, 'Light'),
  col('backB', 'Background Blue', 0.022, 'Light'),

  opt('detect', 'Detect On', ['Luma', 'Alpha', 'Edges'], 0, 'Over', 'Over only: what makes a part of the clip ground.'),
  std('threshold', 'Ground Threshold', 0.60, 'Over', 'Over only: the clip is thresholded onto a 160-wide mask, a frame late, and every cell past this becomes ground the discharge can strike.'),
  std('illumination', 'Illumination', 0.35, 'Over', 'Over only: how much the flash lights the clip.'),
  std('mix', 'Mix', 1.0, 'Over', 'Over only: the clip against the composite.'),

  std('posX', 'Position X', 0.5, 'Layout', PAD),
  std('posY', 'Position Y', 0.5, 'Layout', PAD),
  std('scale', 'Scale', 0.5, 'Layout', 'x0.25 to x4 on the machine’s own framing.'),
  std('rotation', 'Rotation', 0.5, 'Layout'),
);

let renderer = null;

const mounted = mountDemo({
  name: 'Flyback',
  pluginId: 'HV01 · HV02',
  tagline:
    'High-voltage discharges — a Jacob’s ladder, a Tesla coil, a Van de Graaff, a plasma globe and a Lichtenberg figure — grown by the dielectric breakdown model. Nothing is drawn as a shape of lightning: the potential in the air is solved as a Laplace problem, the channel grows toward where the field is strongest, and five small circuits decide when the air breaks and how much energy it carries. The light is the plugin’s own shaders; the physics is a full JavaScript port of its engine.',
  repo: 'https://github.com/stoatworks-labs/flyback',
  page: 'https://stoatworks-labs.com/software/flyback/',

  blurb:
    'It is Flyback’s own GLSL, ported from the repository to WebGL2, driven by a JavaScript port of the plugin’s CPU engine — the Laplace solve, the breakdown model and all five machines — which nothing checks but a reader. The source build reads no video; the Over build strikes into a generated clip.',

  showBackdrop: false,
  needFloat: true,
  needFloatBlend: true,

  variants: {
    label: 'Plugin',
    default: 'source',
    options: [
      { id: 'source', name: 'SW Flyback (source)', hint: 'HV01, FF_SOURCE: the machine over its own background. Reads no clip.' },
      { id: 'over', name: 'SW Flyback Over (effect)', hint: 'HV02, FF_EFFECT: the machine over the clip, whose bright parts become ground the discharge strikes into.' },
    ],
  },

  // For the Over build. Lights on black first: bright objects on a dark field
  // are what a threshold turns into clean ground.
  sources: ['spot', 'scene', 'grid', 'alpha'],

  params: PARAMS,

  differences: [
    'The engine is a hand port to JavaScript, and nothing checks it but a reader. It is the whole engine — the PCG-and-multigrid Laplace solve, the breakdown model with its quenched disorder, Kirchhoff, all five machines and their circuits, every control’s conversion, the presets — at the plugin’s own lattice sizes. It is not bit-exact: the plugin does its lattice arithmetic in 32-bit floats and this rounds only when a value is stored, so a given seed grows a discharge of the same model but not the same one.',
    'The engine runs on this page’s main thread, in the frame it is for. The plugin runs it on a worker thread one frame late. On a slow machine the page drops frames rather than the circuit changing, and a frame longer than a tenth of a second is clamped by the page’s clock, so time runs slow instead of jumping. The line under the picture says what the engine cost. Detail is the control that sets that cost.',
    'Nothing audio. The plugin declares an Audio FFT buffer that Resolume fills, and Audio Fires and Audio Drive over it. A browser has no Resolume FFT, so all three are absent. With no spectrum the plugin’s analyser reports a level of 0 and never fires, so leaving them out changes nothing the page draws.',
    'Fire is FF_TYPE_EVENT in the plugin. The kit has no event type, so it is a toggle here that the page releases on the frame it acts, which is why it blinks. Target, Finger and Position X/Y are XPOS/YPOS pairs a host may draw as a pad; here they are plain sliders. The About block is not on the panel.',
    'WebGL2 has no CLAMP_TO_BORDER. The glow pyramid’s levels are read bilinearly with a zero border in the plugin, so light blurred past the frame’s edge fades out; here the edge texel is repeated instead, so a discharge touching the frame’s edge can leave a sliver more halo there. The plugin’s --light check holds its light to 4e-5; this page makes no such claim.',
    'Over: the clip’s ground mask is read back with a synchronous readPixels and used a frame late. The plugin reads it through a two-buffer ring, also a frame late, without the stall. The canvas composites premultiplied, and the plugin writes straight colour with the clip’s alpha, so over the transparency clip what shows through is the page’s compositing, not Resolume’s.',
    'Paused, a moved control redraws the last frame with the new look but without its persistence trail, and the engine is not stepped: engine controls take effect on Play or Step.',
    'The page needs 32-bit float render targets with blending and bilinear filtering (EXT_color_buffer_float, EXT_float_blend, OES_texture_float_linear). The plugin keeps the whole light path in float32 because a pixel’s joules are below half float’s smallest normal number; a browser without all three is refused rather than handed a quietly wrong picture.',
    'Nothing here is measured. The plugin’s harness checks the Laplace solve against the coaxial closed form, the fractal dimension against DLA, the ladder’s extinction length, the Tesla coil’s bang count, the Van de Graaff’s interval, Kirchhoff and the light’s conservation, each with a negative control — and that harness, not this page, is the reason to believe the model.',
  ],

  createRenderer: (gl, quad) => {
    renderer = new FlybackRenderer(gl, quad);
    return renderer;
  },
});

live = mounted?.params ?? null;

if (live) {
  // Every readout depends on the whole set (the machine's nominal, the preset
  // over it), so any change resyncs them all. `reset` is what buildPanel
  // listens to for that; it carries and changes no values.
  live.addEventListener('change', () => live.dispatchEvent(new CustomEvent('reset')));
  live.dispatchEvent(new CustomEvent('reset'));
}

//---------------------------------------------------------------------------
// The clip picker and "Use my own…" belong to the Over build only: a source
// reads no clip. Hidden while the source is chosen rather than left inert.
//---------------------------------------------------------------------------
const clipField = [...document.querySelectorAll('.transport__field')]
  .find((field) => field.querySelector('.transport__label')?.textContent === 'Clip');
const fileField = document.querySelector('.transport__file');
const plugin = [...document.querySelectorAll('.transport__field')]
  .find((field) => field.querySelector('.transport__label')?.textContent === 'Plugin')
  ?.querySelector('select');

function syncClipControls() {
  const over = (plugin?.value ?? mounted?.state.variant) === 'over';
  if (clipField) clipField.hidden = !over;
  if (fileField) fileField.hidden = !over;
}
plugin?.addEventListener('change', syncClipControls);
syncClipControls();

//---------------------------------------------------------------------------
// What the engine cost, under the picture: a slow frame should read as a slow
// CPU, not as a broken page.
//---------------------------------------------------------------------------
const stage = document.querySelector('.stage');
if (stage && mounted) {
  const line = document.createElement('p');
  line.className = 'stage__status flyback-stats';
  line.setAttribute('aria-live', 'off');
  stage.append(line);
  const names = ["Jacob's Ladder", 'Tesla Coil', 'Van de Graaff', 'Plasma Globe', 'Lichtenberg'];
  const tick = () => {
    if (renderer) {
      const s = renderer.stats;
      line.textContent = `${names[s.machine] ?? ''} · lattice ${s.lattice[0]} × ${s.lattice[1]} · engine ${s.engineMs.toFixed(1)} ms on this page’s thread · ${s.segments} segments · ${s.events} discharge${s.events === 1 ? '' : 's'} lit this frame`;
      line.dataset.frames = String(s.frames ?? 0);
    }
  };
  setInterval(tick, 500);
  tick();
}
