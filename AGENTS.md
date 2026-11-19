# flyback — for agents

The why behind the code. `CLAUDE.md` has the commands; this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-09-23 in one session, from Allan's request for "a new resolume
plugin/source which recreates high voltage electric effects, such as Jacob's
ladders, tesla coils, van de Graaff generators etc." The spec is
`~/Projects/resolume/specs/SPEC-flyback.md`; the shared brief is
`~/Projects/resolume/specs/BRIEF.md`.

Public at github.com/stoatworks-labs/flyback and registered with the
website. Released at **v0.1.0** on 2026-09-24 and **v0.2.0** on 2026-11-19
(the singing coil, below, and its first Windows build). CI (`ci.yml`) and the
tag-driven `release.yml` run on GitHub; the repo is public, so Actions cost
nothing. `deploy.yml` redeploys the browser demo on push to main.

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
    source/Singing.*          the singing coil: earshot's pitch -> the interrupter's rate; the menu; the law
    source/render/Lamp.*      the singing coil's status lamp (not in Shaders.cpp: the demo copies that file)
    external/earshot/         earshot 8124191, a `git archive` snapshot (SNAPSHOT says so); never edited here
    source/{Source,Effect}Plugin.cpp   the two registrations, one per bundle
    tools/hvtest/             the harness: checks, --negative, --offline, --bench, --film/--pipe
    tools/hvtest/Singing.inc  the singing coil's checks, --demo-music, --trap-probe
    tools/catrap/             a DYLD_INTERPOSE trap on every CoreAudio call: the mode-off proof
    tools/sweep.py            no control is silently dead
    tools/mutate.sh           one-character mutants must be caught
    tools/glslc.sh            every shader through glslc (verify.sh and CI)
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
- **The engine runs on a worker thread, one frame late** (64c21d2; this
  entry said "render thread" until 2026-09-24). A worker cannot run ahead,
  because it would need the next frame's time before the host gives it, so it
  is pipelined: `ProcessOpenGL` for frame n waits for the step started at
  n-1, draws it, and submits frame n's. The engine sees the same calls in the
  same order as synchronously, so `--determinism` requires the worker's
  frames to equal the synchronous ones one frame earlier, bit for bit. Cost:
  the picture is a frame behind the clock, and the first frame shows the
  apparatus with no light. Mean engine cost is 1–5.8 ms, the worst 11–12 ms
  (the globe). The README's bench table predates this change.
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
committing: only `Diag.cpp` may have one, and `Singing.cpp`'s
`FLYBACK_NO_DEVICES` (woofer's and rubens's gate, which verify.sh's oxbow runs
set: it is a contract, not a debug switch).

**A shared machine's load is in the GPU numbers.** The first bench, with the
load average at 13, put three machines at 11–12 GPU ms at 4K. Run alone they
were 2.6–3.1 ms. `--bench` prints the load average, and the README's table is
from a quieter run (load ~8).

## Would this hold on another rasteriser, at another raster?

One row per check. "Rasters" is where it runs: its development raster(s), and
the 320x180 pass (`--size 320x180`, CI's raster, run by `tools/verify.sh` with
its negative controls). Every pixel check held at 320x180 on the first run, so
none needs a "cannot hold" note. Tolerances are derived (a float ULP, a lattice
step, a frame, a kernel's sampling error), never fitted to what this Mac printed.
The singing coil's four GL checks also run on Apple's software renderer
(`HVTEST_RENDERER=software`, verify.sh) at both rasters. There `--off` first
failed on pixels, which is how that renderer's nondeterminism was found (the
singing coil's traps).

| check | bound | why that number | rasters | another rasteriser? |
| --- | --- | --- | --- | --- |
| `--laplace` | per site, h·(\|ln(r/R2)\|/R1 + \|ln(r/R1)\|/R2)/L² + solver residual | a staircase electrode's effective radius is uncertain by one lattice step; that times the profile's sensitivity to each radius | none: a lattice in metres, at two steps (10.4 and 5.2 mm), where the bound scales with h. `--offline` | no pixels: holds on any GPU, or none |
| `--dimension` | 1.70 ± 0.05; η = 0 > 1.9; η = 6 < 1.25; monotonic | the spec's and literature's band, and 3 SE on the DLA reference | none (engine only). `--offline` | no rasteriser enters. Floating-point order could change one site's choice; the statistic would not notice |
| `--ladder` | L* to one lattice step (the column measured on a polyline refined to h/4); climb time to one frame | the arc cannot be located more finely than the lattice it lives on; the spec's "± one frame" | none (metres and seconds). `--offline` | raster-independent by construction |
| `--tesla` | bang count exact; lengths at 3 SE, the SE widened by √(τ·BPS) for bangs sharing channel | a clock is exact; the memory claim is statistical | none (engine only). `--offline` | no rasteriser enters |
| `--vdg` | polished interval to 1e-9 s; corona interval to dt/(2e)·(V∞−V_c)/(V∞−V_b) + 1.125 dt²/τ; rough equilibrium to 1e-9 V_b; images to 1e-9 V at the surfaces | the belt alone integrates exactly; backward Euler's global error on a linear decay, the step straddling V_c, the linear read of the crossing; BE's fixed point is the ODE's; the image series converges to 1e-14 of the first charge | none (engine only). `--offline` | no rasteriser enters |
| `--kirchhoff` | 8 × 2 × 2^-24 relative | float currents, at most 8 children per node, one rounding each | none (engine only). `--offline` | no rasteriser enters |
| `--defaults`, `--names` | exact | registration facts | none. `--offline` | no pixels |
| `--light` | 2·2e^(−2π²σ²) + J/6 + 1.5e-7 + (stores+1)·16·2^-24, σ = 0.8 px (≈ 4e-5) | Poisson summation of the kernel at its narrowest; A&S erf; float32 stores, worst case | 640×360 and 1920×1080 (worst 1.9e-5); **320×180: 2.4e-5 and 1.7e-5**, the same bound, because σ is in pixels at its narrowest at every raster and the glow's octaves are placed in the frame, not in pixels | another rasteriser moves pixel centres, which the Poisson bound covers at any phase. Apple's software renderer steps interpolants by ~1e-5 relative (inside GL 4.1 §2.1.1); that is not in the bound, and this check has never run there (CI does not run it). A GPU storing float32 with less precision would need its own term |
| `--exposure` | event counts exact; each frame's pixel light within 1% of its events' | a spark is in a window or it is not; 1% guards only which frame the light is in, which `--light` holds to 4e-5 | 480×270 and 1280×720; **320×180: 95 of 95 at 360°, 48 of 95 at 180°, pixels agree in all 240 frames** | the frame a spark lands in is from the clock, not the rasteriser |
| `--determinism` | bit-identical | same program, same inputs | 480×270; **320×180: bit-identical, worker = synchronous one frame late, all three machines** | holds on another GPU run against itself; not across GPUs (float order), and it does not claim to |
| `--over` | ≥ 25% and ≥ 10× chance into the disc; none into it when black | chance is the disc's share of the frame; 10× is a margin a working ground clears and a random strike cannot | 640×360; **320×180: 600 of 600 into the disc, 152 of 152 to the coil's grounds when black** | the clip is thresholded onto a 160-wide mask whatever the raster, and strikes are counted in scene metres to one lattice step |
| `--onset` | exact | a detector fires or does not | 320×180 (its development raster; also run with `--size 320x180`) | no pixels are read |
| `--state` | exact | GL state is equal or it is not | 320×180 (as above); the viewport it hands over is the raster's | this driver only; state restoration is API, not raster |
| `--pipe` / `--film` (verify.sh) | exact byte counts and exit codes | 2.5 frames in → 2 out, exit 0; unknown cue → exit 2; closed stdout → exit 1 (SIGPIPE ignored) | 64×36; with the singing mode on (`--tone 220`) too | not a pixel check |
| `--sing` | bangs in [a, b] = ⌊∫r dt⌋, i.e. within (∫ − 1 − ε, ∫ + ε], ε = 64·2^-53·∫, and none before a; every voiced frame within 1 cent; every rate exactly hz·2^(oct+k) inside [5, 1000] and folded no further than needed; a glide's every interval integrates to 1 cycle within 2(2·steps·(2 + 1000/24)·2^-53 + 2000·ulp(t)) | a phase accumulator from 0 crosses ⌊integral⌋ cycles; 1 cent is earshot's own limit for a sine (its CheckPitch `pitch.sine`: ten times MPM's parabola bias), not ours to tighten; the glide bound is the double arithmetic's, term by term | none: engine and pitch input, 60 fps. `--offline` | no rasteriser enters |
| `--sing-clock` | counts exact, ⌊rT⌋, at 24/30/60/144 fps and 120 irregular steps; each bang at k/r within 4(2·2^-53·(rT + steps)/r + ulp(T)); at 499,000 s the same interval bound with ulp(origin + T) | the phase only ever sees t1 − t0, exact by Sterbenz; what remains is the accumulator's and the time's rounding | none (engine). `--offline` | no rasteriser enters |
| `--sing-memory` | sung streamer mean = the BPS coil's at the same rate to 3 SE (the SE widened as `--tesla`'s); flat while 1/rate ≫ τ, longer each octave by 3 SE | `--tesla`'s memory claim and tolerance, the sung rate in place of BPS | none (engine). `--offline` | no rasteriser enters |
| `--device` | exact | counts of lists, selects and engines; the menu's keys by name | none (scripted capture devices, no CoreAudio). `--offline` | no pixels |
| `--sing-picture` | each frame's light a whole number of bangs within 2× `--light`'s bound a bang (7.9e-5 to 8.1e-5); the bang count within one of ∫r dt | `--light`'s derived bound; the phase | 480×270 and 960×540 (worst 1.1e-5 to 4.1e-5); **320×180 the same bound**; Apple's software renderer at both: worst about 1e-6 | the bound covers any pixel phase; software renders pass it, with differing frames run to run (traps, below) |
| `--off` | the engine's Frame bit-identical to the defaults' on all 90 frames with the new controls moved; the pixels bit-identical on a renderer that reproduces itself; zero earshot engines, listings, selects, feeds and updates | the mode off must be the v0.1.0 plugin | 480×270 and 320×180, source and effect | pixels are not claimed on Apple's software renderer, which does not reproduce its own output; the Frame comparison holds everywhere |
| `--lamp` | the lamp's centre pixel within 1/255 of its state's colour; the bezel inside the frame; GL state as `--state` | the colour is a constant written once; 1/255 is one 8-bit step | 480×270 and 1280×720 (radius 3.2 and 8.6 px); **320×180: 3.0 px**; software renderer at both | sized from the viewport's height, so it holds at any raster |
| `--resize-sing` | the count across a resize within one of ∫r dt; no new engine, listing or select; the lamp on every frame | a resize must not touch the pitch input | 480×270, then the host's framebuffer reallocated at 640×360, then DeInitGL/InitGL | the raster changes inside the check |
| CoreAudio trap (verify.sh) | 0 hits in 13 mode-off processes (oxbow probe and selftest on both bundles, `--off`, `--defaults`, `--list`, a film of each machine, a piped effect); `--trap-probe` must leave its hits, and refuses to run without the trap | the mode off never reaches CoreAudio; a trap that cannot fire proves nothing | 160×90 films | not a pixel check |

Every physics and pixel check above has a wrong model in `--negative` (31 in
all: two each for ladder, tesla and vdg; seven for `--sing`, two for
`--sing-clock`, two for `--off`, one each for the other singing checks), and
each fails as it must at the development rasters and at 320x180. `--state`, `--defaults` and `--names` have
none: they compare API facts, not a model.

### The recorded mutation (the harness drives the GLSL it ships)

`tools/mutate.sh` changes one character of the shipped source in a copy of the
tree (one substitution, insertion or deletion: an edit distance of 1), rebuilds
`hvtest`, and requires the named check to fail within `MUTANT_SECONDS` (300 s;
a timeout counts as caught, and says so). Two of its eighteen mutants change
the shipped GLSL in `source/render/Shaders.cpp`, and one the lamp's:

| file | original | mutant | caught by |
| --- | --- | --- | --- |
| `Shaders.cpp` (the segment kernel's normal CDF) | `return 0.5 * ( 1.0 + erfAS( ... ) );` | `return 0.6 * ...` | `--light` |
| `Shaders.cpp` (the core/glow mix) | `light = ( 1.0 - Glow ) * e + Glow * g;` | `( 1.0 + Glow )` | `--light` |

The other seven of the first nine are the engine's: L*'s linear coefficient
(`--ladder`), an image charge's side (`--vdg`), the Laplacian's centre weight
(`--laplace`), the growth field's sign (`--dimension`), the interrupter's beat
(`--tesla`), the corona conductance and the corona's drain voltage (`--vdg`).
The singing coil's nine:

| file | original | mutant | caught by |
| --- | --- | --- | --- |
| `Engine.cpp` (the sung phase) | `phase + r * ( b - a )` | `phase - r * ...` | `--sing` (8 checks) |
| `Engine.cpp` (a sung bang's time) | `a + ( k - phase ) / r` | `( k + phase )` | `--sing-clock` (5) |
| `Engine.cpp` (a rest) | `phase = 0.0;` | `phase = 1.0;` | `--sing` (7) |
| `Singing.cpp` (the fold) | `r *= 0.5;` | `r *= 0.6;` | `--sing` (2) |
| `Singing.cpp` (the Gate) | `levelDb ) >= gateDb` | `<=` | `--sing` (11) |
| `Singing.cpp` (sustain) | `( !above && ...` | `( above && ...` | `--sing` (2) |
| `Singing.cpp` (the menu key) | `h *= 16777619u;` | `16777618u` | `--device` (2) |
| `Singing.h` (on or off) | `applied > 0.0f` | `>=` | `--off` (3) |
| `Lamp.cpp` (the disc) | `clamp( ..., 0.0, 1.0 )` | `0.0, 0.0` | `--lamp --size 320x180` (3) |

Last run 2026-11-16: 18 mutants, 18 caught, none by the time limit (159 s in
all). The first run of the singing nine caught 6: the rest mutant passed
`--sing` (traps, below), and two mutants were refused by the old positional
diff (an insertion or deletion counted as every character after it).

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

- **Resolume, briefly.** Arena 7.29.0 on this Mac (2026-11-19, a local
  universal build beside the signed v0.1.0): both register (58 / 59 REST
  controls), BPS-off frame and Over at Mix 0 pixel-identical to v0.1.0's, the
  clock votes milliseconds, and Resolume's FFT reaches Audio Fires (Beat
  001.wav, Audio on Composition, BPS off: bangs at 1, none at 0). Pitch Source
  was never switched on there (every choice above Off opens a real device).
  Arena 7.27.1 on win-lab: the gate (README's Status). Its single-frame grabs
  can't reliably see the intermittent controls (Rod Length, Rise Speed, Target
  Y, the Over's Sphere Finish): which land inconclusive, or fail, changes from
  run to run, and the harness shows each one acting. Still unknown: how the
  panel presents, whether the X/Y pairs show as pads (REST calls them
  ParamRange), and what the 64 bins hold for a known tone.
- **Windows is built by CI only** (MSVC, `ci.yml` and `release.yml`), and run
  only in Arena on win-lab's software renderer (the README's Status). RGBA32F
  additive blending is standard on DX11-class hardware but untested on one.
- **The GPU-less CI runner** cannot create an accelerated GL context, so
  `ci.yml` runs `hvtest --list`, `hvtest --offline` (the physics and
  registration checks and their negative controls, no GL context) and
  `tools/glslc.sh --require`. The pixel checks, and anything only a real driver
  catches, run in `tools/verify.sh` on a Mac with a GPU and nowhere else.
- **Finger in the Over does nothing over a bright clip** (found by the Arena gate
  2026-11-19, v0.1.0's code). `GlobeMachine::Finger()` moves the finger to the glass
  nearest the centroid of the clip's ground cells, which already ground the field there,
  so Finger, Finger X and Finger Y leave every frame identical over Trinity's still
  (hvtest `--pipe --effect`, 120 frames, 0.0 difference); over black they act (8-10
  levels). sweep.py only sweeps them in the source. The guide says so; the model is
  unchanged, and plugin-bench's expectation marks the effect's three inert with this reason.
- **The worker thread has met a host only briefly** (the two Arena sessions
  above, nothing timed). The engine's worst step is
  12 ms (globe) on this CPU; overlapped with the host's work that is hidden
  here, but on a slower machine it still eats the frame.
- **No racing sparks, no corona current, no breakout-point physics** on the
  coil. The globe's glass coupling (held at 0.35) is a model value.
- **OpenFX port: not done** (not required for 0.1.0). The browser demo is below.

## The browser demo (2026-09-24)

`demo/` is <https://flyback-demo.stoatworks-labs.com>, built to the fleet's
`resolume-demo` kit rules. What a reader of it must know:

- **The shaders are the plugin's**, all nine, copied unedited;
  `demo/tools/check_shaders.py` compares them (and `kGlowWeights`) to
  `Shaders.cpp` character for character and `tools/verify.sh` runs it.
- **The CPU half is a port that only a reader checks.** `demo/engine.js`
  translates Rng, Physics, Lattice (PCG + multigrid), Dbm (Fenwick candidates,
  quenched disorder, arcs), Tree, all five machines, Controls and Presets, at
  the plugin's own lattice sizes -- nothing reduced. It is not bit-exact: C++
  `float` rounds every operation, the port rounds only on a Float32Array
  store, so a seed grows a discharge of the same model, not the same one. On
  2026-09-24 its closed forms matched the README (L* 0.1338 m, V_b 189.6 kV,
  C 18.56 pF, interval 0.3519 s, air 0.171/0/0.829), every readout matched
  `hvtest --list`, and each preset's frame looked like hvtest's. Change the
  engine, change the port.
- **Differences, all said on the page:** the engine runs synchronously on the
  page's thread (not a worker a frame late); no Resolume FFT (the Audio
  buffer, Audio Fires and Audio Drive are absent -- with no spectrum the
  plugin does the same); Pitch Source is Off / the page's test tune / the
  visitor's file, and Rescan Inputs is absent; Fire is a toggle the page releases; XPOS/YPOS are sliders; no About
  block; WebGL2 has no CLAMP_TO_BORDER, so the glow levels clamp to edge; Over's
  mask is a synchronous readPixels, still a frame late; it needs float
  render, blend and linear filtering, and refuses without them.
- The Over build strikes into the kit's generated clips (Lights on black
  first). The Plugin switch is a new instance: engine and history restart.
- **The singing coil (2026-11-19): the pitch input is compiled, the
  interrupter is ported.** `demo/flyback-sing.wasm` is `source/Singing.cpp` +
  `Diag.cpp` with earshot's snapshot (`EARSHOT_CAPTURE=OFF`: NoCapture.cpp, no
  miniaudio), built unmodified by `demo/tools/build-wasm.sh` (emscripten
  6.0.5, `-DFLYBACK_SINGING=1`, reproducible) behind `demo/wasm/glue.cpp`,
  which runs `FLYBACK_NO_DEVICES=1` and feeds through `PitchInput::Feed` as
  hvtest does. `demo/wasm/inputs.sha256` hashes every input (from the
  compiler's dependency files) and both outputs; `check_shaders.py` fails on a
  changed hash, an earshot source not compiled, an executable output, a served
  file naming a capture API, or drift in Lamp.cpp's shader (copied into
  `demo/singing.js`). The sung interrupter (Engine.cpp's StepSung) is a hand
  port in `engine.js` that only a reader checks. The page feeds a 12.25 s
  sine-tone tune it synthesises (`demo/tune.js`) or a visitor's file decoded
  by an OfflineAudioContext, against the page clock, looped; no audio out, no
  microphone, no device. Fed the tune once at 60 fps, the wasm and hvtest's
  own objects agree on every frame's voiced, sounding and lamp state (736
  frames), with held rates within 1.1e-6 relative (0.002 cents: no FMA in
  wasm, another libm); the page's path (wasm + JS interrupter) reproduces
  `hvtest --demo-music`'s summary (76.8 / 77.3 / runs of 63.2 / median 329.63)
  and its 3615 bangs. With Pitch Source off, `engine.js` gives frames
  bit-identical to the port before singing (2400 frames: five machines, every
  preset). The page's photosensitivity note carries the plugin's measured
  flash rates (README), not the page's own.

## The singing coil (2026-11-16, after v0.1.0)

Allan's request: the Tesla coil's interrupter firing at the detected note's
pitch, from earshot, instead of at BPS. That is what a musical coil is: its
interrupter fires once per period of the note, so the crack of each bang is
the note (Kaizer's musical interrupter, the UD3, Syntherrupter; the spec is
`~/Projects/resolume/specs/SPEC-flyback-singing.md`, with the sources).

**Pitch Source** (Off, System default, then capture devices), **Rescan
Inputs**, **Octave** (−4 to +2) and **Gate** (−100 to −20 dBFS, −60 in the
middle, earshot's own pitch gate) are appended after the About block as ids
54–57, in their own group, Singing. Nothing above them moved: a v0.1.0
composition stores parameters by index and loads with the mode off.

**earshot is a snapshot**, `git archive 8124191` under `external/earshot`,
unmodified (`SNAPSHOT`), eidophone's arrangement. earshot is public at
github.com/stoatworks-labs/earshot (SNAPSHOT's `Source:` line); verify.sh
compares the snapshot file for file with the checkout at
`~/Projects/resolume/earshot` (or `$EARSHOT`) when it exists, and requires this
file to name the commit. Do not patch it here; change earshot and take a new
snapshot.

### The law, decided from the demo music

`hvtest --demo-music "/Applications/Resolume Arena/media/AV"` runs Resolume's
eighteen demo WAVs (six bass, six beats, six synths, 7.5 s each) through the
real pitch input and the real sung interrupter, under the law and under
"voiced frames only". At the default Gate (−60 dBFS), 60 fps:

| clips | voiced frames | voiced-only coil: sounds, mean run | the law: sounds, mean run |
| --- | --- | --- | --- |
| Beat 001–006 | 2.9–19.1% | 2.9–19.1%, 1.2–3.7 frames | 88.9–98.7%, 401–445 frames |
| Bass 001–006 | 22.4–89.8% | the same, 6.7–28.9 frames | 91.1–98.9%, 205–446 frames |
| Synth 001–006 | 19.3–97.1% | the same, 4.1–62.6 frames | 33.5–98.9%, 34.6–446 frames |

A voiced-only coil flickers: on the beats it is dark 81–97% of the time and
lit in bursts of one to four frames. So:

- **A voiced frame sets the note** (earshot: clarity ≥ 0.80, level ≥ Gate,
  40–2000 Hz).
- **An unvoiced frame above the Gate sustains it.**
- **Below the Gate is a rest, and it forgets the note**: the next sound waits
  for its own pitch. A new stream (another device, a restart) forgets it too.
- **The rate is hz·2^Octave, folded by whole octaves into 5–1000**, the BPS
  control's own range (`ldexp`, so the fold is exact).

The cost of the law: on unpitched material the coil holds a pitch heard on a
few per cent of frames through the whole loop, because a loop's tail stays
above −60 dBFS between hits (Beat 001: voiced on 2.9% of frames, sounding on
94.7%). **The Gate is the lever**: the report's second table raises it, and
at −30 dBFS the beats sound on 8.0–98.0% of frames in runs of 3.6–442 frames
(Beat 002 stays loud throughout). The user guide says to raise the Gate for
drums.

**In MIDI terms**: note-on is the first voiced frame after a rest; a later
voiced frame at another pitch is a legato change of note (the interrupter's
phase runs on, no restart); note-off is the level falling below the Gate. An
FFGL plugin gets no MIDI, so this is a description, not an input. Resolume can
map a MIDI note to any parameter (Gate, Pitch Source) the way it maps any
control; that has not been tried.

### Decisions taken without asking

- **The interrupter is a phase accumulator in double**, in cycles: bangs fall
  where ∫r dt since the last one reaches 1, so a rate that changes every frame
  is followed without a leap. The rate is held for each frame's step (a glide
  is a staircase at the frame rate; `--sing`'s glide bound is for that
  staircase). Only t1 − t0 enters the phase, never the clock's value, and
  t1 − t0 is exact (Sterbenz), so at half a million seconds the phase
  resolves what it does at zero (`--sing-clock`).
- **The clock-overflow answer.** The clock is a double in seconds. At
  499,000 s (5.8 days) `--sing-clock` measures the bang times to 4.4e-11 s
  against a bound of 2.3e-10. The bang time's ulp grows with the clock: at
  10^9 s (31.7 years) it is 1.2e-7 s, still 10^4 below the shortest period
  (1 ms). The phase is in [0, 1) and never grows. earshot's Frame reports
  audio time as a double and its counters as 64-bit. A float clock would not survive: `kSingHookFloatClock` rounds
  the step times to float, and `--sing-clock`'s negative control fails.
- **A rest zeroes the phase, and so does entering the mode**: a note's first
  bang is a whole period after it starts, never at once (a bang owed to the
  silence would be a click that is not in the music).
- **Folding, not on-time reduction.** A real coil keeps a high note by
  shortening each pulse (Syntherrupter multiplies every voice's on-time by one
  factor once the summed duty passes the coil's limit). Here every bang carries
  ½C_pV², the BPS coil's bang, so the only lever is the rate, and a note above
  1000 is folded down by octaves (pitch class kept). 1000 BPS at a 100 µs bang
  is the 10% duty the musical-interrupter designs give (Kaizer; the UD3's
  `max_tr_duty` default). The UD3's own PRF ceiling default is 800 Hz; 1000 is
  the BPS control's existing top.
- **Channel memory is unchanged.** A sung bang is a bang: the streamers re-use
  hot channel exactly as at that BPS (`--sing-memory`: sung equals BPS at the
  same rate, flat at 6.875 and 13.75, then longer each octave). A high note
  throws long streamers and a low one short, fresh ones.
- **CoreAudio, never, until asked.** earshot's `listDevices` and
  `selectDevice` both start CoreAudio (earshot's finding), so with Pitch Source
  Off the plugin makes no earshot engine, lists nothing and selects nothing,
  and the menu holds Off, System default and fourteen "(none)" spares. The
  menu's length (16) is fixed at declaration; its elements are refilled when
  the mode is first switched on, and by Rescan, which acts only while the mode
  is on. eidophone lists at construction; here that is `kHookEagerDevices`,
  and `--off` fails it.
- **The earshot library is still linked**, and its PUBLIC framework link puts
  CoreAudio and AudioToolbox in the bundle's load commands (`otool -L`): the
  images load with the plugin even with the mode off. The trap counts calls,
  not loads, and finds none. A build with `-DFLYBACK_SINGING=OFF` links
  neither.
- **FLYBACK_SINGING is ON on every platform since v0.2.0.** It was OFF off
  macOS until then for one reason: earshot had never been built by MSVC. It
  has since (needle's CI compiles this snapshot, and woofer, rubens, eidophone
  and marbling ship Windows DLLs with it), and flyback's own CI compiles it
  cleanly. `-DFLYBACK_SINGING=OFF` still builds a stub that declares the same
  parameters, so a composition loads the same, and the lamp says no device
  can open. hvtest requires it ON.
- **The lamp** is a disc in a dark bezel, top left, drawn last and only while
  the mode is on, on any machine: green Running, red Silent (bit-exact zeros
  for 3 s: no permission, an unpatched input), blinking red Missing, blinking
  amber while opening. spasis's and earshot's colours, as rubens and eidophone
  show them. It is in `render/Lamp.cpp`, not `Shaders.cpp`, because the demo
  copies `Shaders.cpp` character for character.
- **BPS displays "sung"** on the Tesla coil while the mode is on.

### Departures from a real singing coil

- **One voice.** earshot's MPM finds one pitch. Kaizer's square-wave
  modulator "is not able to express a chord"; Syntherrupter plays up to 16
  voices a coil. A chord here sounds as whichever pitch MPM finds, or as
  unvoiced (a sustain).
- **Every bang the same energy.** No on-time, so no velocity or volume: a real
  coil's pulse width is its volume control (UD3 `vol_mod`, Syntherrupter's
  on-time). Light per second goes up with the note, as with BPS.
- **From audio, not MIDI.** The interrupters read for the spec are driven by
  MIDI or a SID (the UD3's `synth`), or by a knob. Pitch tracking a live input
  into an interrupter is this plugin's own idea; no source was found for it.
- **No minimum off-time, no duty limiter**, because nothing here can overheat:
  the fold to 1000 stands in for both.

### The traps

**Apple's software renderer does not reproduce its own output.** The Van de
Graaff's 90-frame film at 480x270 gave 2 different hashes in 3 runs on
`kCGLRendererGenericFloatID`, 1 on the GPU. So `--off` compares the engine's
Frame bit for bit everywhere and the pixels only on a renderer that renders
the same frame twice (`ReproducibleRenderer`), and says so when it skips them.
`--sing-picture` holds its bound there anyway (worst about 1e-6).

**earshot's `selectDevice` is asynchronous.** The device worker opens it and
sets Opening; the frame after a select can still say Waiting. `--device`
polls with a deadline (`UpdateUntil`) instead of expecting the next frame.

**A double select on first switch-on**, found by `--device`: the first Apply
listed the devices, re-selected the remembered key, and then selected the
asked-for one. A select ends a feed and reopens. `Refresh( reselect )`:
Start lists without re-selecting; only Rescan re-selects.

**`--sing` counted bangs in (a, b], and a bang at exactly a is the one a rest
must prevent.** The rest mutant (`phase = 1.0`) puts the first bang at the
note's first step, at a, and passed. The count is now over [a, b] and
requires none before a.

**mutate.sh's positional diff** refused insertions and deletions (an inserted
`=` counted as seven differing characters). It is now an edit distance.

**A sustained note is held from the last voiced frame**, whose window already
has the noise in it: the held pitch was +7.1 cents off a 220 Hz tone.
`--sing` reports the held range and asserts only that it sounds.

**Apple's software renderer is slow**: `--lamp` at 1280x720 took over 5 s,
so its render guard is 60 s. verify.sh runs the four GL singing checks there
at both rasters (at 320x180 alone: `--sing-picture` 29 s, `--off` 25 s,
`--resize-sing` 9 s, `--lamp` 1.5 s).

**perl strips DYLD_* (it is a SIP binary)**, so the trap runs exec hvtest and
oxbow directly with `DYLD_INSERT_LIBRARIES`, never through perl's alarm.

### Measured

| | |
| --- | --- |
| the count | 110 Hz: 322 bangs where ∫r dt = 322.667; 440 Hz: 1290 (1290.667); 1318.51 Hz folded to 659.26: 1933 (1933.833); 55 Hz at −3: 20 (20.167); 220 Hz at +2: 2581 (2581.333); 98 Hz at −4: 17 (17.967) |
| the pitch | worst 0.002–0.022 cents from the tone on every voiced frame (earshot's limit 1 cent) |
| a glide | 200→400 Hz in 2 s, 116 distinct rates: 556 intervals, worst 7.7e-14 of a cycle off 1 (bound 9.7e-13) |
| rest, Gate, sustain | 0 bangs 1.092 s after a tone ends; the forgotten note on 0 of 30 frames; a −70 dBFS tone silent under a −60 gate, sounding 86 of 91 frames under −80; noise above the gate sustains on 54 of 54 frames |
| frame-rate independence | exact counts at 24, 30, 60 and 144 fps and 120 irregular steps; worst bang time 2.2e-16 to 6.7e-16 s (bounds 5.8e-15 to 7.6e-14) |
| clock | at 499,000 s: worst 4.4e-11 s, bound 2.3e-10 |
| memory | sung = BPS at each octave: 0.255 m at 6.875 and 13.75, then 0.849, 1.020, 1.273 m |
| picture | 330, 989 and 660 bangs counted from pixels; worst per bang 1.1e-5 to 4.1e-5 on the GPU, about 1e-6 in software, against `--light`'s 7.9e-5 to 8.1e-5 |
| mode off | `--off` 0 differing frames; against origin/main's hvtest, 36 of 36 renders byte-identical (films of each machine and preset, with `--beat` and Audio Fires/Drive, a 260-frame cue sheet switching machines, 640x360, the effect piped for each machine and Detect On, PNGs with Fire); every existing check's text identical but `--names` (54 → 58) and `--dimension`'s wall-clock line; `oxbow selftest` identical but the four appended parameters (the effect's rgb sum differs run to run on origin/main too) |
| CoreAudio | 13 mode-off processes under the trap: 0 calls; `--trap-probe`: the dlopen and the AudioObject call logged |
| cost | ms/frame, M4 Max, load ~8–9: the coil singing 220 Hz costs what the coil at 220 BPS costs (engine 7.2 ms mean, GPU 1.4 ms at 720p, 2.4 at 1080p); the render thread goes from 0.5 to 1.1–1.3 ms mean, which holds earshot's `update`, the lamp, and the harness feeding the tone (an audio thread's work in a host) |
| dead controls | Pitch Source 4.8, Octave 10.8, Gate 4.2 (sweep units); 51 swept, none dead |

### Not verified

- **Never in Resolume**, and never on a real capture device in a check
  (verify.sh uses earshot's scripted backend; `HVTEST_REAL_DEVICES=1` exists
  but no claim rests on it). How Resolume shows a 16-element menu with
  fourteen "(none)" spares, and whether it re-reads refilled elements, is
  unknown.
- **Microphone permission.** A host without it gets bit-exact zeros, which
  the lamp shows as red Silent after 3 s (earshot's rule). Not tried.
- **Windows**: the mode is built (FLYBACK_SINGING ON since v0.2.0, by CI's MSVC), and has
  never heard an input there: win-lab, where Arena runs it, has no sound device. Switched on
  there anyway (2026-11-19, Pitch Source = System default): WASAPI listed 0 devices, earshot
  reported "could not open the default input" and retried, the lamp lit, Rescan, Octave and
  Gate moved without harm, Off put the picture back, and Arena stayed one process.
- **The browser demo's singing coil**: its pitch input is the plugin's C++ in
  WebAssembly, compared with hvtest only on the page's test tune at 60 frames
  a second (never at 120 Hz or a variable frame rate, never on a visitor's
  file); its interrupter is a hand port that only a reader checks. Tried only
  in Chromium. The page's own flash rate is not measured (the plugin's is).

## Conventions

Tabs. British spelling in prose. Comments explain why, and especially what
goes wrong. Public repo, released (v0.1.0, v0.2.0), on the website.
