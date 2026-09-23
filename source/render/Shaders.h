#pragma once

/**
    Every shader the renderer uses, as `#version 410 core` source.

    ## The one rule: light is deposited, never painted

    The engine hands over segments carrying joules of light. The segment pass
    spreads each one's joules over the pixels around it by the exact
    convolution of a uniform line segment with a Gaussian spot -- vectrix's
    closed form, energy-conserving for any length and any width:

        f(u, v) = (E / L) G_sigma(v) [ Phi((u + L/2)/sigma) - Phi((u - L/2)/sigma) ]

    with u along the segment from its centre and v across it. Three things
    were changed from vectrix's copy, because here the SUM is a test, not a
    look:

    - Phi is Abramowitz & Stegun 7.1.26 (|error| < 1.5e-7), not a tanh fit
      (3e-4), which would have been the largest error in the frame;
    - the across profile has the quad's edge value subtracted, as vectrix does,
      and is then renormalised by the exact integral of what is left,
      `AcrossNorm`, so the pedestal does not quietly eat 1.5e-4 of every
      segment;
    - sigma is never below `MinSigma` (0.8 px): sampling a Gaussian at pixel
      centres sums to its integral to within exp(-2 pi^2 sigma^2), 3e-6 at
      0.8 px and 7e-3 at 0.5.

    So the emission buffer, in joules per pixel, sums to the joules the engine
    sent, and `hvtest --light` holds it to that.

    ## Glow redistributes; it does not add

    A pyramid of 2x2 SUMS (so every level holds the same total), each level
    blurred by a normalised Gaussian with zero outside the frame, and brought
    back up bilinearly, which is a partition of unity. `Glow` is the fraction
    of the light that goes into the halo, and the halo's octave weights sum to
    one. Light that the blur carries off the edge of the frame is lost, as it
    is from a camera's frame.

    ## Reserved words

    `sample input output filter common active half layout flat patch` are GLSL
    reserved words and must not be identifiers. `tools/verify.sh` greps for it.
*/
namespace flyback::shaders
{
extern const char* const kQuadVertex;   ///< full-screen quad, FFGLScreenQuad's layout
extern const char* const kSegmentVertex;///< one instanced quad per segment
extern const char* const kSegmentFragment;
extern const char* const kDecayFragment;///< the camera's persistence: E *= decay
extern const char* const kDownFragment; ///< 2x2 sum
extern const char* const kBlurFragment; ///< separable normalised Gaussian, zero outside
extern const char* const kLightFragment;///< (1-g) E + g sum_k w_k up(G_k)
extern const char* const kDisplayFragment;///< background, apparatus, the clip, the light
extern const char* const kGroundFragment; ///< Over: the clip, thresholded, onto the lattice

/// How many octaves of glow, and each one's share. Mirrored nowhere: the
/// renderer passes these to the light pass as a uniform array.
constexpr int kGlowLevels                   = 6;
extern const float kGlowWeights[ kGlowLevels ];
} // namespace flyback::shaders
