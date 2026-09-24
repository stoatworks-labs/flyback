# Attributions

Flyback is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Audio analyser and host-clock vote — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is millpond's copy of rosette's analyser (itself from macroblock's), with its primed first frame, which hvtest --onset checks. The host-clock unit vote in UpdateClock is rosette's.

### Source-plus-Over shape and the segment renderer — Stoatworks downpour

<https://github.com/stoatworks-labs/downpour>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect is downpour's shape. The segment renderer's closed form, a uniform segment convolved with a Gaussian spot, is vectrix's, tightened here for exact conservation. GLState.h and PassBuffer come from millpond, vectrix and tinsel; the override preset model and --defaults/--names from graticule; the harness shape, --script cue sheets, tools/sweep.py and tools/verify.sh from millpond and tinsel.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The dielectric breakdown model

L. Niemeyer, L. Pietronero and H. J. Wiesmann, "Fractal dimension of dielectric breakdown", Phys. Rev. Lett. 52, 1033 (1984); diffusion-limited aggregation as the harness's reference cluster, after Witten & Sander, Phys. Rev. Lett. 47, 1400 (1981). Implemented from the papers; nothing is copied from anyone's source.

## Standards and published specifications

What the implementation is measured against.

- **Hertha Ayrton, The Electric Arc (1902)** — The form of the arc-voltage equation behind the Jacob's ladder's extinction length; the low-current constants are this repository's own (AGENTS.md).
- **F. W. Peek, Dielectric Phenomena in High Voltage Engineering (1929), and W. R. Smythe, Static and Dynamic Electricity** — Corona and breakdown fields at a sphere; the two-sphere field by Kelvin's method of images.
- **Wyman, Sloan & Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions", JCGT 2(2), 2013** — The CIE 1931 observer as analytic lobes; spectral lines from the NIST Atomic Spectra Database, weights approximate.
- **Melissa E. O'Neill, PCG (2014), and Abramowitz & Stegun 7.1.26** — The random numbers behind the quenched disorder, and the normal CDF.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
