#pragma once

#include "PassBuffer.h"
#include "engine/Engine.h"
#include "engine/Physics.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <vector>

/**
    The camera: joules of light in, a picture out.

    Five passes, and the first three are where the light is accounted for:

      1. Emission   RGBA32F at the output's size, in joules per pixel. The
                    camera's persistence scales last frame's in place (Decay),
                    then every segment's light is deposited additively by the
                    exact closed form (Shaders.h).
      2. Pyramid    six-plus octaves of 2x2 sums, each blurred by a normalised
                    Gaussian. Every level holds the same joules.
      3. Light      (1 - Glow) emission + Glow * the octaves, brought back up
                    bilinearly. RGBA32F. This is the buffer `hvtest --light`
                    sums: after the glow, before the film.
      4. Display    into the host's framebuffer: the background (or the clip,
                    lit by the flash), the apparatus lit by the discharge, and
                    the light through the film's response.
      5. Ground     Over only: the clip thresholded onto a small mask, read
                    back through a pixel-buffer ring a frame late, for the
                    engine's lattice.

    ## Raster independence

    The physics is in metres, so the picture should not change with the
    output's size except in sharpness. Three things see pixels and are
    scaled so they do not: the exposure is per frame-height squared, the
    narrowest core is 0.8 px at 1080 lines and proportionally more above, and
    the glow's octaves are placed in the FRAME, not in pixels: each of the six
    reference octaves (sized for 1080 lines) sits at level k + log2(H / 1080)
    of this raster's pyramid, its weight split linearly between the two levels
    either side. At 360 lines the halo is as wide a fraction of the frame as at
    1080; with octaves fixed in pixels it was three times wider, and blurred
    light off the edge of a small frame (`--light` caught 0.24% leaving).

    ## Precision

    Everything on the light's path is 32-bit float. Not for the mantissa: for
    the exponent. A pixel holds joules of order 1e-7, and half float's
    smallest normal number is 6.1e-5, so a half-float glow pyramid flushed or
    crushed most of the halo -- it lost 10 to 50% of the Glow's light, more at
    1080p than at 360p and more for a branchier spark, because both spread the
    same joules thinner. `--light` found it. Its tolerance is now float32's:
    2^-24 relative per store, times the stores a joule passes through.
*/
namespace flyback
{
class Renderer
{
public:
	struct Look
	{
		double sceneHeight = 1.0;///< metres of scene the frame's height shows
		double posX = 0, posY = 0, scale = 1, rotation = 0;

		float camera      = 1.0f;///< display units per J per frame-height^2
		float glow        = 0.3f;///< fraction of the light in the halo
		physics::Rgb gas;        ///< streamer colour, energy weights
		physics::Rgb arc;        ///< thermal colour, energy weights
		float decay       = 0.0f;///< persistence: fraction of last frame's emission kept
		float apparatus   = 1.0f;
		float background[ 3 ] = { 0.0f, 0.0f, 0.0f };///< sRGB
		float illumination = 0.0f;
		float mix          = 1.0f;
		bool clearHistory  = false;
	};

	bool InitGL();
	void DeInitGL();

	/// Draw a frame. `clip` is 0 for the source.
	bool Render( const Frame& frame, const Look& look, GLuint hostFBO, const GLint viewport[ 4 ], GLuint clip,
	             float maxU, float maxV );

	/// Over: threshold the clip onto a `w` x `h` mask and hand back the one
	/// rendered LAST frame (a frame late, so nothing waits on the GPU). Returns
	/// false while there is nothing to hand back yet.
	bool Ground( GLuint clip, float maxU, float maxV, int detect, float threshold, int w, int h,
	             std::vector< uint8_t >& mask );

	//-------------------------------------------------------------------
	// For the harness.
	//-------------------------------------------------------------------
	GLuint LightTexture() const
	{
		return light.TextureID();
	}
	GLuint EmissionTexture() const
	{
		return emission.TextureID();
	}
	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}
	/// Float stores a joule passes through between the emission buffer and
	/// the light buffer, the worst case over the octaves.
	int FloatStores() const
	{
		return floatStores;
	}

private:
	bool Ensure( int w, int h );

	ffglex::FFGLShader segmentShader, decayShader, downShader, blurShader, lightShader, displayShader, groundShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer emission, light;
	std::vector< PassBuffer > levelA, levelB;
	int levels      = 0;
	int floatStores = 0;
	float levelWeight[ 8 ] = {};///< each pyramid level's share of the halo

	PassBuffer groundMask;
	GLuint groundPbo[ 2 ]  = { 0, 0 };
	int groundSize[ 2 ][ 2 ] = { { 0, 0 }, { 0, 0 } };
	int groundWrite        = 0;

	GLuint segmentVAO = 0, segmentVBO = 0;
	int width = 0, height = 0;
	std::vector< float > packed;
};
} // namespace flyback
