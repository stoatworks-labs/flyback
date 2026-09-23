# flyback — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-09-23 in one session, from Allan's request for "a new resolume
plugin/source which recreates high voltage electric effects, such as Jacob's
ladders, tesla coils, van de Graaff generators etc." The spec is
`~/Projects/resolume/specs/SPEC-flyback.md`; the shared brief is
`~/Projects/resolume/specs/BRIEF.md`.

## The one idea

**Air is an insulator until the field tears it, and where it tears is a
Laplace problem.** Every discharge grows by the dielectric breakdown model
(Niemeyer, Pietronero & Wiesmann, PRL 52, 1033, 1984). The electrodes and the
channel already grown are held at their potentials, and φ in the air solves
∇²φ = 0. The channel extends to a neighbour with p ∝ |∇φ|^η. Around that sit
five small circuits, each deciding *when* the air breaks and *how much energy*
the channel carries. The light is accounted for exactly, from the event's
energy to the frame's pixels.

## Shape of the code

    source/engine/Lattice.*   the Laplace solve: PCG + multigrid V-cycle, Dirichlet cells
    source/engine/Dbm.*       the breakdown model: cells, candidates (Fenwick tree), growth
    source/engine/Tree.*      a grown discharge; Kirchhoff currents; lengths
    source/engine/Physics.*   every constant and closed form: Ayrton L*, Peek, images, colours
    source/engine/Engine.*    the five machines, the clock, the shutter window. No GL.
    source/render/Renderer.*  segments -> emission -> glow pyramid -> light -> picture
    source/render/Shaders.*   the seven GLSL passes
    source/Controls.*         0..1 host parameters <-> physics; display strings
    source/Presets.h          one preset per machine (override model)
    source/Flyback.*          the plugin: params, clock, audio, Over readback
    source/{Source,Effect}Plugin.cpp   the two registrations, one per bundle
    tools/hvtest/             the harness: checks, --negative, --bench, --film/--pipe
    tools/sweep.py            no control is silently dead
    tools/mutate.sh           one-character mutants must be caught
    tools/verify.sh           all of it

## Decisions taken without asking

- **A real Laplace solve, not point-charge superposition.** The spec suggested
  Kim–Sewall–Lin's O(candidates) superposition. It was prototyped first and
  measured: at η = 1 it grows clusters whose mass dimension scatters from 1.67
  to 2.11 between seeds, and with its min/max normalisation the mean is 2.19.
  Equal charge per site is not a conductor, because a conductor's charge piles
  up at its tips. A CPU finite-difference solve was chosen instead: PCG with a
  multigrid V-cycle as the preconditioner (`Lattice.h` says why multigrid alone
  was not enough). The GPU multigrid alternative was not tried: growth needs
  the field after every few sites, and a readback per site is a stall.
- **Growth one site at a time, the field refreshed every 32.** A full solve per
  site measured 1.74 ± 0.03 at η = 1 over four seeds, at 3.7 ms/site on 257².
  A local Gauss–Seidel window (radius 8, 4 sweeps) after each site plus a full
  solve every 32 measured 1.71 ± 0.03, 23× cheaper. The same result, so it
  ships, and `--dimension` measures it as shipped.
- **Eight neighbours, and quenched disorder.** With 4-neighbour growth, or
  8-neighbour growth on a clean lattice, anything with η above about 2 showed
  the lattice's axes: the first Tesla coil render was a plus sign. Real air
  varies from place to place, and its breakdown threshold with it. Each site's
  growth weight is multiplied by exp(0.5 ξ), with ξ a standard normal fixed
  per site per burst (Box–Muller from hashed uniforms). That is a known
  extension of the model. At η = 1 it moved D from 1.787 to 1.723 (6 clusters
  each, 320² lattice), both inside the band. `--dimension` runs with it.
- **The fractal-dimension estimator is radius of gyration over the growth
  history, not box counting.** Box counting, calibrated on real DLA (random
  walkers, known D = 1.71), reads 1.47–1.50 at 2000–5000 sites. Small
  clusters' lacunar edges and one-site branches bias it low by 0.2, so it
  cannot hold anything to 1.70 ± 0.05 at a size the harness can grow. The Rg
  estimator reads the same DLA clusters as 1.71–1.74. `--dimension` asserts
  on it and prints box counting for the record. The ±0.05 band is the spec's
  and the literature's; it is also 3 SE of the mean as measured on the DLA
  reference (0.045), not on the clusters under test.
- **Relative controls.** Voltage, Source Impedance, Branching, Detail, Channel
  Memory, Reach, Efficiency and Scale are trims around each machine's or
  supply's nominal. Switching Machine alone gives each machine looking like
  itself. The display shows the physical value.
- **Presets are graticule's override model**, one row per machine, row 1 = the
  constructor's defaults (the Tesla coil). While a preset is selected its
  columns are not editable from the panel. That is the model's known cost.
- **A Lichtenberg group with one control (Origin).** The spec's control list has
  none, but "a point or an edge" needs a switch.
- **A fourth gas, Neon-Xenon**, the classic plasma-globe fill. Pure argon comes
  out blue through the observer, and pure neon orange. The 0.5 : 1 line
  weighting of the mix is a look, and the code says so.
- **Supply kinds are named (V_oc, R_s) pairs**: ZVS 20 kV / 0.8 MΩ, NST
  15 kV / 0.5 MΩ, Flyback 25 kV / 4 MΩ. All are linear load lines. An NST's is
  really reactive (an ellipse), but the spec's closed form needs a line, and
  the spec says an NST is "the same model with a bigger R_s".
- **The Tesla coil's bang energy is ½ C_p V_oc²** (lossless transfer from a
  30 nF primary), and V_top = V_oc √(C_p / C_top). The topload is a sphere of
  the toroid's major radius. A streamer is capped at V_top / 5 kV/cm. Reach
  sets the metres of new channel per bang.
- **Channel memory**: a segment carrying a fraction f of its root's current
  cools in f·τ, because thermal diffusion time goes as radius² and radius² as
  current. So tips cool first and the trunk last, and the next bang re-uses
  what is still hot. That is what makes streamer length rise smoothly with BPS
  once 1/BPS < τ.
- **The secondary is inert, not ground.** With it held as ground (quarter-wave
  potential), `--over`'s control sent 469 of 469 strikes racing down the coil.
  On a real coil that is the sign of one badly overdriven. The secondary still
  shapes the field; the base and strike rail are ground.
- **The Van de Graaff's spark starts on the cap facing the gap** (within 50° of
  the axis), because that is where the surface field first reaches Peek's
  value. Corona comes from the whole sphere.
- **The Van de Graaff's corona is charged against the belt (round two).**
  C dV/dt = I − G(V − V_c) past onset V_c = m·V_b, where m is Peek's surface
  factor (the Sphere Finish control, 1 polished to 0.82) and
  G = 24π ε₀ μ a V_c / b², derived in `Physics.h` from unipolar ion drift with
  the surface field held at onset (Kaptzov) to first order in space charge;
  b is the sphere-to-base distance, 0.34 m. Integrated by backward Euler at
  10 µs, exact for the belt alone. The finding: τ = C/G ≈ 0.7 ms and I/G ≈
  350–420 V, so the corona does not visibly slow the climb, it clamps it at
  V_c + I/G. The sphere both glows and sparks only when V_b − V_c < I/G, a
  window m > 0.998. Polished (m = 1) has no corona term at all (Peek: between
  spheres, sparkover comes before visual corona while the spacing is under
  about 2.04 radii; here 10 cm against a 15 cm sphere, 0.67), so the
  interval is C·V_b/I to rounding. Setting V_c = m·V_b (onset as a fraction
  of the sparkover voltage, rather than Peek's visual-corona formula for the
  sphere alone) is a simplification: it keeps the polished limit exact.
  Sphere Finish is a preset column, 1.0 in every row, so presets are polished.
- **The Van de Graaff's capacitance is the two-sphere one**, from the same
  image series (18.56 pF, against 16.7 pF for the sphere alone).
- **Globe filaments grow one at a time**, each to the glass. That path becomes
  an arc (Cell::Arc: a resistor held on a linear potential, no longer a
  source), and the burst's dead ends are dropped. Grown all at once, the dead
  branches beside the first contact sat in the strongest field in the globe and
  spent the budget crawling along the glass. Filament count is 2 + V_oc / 3.5 kV.
- **The Lichtenberg figure grows over 3 s and stays lit.** That is staging; a
  real one forms in nanoseconds and is dark afterwards. README says so.
- **The engine runs on the render thread.** A worker thread would need the next
  frame's time before the host gives it. Pipelining a frame late was possible
  but not done: mean engine cost is 1–5.8 ms and the worst frame 11–12 ms
  (the globe).
- **Provisional About/ATTRIBUTIONS** hand copies with `guide = ""`, as in the
  rest of the unreleased tranche. `sync-about.py --only flyback` will replace
  them once flyback is registered.
- **Display names `SW Flyback` (10 bytes) and `SW Flyback Over` (15 bytes)**, ids
  HV01/HV02 (FB0x are flipbook's). Graticule's un-prefixed naming is a gap, not
  a precedent.

## The traps

Ordered by how much time they cost.

**Half float cannot hold a pixel's joules.** The glow pyramid was RGBA16F. A
pixel holds light of order 1e-7 J, below half float's smallest normal number
(6.1e-5), so the halo was flushed or crushed. It lost 10 to 50% of the Glow's
light, more at 1080p than at 360p and more for a branchier spark, because both
spread the same joules thinner. `--light` found it: the emission buffer
summed to within 1e-5 while the light buffer was 4–21% short. Everything on
the light path is now float32. (Millpond's half-float truncation trap is the
other reason.)

**A glow sized in pixels is a different glow at every raster.** The octaves
started at level 0 at every size, so at 640×360 the halo was three times wider
as a fraction of the frame, and 0.24% of the light blurred off its edge.
`--light` caught it at the smaller raster only. The six reference octaves are
now placed in the frame (octave k of 1080 lines sits at level k + log2(H/1080),
split linearly between neighbours).

**The kernel's sampling error has a slope term.** The first `--light` bound was
the Gaussian's Poisson-summation term alone, 2 exp(−2π²σ²). The
pedestal-subtracted across-profile meets zero with a slope jump
J = 4.5 φ(4.5)/σ², and that adds up to J/6 (1.9e-5 at σ = 0.8 px). A re-tuned
spark measured 1.9e-5 against a 1.5e-5 bound. The term was derived, not
widened to fit.

**A check that uses the code under test to compute its expected value catches
nothing in that code.** `tools/mutate.sh` changed one character in L*'s linear
coefficient and one in the image series, and `--ladder` and `--vdg` passed:
they asked `ExtinctionLength` and `SolveTwoSpheres` what to expect. L* is now
found by bisection on the load-line discriminant. The image charges are
checked against both spheres' boundary conditions at 128 surface points (to
5e-15 V; by uniqueness that makes them the solution), and C, the facing fields
and Peek's V_b are worked out from them in the harness.

**The lattice's outer ring is a wall at 0 V, and the field beside it is
strong.** A Van de Graaff spark that found the ring crawled round the whole
frame along it and never completed: a 1.6 m spark of 1000 sites, an 86 ms
frame. A ring held at or below 0.25 now counts as ground (the room's far
walls). The ladder's ring is at 0.5 and does not.

**Point-charge superposition is not the breakdown model** (above). The spec
suggested it; it was measured before being built on.

**Multigrid alone converges at 0.4–0.6 a cycle** once a spidery conductor fills
the interior, against 0.03 on an empty square. A coarse grid cannot represent
a wire one cell wide. PCG with the V-cycle as preconditioner fixes it (about
five iterations from a warm start). The V-cycle must be symmetric for CG, with
red-black order reversed on the way up. Masking a coarse cell by *injection*
instead of "any held fine cell under its 3×3 stencil" made the V-cycle
diverge. The residual printed 0 because `max` of NaN is 0.

**`k * (1 / BPS)` is not `k / BPS`.** At 37.5 BPS the product put the 2250th
bang at 60.000000000000007 s and the minute had 2249. Bang times are
`anchor + (k + 1) / bps`.

**Scene framing decides whether a physics claim is visible.** On 0.4 m rods at
14°, every arc ran off the top before reaching L*, so the headline behaviour
never showed on the default. The default spread is now 20° (a 14 cm straight
gap at the top, beyond L*). `--ladder` asserts on the shipped geometry too:
every extinction is at L* or over the top, nothing else.

**Kept dead ends breed a bush.** Globe dead ends kept for τ, as the coil keeps
its streamers, grew a thicket against the glass on whichever side the first
filaments landed. The RF re-strikes every cycle, so dead ends go every frame.

**`half` is a GLSL reserved word, and it was walked into** as a local in the
display pass's coil shading. `verify.sh` greps for it and for the other
reserved words, and glslc compiles every shader.

**Clipping out-of-gamut violet turned argon pure blue.** Violet lies outside
sRGB. Setting the negative red to zero discarded the hue; desaturating toward
white (adding the most negative channel to all three) keeps it.

**The harness's own loop bounds.** `--onset`'s second case fed a hit on frame
50 and rendered frames 30–49. Rendering 25 frames fixed it. It was a harness
bug; the analyser was fine.

**Debug `getenv` switches** were added to the engine while hunting the ladder's
corridor and are gone again (millpond's trap). Grep for `getenv` before
committing: only `Diag.cpp` may have one.

**A shared machine's load is in the GPU numbers.** The first bench, with the
load average at 13, put three machines at 11–12 GPU ms at 4K. Run alone they
were 2.6–3.1 ms. `--bench` prints the load average, and the README's table is
from a quieter run (load ~8).

## Every numeric check: its tolerance, and another rasteriser or raster?

| check | bound | why that number | another rasteriser / raster |
| --- | --- | --- | --- |
| `--laplace` | per site, h·(\|ln(r/R2)\|/R1 + \|ln(r/R1)\|/R2)/L² + solver residual | a staircase electrode's effective radius is uncertain by a lattice step; that times the profile's sensitivity to each radius | no pixels: holds on any GPU. Run at two lattice steps (10.4 and 5.2 mm), where the bound scales with h |
| `--dimension` | 1.70 ± 0.05; η = 0 > 1.9; η = 6 < 1.25; monotonic | the spec's and literature's band, and 3 SE on the DLA reference | engine only, lattice in metres: no rasteriser or raster enters. Floating-point order could change one site's choice; the statistic would not notice |
| `--ladder` | L* to one lattice step (the column is measured on a polyline refined to h/4); climb time to one frame | the arc cannot be located more finely than the lattice it lives on; the spec's "± one frame" | engine only, metres and seconds: raster-independent by construction |
| `--tesla` | bang count exact; lengths compared at 3 SE, the SE widened by √(τ·BPS) for the correlation between bangs sharing channel | a clock is exact; the memory claim is statistical | engine only |
| `--vdg` | polished interval to 1e-9 s; corona interval to dt/(2e)·(V∞−V_c)/(V∞−V_b) + 1.125 dt²/τ; rough equilibrium to 1e-9 V_b; images to 1e-9 V at the surfaces | the belt alone is integrated exactly; backward Euler's global error on a linear decay, the step straddling V_c, and the linear read of the crossing; BE's fixed point is the ODE's; the image series converges to 1e-14 of the first charge | engine only |
| `--kirchhoff` | 8 × 2 × 2^-24 relative | float currents, at most 8 children per node, one rounding each | engine only |
| `--light` | 2·2e^(−2π²σ²) + J/6 + 1.5e-7 + (stores+1)·16·2^-24, σ = 0.8 px | Poisson summation of the kernel at its narrowest; A&S erf; float32 stores worst-case | a pixel check, run at 640×360 and 1920×1080. Another rasteriser changes where pixel centres fall, which the Poisson bound already covers at any phase. A GPU that stored float32 with less precision would need its own term |
| `--exposure` | exact (event counts); pixel light within 1% | a spark is in a window or it is not; 1% only guards the pixel sum, which `--light` holds tighter | two rasters (480×270, 1280×720); the frame a spark lands in is from the clock, not the rasteriser |
| `--determinism` | bit-identical | same program, same inputs | would hold on another GPU run against itself; not across GPUs (float order), and it does not claim to |
| `--over` | ≥ 25% and ≥ 10× chance into the disc; none into it when black | chance is the disc's share of the frame; 10× is a margin a working ground clears easily and a random strike cannot (measured 100%) | through the GPU threshold of a 160-wide mask, so raster-independent in the mask; one raster (640×360) |
| `--onset` | exact | a detector fires or does not | no pixels |
| `--names`, `--defaults` | exact | registration facts | no pixels |
| `--state` | exact | GL state is equal or it is not | this driver only; state restoration is API, not raster |

Every one of these has a wrong model in `--negative` (13 in all), and each
fails as it must. `tools/mutate.sh` changes one character in the shipped
engine or GLSL for seven of them (the CDF, the glow mix, L*, an image charge,
the Laplacian, the growth field, the interrupter) and all seven are caught.

## What is genuinely verified, and what is assumed

Verified on this machine (M4 Max, macOS 26.4.1): everything in the README's
Status table and `tools/verify.sh` green. That covers a fresh universal
build, lipo showing both slices on both bundles, plists, ad-hoc signatures,
`oxbow probe` reading HV01/SW Flyback/source and HV02/SW Flyback Over/effect,
and `oxbow selftest` instantiating each through the host path.

## The constants, checked against their sources (round two)

| constant | status | source |
| --- | --- | --- |
| Peek's law for spheres, g_v = 27.2 δ (1 + 0.54/√(δ r_cm)) kV/cm | **confirmed**, exactly as coded | F. W. Peek, *Dielectric Phenomena in High Voltage Engineering* (1929 ed.), eq. (28) "For spheres" and p. 93; full text on archive.org (`dielectricpheno00peekgoog`). Peek: calculated voltages within 2% for spheres ≥ 2 cm diameter |
| positive streamer propagation field, 5 kV/cm | **confirmed** as the top of the measured range | the "stability field" is 4.5–5 kV/cm; Allen & Ghaffar (1995) measured 4.55 kV/cm; steady propagation at 4.675 kV/cm in X. Li et al., *Plasma Sources Sci. Technol.* 30 (2021), arXiv:2107.06781 |
| Ayrton's own constants (38.88, 2.074, 11.66, 10.54; V, V/mm, W, W/mm) | **confirmed** | H. Ayrton, *The Electric Arc* (1902), eq. (3), for a silent arc between solid carbons at 1.6–14 A; archive.org `electricarc00ayrtrich` |
| the low-current constants A = 350 V, B = 1 kV/m, C = 5 W, D = 750 W/m | **not settled by any source**; D is inside two sourced bounds | upper bound on the column field: a non-thermal atmospheric air glow runs at 1.2 kV/cm up to 22 mA (Mohamed, Block & Schoenbach, *IEEE Trans. Plasma Sci.* 30, 182, 2002), i.e. D ≲ 2400 W/m; the classic 12–15 kV / 20–30 mA ladder has rods 1/4" apart at the bottom and 1–3" at the top (D. Klipstein, donklipstein.com/jacobs.htm), so L* ≳ 3" once bowed. D = 750 W/m gives L* = 13 cm and 0.5 kV/cm at 15 mA. A (cathode fall) is from memory and changes L* by 5% |
| N₂ second positive line weights | **replaced**: now the v'=0 progression weighted by Franck–Condon factor × ν⁴ | q(0,v'') = 0.500, 0.319, 0.101, 0.0488, 0.0247 from R. W. Nicholls, *J. Res. NBS* 65A (1961), table 2. The v'=1 and v'=2 rows as read this session were not legible enough to trust and are left out. The colour moved from (0.162, 0, 0.838) to (0.171, 0, 0.829) |
| N₂⁺ first negative 391.4 nm at 0.2 × 337.1 nm | **assumption, no source**: the ratio depends on the reduced field and published corona spectra span a wide range | 427.8 nm follows from N₂⁺ B–X Franck–Condon factors (0.66, 0.25; from memory) × ν⁴ |
| positive ion mobility in air, μ = 1.76 cm²/Vs | **sourced** (pulsed-corona measurement, 2017, "Measurement of positive ion mobility in air using pulsed corona discharge"); textbook range 1.4–2.2 | sets the corona conductance G linearly |
| Peek's surface factor m_v: 1 polished, 0.82 rough | **sourced for cables, borrowed for a sphere** | Peek 1929, "Visual Corona": 0.82 for "decided" corona on stranded cable (0.72 local). He tabulates nothing for a spun aluminium sphere; 0.82 is the knob's end, not a measured sphere |
| Kim, Sewall & Lin (2007) as the superposition method's source | not re-checked; the method was measured and rejected either way | |

The checks re-derive their expectations from these: `--vdg` works out Peek's
V_b afresh from the formula as quoted above, and `--ladder` bisects Ayrton's
equation with the constants in `physics::Ayrton`. None changed value, so no
expected number moved.

Assumed, or not yet done:

- **Never loaded into Resolume.** How the panel presents, whether the clock
  arrives in seconds or milliseconds (voted on, as rosette and millpond do),
  whether FF_TYPE_XPOS/YPOS pairs show as pads, and what the FFT bins are.
- **Windows never built.** RGBA32F additive blending is standard on DX11-class
  hardware but untested here.
- **The GPU-less CI runner** would fail the pixel checks (it has no GL);
  `ci.yml` only builds, as the fleet's does.
- **No worker thread.** The engine's worst frame is 12 ms (globe) on this CPU.
  On a slower machine that eats the frame.
- **No racing sparks, no corona current, no breakout-point physics** on the
  coil. The globe's glass coupling (held at 0.35) is a model value.
- **OpenFX port, browser demo, user guide: not done** (not required for 0.1.0).

## Conventions

Tabs. British spelling in prose. Comments explain why, and especially what
goes wrong. Local repo: no remote, no tag, not on the website.
