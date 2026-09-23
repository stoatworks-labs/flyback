# Attributions

flyback is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. flyback is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. The offline harness links it to deflate its PNG output.
Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### millpond, rosette, macroblock — the audio analyser and the clock

<https://github.com/stoatworks-labs/rosette>
Licence: MIT
Copyright: Stoatworks Labs

`source/Audio.{h,cpp}` is millpond's copy of rosette's analyser (itself from
macroblock's), with its primed first frame, which `hvtest --onset` checks. The
host-clock unit vote in `UpdateClock` is rosette's.

### downpour, vectrix, millpond, graticule, tinsel

<https://github.com/stoatworks-labs/downpour>
Licence: MIT
Copyright: Stoatworks Labs

One core registered as a source and an `Over` effect is downpour's shape. The
segment renderer's closed form -- a uniform segment convolved with a Gaussian
spot, energy-conserving for any length -- is vectrix's (`source/render/`),
tightened here for exact conservation. `GLState.h` and `PassBuffer` (`FFGLFBO`
with the SDK's colour-texture leak fixed) come from millpond, vectrix and
tinsel; the override preset model and `--defaults`/`--names` from graticule;
the harness shape, `--script` cue sheets, `tools/sweep.py` and
`tools/verify.sh` from millpond and tinsel.

## Method

Physics described in books and papers rather than copied from anyone's
source. Where a number or formula was carried from memory rather than checked
against the text in this session, AGENTS.md says so.

- The dielectric breakdown model — L. Niemeyer, L. Pietronero and H. J.
  Wiesmann, "Fractal dimension of dielectric breakdown", *Phys. Rev. Lett.*
  52, 1033 (1984).
- Diffusion-limited aggregation, the harness's reference cluster — T. A.
  Witten and L. M. Sander, *Phys. Rev. Lett.* 47, 1400 (1981).
- The arc's voltage — Hertha Ayrton, *The Electric Arc* (1902): the form of her
  equation; the low-current constants are this repo's (AGENTS.md).
- The two-sphere field by Kelvin's method of images — W. R. Smythe, *Static and
  Dynamic Electricity*.
- Corona and breakdown fields at a sphere — F. W. Peek, *Dielectric Phenomena
  in High Voltage Engineering* (1929).
- Point-charge superposition for fast Laplacian growth, measured here and NOT
  used — T. Kim, J. Sewall and M. C. Lin (2007).
- The CIE 1931 observer as analytic lobes — C. Wyman, P.-P. Sloan and P.
  Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching
  Functions", *JCGT* 2(2), 2013. Spectral lines from the NIST Atomic Spectra
  Database, weights approximate.
- The normal CDF — Abramowitz & Stegun 7.1.26.
- PCG random numbers — M. E. O'Neill (2014); lowbias32 — C. Wellons.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong,
or you would rather not be listed — open an issue and it will be fixed.
