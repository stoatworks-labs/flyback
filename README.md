# flyback

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. It's **never been loaded into Resolume
> on macOS**. On Windows it loads and renders in Resolume Arena, on software rendering.
> Everything else is measured by an offline harness that drives the real plugin class in a
> headless GL context, or the engine on its own where a claim doesn't need a GPU, and every
> check is also run against a wrong model to make sure it can tell. The numbers, and the list
> of what I haven't tried yet, are under [Status](#status).

The high-voltage corner of a science museum, for Resolume Arena and Avenue: a Jacob's ladder, a
Tesla coil, a Van de Graaff generator, a plasma globe and a Lichtenberg figure. It comes as two
FFGL plugins, a source (`SW Flyback`) and an effect that strikes into your clip
(`SW Flyback Over`), and nobody has to stand behind the cage.

![A Tesla coil firing: violet streamers branching off the toroid into the air, and one white-hot strike down to the grounded base](docs/hero.png)

<sub>The Tesla coil as it starts up: an NST coil at 120 bangs a second, streamers into the
air and one striking the grounded base. Rendered by the plugin's offline harness (`hvtest`)
rather than captured from Resolume.</sub>

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/flyback/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`flyback-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/flyback/releases/download/v0.1.0/flyback-0.1.0-macos-universal.dmg) | 712 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`flyback-macos-universal.zip`](https://github.com/stoatworks-labs/flyback/releases/latest/download/flyback-macos-universal.zip) | 641 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`flyback-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/flyback/releases/download/v0.1.0/flyback-0.1.0-windows-x86_64-setup.exe) | 286 KB |
| x64 · .zip archive | [`flyback-windows-x86_64.zip`](https://github.com/stoatworks-labs/flyback/releases/latest/download/flyback-windows-x86_64.zip) | 347 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/flyback/releases](https://github.com/stoatworks-labs/flyback/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## The one idea

**Air is an insulator until the field tears it, and where it tears is a
Laplace problem.** Every discharge grows by the dielectric breakdown model
(Niemeyer, Pietronero & Wiesmann, 1984). The electrodes, and whatever channel
has grown so far, are held at their potentials. The potential in the air
solves Laplace's equation on a lattice sized in metres of scene, and the
channel then grows to a neighbouring site with probability

    p ∝ |∇φ|^η

One exponent decides the look. η = 1 is a Lichtenberg figure, η ≈ 2 to 3 is
lightning, and a large η is a nearly straight arc. None of it's drawn with a
noise function. The branching, the forks heading for the nearest ground and a
strike taking the short way round all happen where the field puts them.

Around that model sit the circuits of the classic machines, and each one
decides *when* the air breaks and *how much energy* the channel carries:

- **Jacob's ladder.** The arc obeys Ayrton's equation on a supply with a
  source resistance, so it has an extinction length **L\***, solved in closed
  form (the harness also finds it by bisection, and the two agree to 1e-9 m).
  The hot column rises at the buoyant speed, its roots lag on the rods and it
  bows. It stretches as the rods spread, snaps at L\* and restrikes at the
  bottom. A stiffer supply climbs further.
- **Tesla coil.** It bangs at the interrupter rate, on the dot, like a clock.
  Each bang grows streamers off the toroid towards the nearest ground.
  **Channel memory:** a segment stays hot for τ times its share of the current
  (a thicker channel cools slower), so the next bang re-uses whatever's still
  hot, and the streamers get longer with BPS once 1/BPS < τ. A slow interrupter
  makes short, fresh sparks every time.
- **Van de Graaff.** The belt charges the sphere at I_belt. The gap breaks when
  the sphere's surface field reaches Peek's value, worked out from the
  two-sphere image-charge solution, and the spark dumps ½CV². So sparks come
  every **C·V_b/I_belt**, twice as often at twice the belt current. Sphere
  Finish sets Peek's surface factor m. A polished sphere (m = 1) has no corona
  before sparkover, as Peek found. A rough one goes into corona at m·V_b and
  leaks I_c = G(V − V_c) to the room, with G derived from ion drift. G is tens
  of nA per volt against a belt of microamps, so the corona doesn't so much
  slow the climb as stop it: the sphere sits at V_c + I/G, and held there below
  V_b it never sparks, it just glows.
- **Plasma globe.** Filaments grow from the electrode to the glass one at a
  time. Every frame the outer part of each filament re-forms, so they wander
  about. A finger on the glass is a stronger ground and pulls them together.
- **Lichtenberg figure.** The pure model, η = 1, grown from a point or an edge
  until its budget runs out, then held.

**Light, one model for all five.** Current flows through each grown tree by
Kirchhoff, so the trunk carries the most. Radius goes as √I, and emission per
metre as I²R = I. A frame's light is exactly the event's energy (½CV² for a
spark, V·I·Δt for an arc) × a luminous efficiency, so **a bang split into ten
branches isn't ten times brighter**. Colour comes from each gas's own spectral
lines through the CIE observer, and a channel heads towards a 6500 K blackbody
as its current nears an ampere, which is why a strike is white-hot and a
streamer violet. The glow moves light around and never adds any.

[![Flyback — Tesla coils, Jacob's ladders and plasma globes, for Resolume](docs/video-thumb.png)](https://www.youtube.com/watch?v=BV5pbQ7ngIs)

*[Watch it](https://www.youtube.com/watch?v=BV5pbQ7ngIs) (83 seconds): the
Jacob's ladder climbing, snapping and restriking, the Tesla coil's streamers
getting longer as BPS goes up, a Van de Graaff sparking every C·V/I and a rough
sphere sitting in corona, a plasma globe and a finger, a Lichtenberg figure,
Branching from bush to lightning to arc, the gases, and SW Flyback Over
striking into Resolume's demo clips. It's all the plugins' own output. An FFGL
plugin doesn't have a window to film, so the footage comes from this repo's
offline harness (`hvtest --film` and `--pipe`, driven by cue sheets) rather
than a screen recording.*

## Controls

- **Machine:** Preset (one per machine), Machine, Fire (a bang, a spark, a
  restrike, a surge, a new figure) and Seed. The same seed and controls grow
  the same bolt, frame for frame.
- **Supply:** Supply (ZVS, NST or Flyback: each a V_oc behind an R_s),
  Voltage and Source Impedance (trims around the supply's own).
- **Discharge:** Branching (η, relative to each machine's own), Detail (lattice
  sites across the scene), Channel Memory (τ) and Reach (growth per event).
- **Jacob's Ladder:** Rod Spread, Rod Length, Rise Speed and Wind.
- **Tesla Coil:** BPS (off, then 5 to 1000), Topload Size, and Target (None,
  Floor or Point) with Target X/Y.
- **Van de Graaff:** Belt Current, Sphere Size, Gap and Sphere Finish (polished
  sparks; a few per cent rough and it sits in corona).
- **Plasma Globe:** Globe Size, and Finger with Finger X/Y.
- **Lichtenberg:** Origin (a point or the bottom edge).
- **Audio:** Audio (Resolume's FFT buffer). Audio Fires turns each onset into a
  bang or a spark (a singing Tesla coil, minus the singing: FFGL has no audio
  out, so the bangs are what you see of the music). Audio Drive raises the
  supply voltage with the level.
- **Light:** Efficiency, Glow, Gas (Air, Neon, Argon, Neon-Xenon), Shutter
  (angle), Persistence (the camera's afterimage, not the air's), Show
  Apparatus (the rods, coil, spheres or globe, lit by the discharge) and
  Background.
- **Over** (the effect): Detect On (luma, alpha or edges), Ground Threshold,
  Illumination (the flash lighting the clip) and Mix. The clip's bright parts
  are ground, so strikes go into the picture and a globe's filaments gather to
  the brightest thing in frame.
- **Layout:** Position X/Y, Scale and Rotation.

The shared controls (Voltage, Impedance, Branching, Detail, Memory, Reach,
Efficiency, Scale) are relative to each machine's nominal, so switching Machine
on its own keeps each one's geometry, branching and memory looking like itself.
Supply and Gas aren't: switching Machine leaves them wherever they're set (the
globe's preset uses the Flyback supply and Neon-Xenon, the Lichtenberg's uses
ZVS, and the rest NST and air), so if you want a machine's usual supply and gas,
pick its preset. The host panel shows the physical value: kV, MΩ, η, ms, BPS, µA.

| | | |
| --- | --- | --- |
| ![Jacob's ladder](docs/ladder.png) | ![Van de Graaff](docs/vandegraaff.png) | ![Plasma globe](docs/globe.png) |
| ![Lichtenberg figure](docs/lichtenberg.png) | ![Tesla coil](docs/tesla.png) | ![SW Flyback Over, a Tesla coil over a clip](docs/over.png) |

<sub>Each preset, rendered by `hvtest`: the ladder mid-climb, a Van de Graaff
spark off a polished sphere, a neon-xenon globe, a Lichtenberg figure, the
coil, and the coil over a test clip through `SW Flyback Over`.</sub>

## Status

**v0.1.0, 2026-09-24, and still early days.**

It's **never been loaded into Resolume on macOS**. `oxbow probe` reads both
bundles the way a host does and finds `SW Flyback` / `HV01` / source and
`SW Flyback Over` / `HV02` / effect, and `oxbow selftest` instantiates each one
through the host's own path and renders.

**Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU,
2026-09-24): a CI build of this source loads from Extra Effects, both plugins
register with the right id and type, all 54 (source) and 55 (effect) host
controls match what the plugin declares, both render, and Arena's log stays
clean. That's 15 of 15 of the fleet gate's checks, with the audio controls
skipped because win-lab has no sound device. 33 of the source's controls and 43
of the effect's measurably moved the picture. Eight of the source's (Belt
Current, Gap, Sphere Finish, Origin, Rise Speed, Wind, Target X, Target Y) were
inconclusive: they act through discharges that come and go between the gate's
single-frame grabs, and the effect shows the same controls live. Software
rendering says nothing about a GPU or about speed.

There's a [user guide](docs/USER-GUIDE.md) and a
[browser demo](https://flyback-demo.stoatworks-labs.com/), but no OpenFX port.
I've only built and measured it on macOS (Apple Silicon, M4 Max). The Windows
build is MSVC's, from CI.

What's measured, on this Mac:

| | |
| --- | --- |
| Laplace solver | coaxial electrode, every free site within its one-lattice-step bound: worst **0.039** (0.65 of its bound) at h = 10.4 mm, **0.016** (0.43) at 5.2 mm |
| fractal dimension | η = 1, six clusters of 3000 sites: **1.744**. The same estimator reads 1.742 on true DLA (literature 1.71). η = 0 gives 2.09, η = 0.5 1.97, η = 2 1.36, η = 6 1.09. Box counting reads DLA as 1.47 at this size, so it is reported and not asserted |
| Jacob's ladder | every extinction at L\* = **0.1338 m** to one lattice step (worst 0.22 mm past it). The straight climb takes the closed-form **0.371 s** to 0.61 ms, the apex rises at the set 1 m/s, and each restrike is at the bottom. R_s doubled gives L\* 0.064 m |
| Tesla coil | **7200** bangs in a minute at 120 BPS, and **2250** at 37.5. Mean streamer **0.254/0.255 m** at 4 and 8 BPS (flat), then **0.365, 0.820, 1.072 m** at 60, 120 and 240 BPS with τ = 20 ms |
| Van de Graaff | polished: every interval **C·V_b/I** = 0.3519 s to 1.3e-11 s (V_b 189.6 kV, C 18.56 pF), and on screen to a frame. Half the belt gives 0.7038 s. 33 image charges hold both spheres to 5e-15 V. In the corona window (m 0.998539): every interval the charging ODE's closed form **0.352393 s** to 8.1e-6 s (bound 9.4e-6), 0.52 ms from the corona-free value. Rough (m 0.95, 0.82): no sparks, held at V_c + I/G to 1e-9 V_b |
| Kirchhoff | every node of every tree, all five machines: worst \|in−out\|/in **1.1e-7**, free tips equal |
| light | frame total against energy × efficiency at 640×360 and 1920×1080, with 5 and 24 free tips: worst **1.9e-5**, inside a bound of 4.1e-5 derived from the kernel's sampling and float32. At 320×180, CI's raster: **2.4e-5**, the same bound |
| exposure | at 360°, 95 of 95 bangs each in one frame. At 180°, 48 of 95, none twice. The pixels agree with the clock in every frame, at two rasters and at 320×180 |
| determinism | 90 frames bit-identical twice with one seed, different with another (coil, ladder, globe) |
| Over | a bright disc in a black clip takes **600 of 600** strikes (chance is 1.8%). A black clip sends all 152 to the coil's grounds |
| audio | the first hit after a fresh instance, and after the clock jumps back, fires |
| GL state | viewport, vertex array, program, units, framebuffer, blend, scissor, clear colour and buffers, as the host left them |
| 320×180 | every pixel check (light, exposure, determinism, Over, onset, GL state) and its negative controls pass again at CI's raster |
| negative controls | **14** wrong models, **all 14** detected, at the development rasters and at 320×180 |
| mutation | **9** one-character mutants of the engine and the GLSL (2 in the GLSL, caught by `--light`), **all 9** caught |
| dead controls | **48** parameters, all live where they apply |

Render cost (`hvtest --bench`; GPU time by `GL_TIME_ELAPSED`, engine CPU time
per frame, on a machine shared with other work while it ran):

| machine | GPU 720p | GPU 1080p | GPU 4K | engine mean | engine worst |
| --- | --- | --- | --- | --- | --- |
| Jacob's Ladder | 0.77 ms | 1.27 ms | 3.21 ms | 1.0 ms | 2.2 ms |
| Tesla Coil | 0.70 | 1.18 | 3.11 | 4.4 | 5.5 |
| Van de Graaff | 0.62 | 1.00 | 3.07 | 1.5 | 6.3 |
| Plasma Globe | 0.76 | 1.04 | 2.88 | 5.0 | 11.0 |
| Lichtenberg | 0.73 | 1.25 | 3.09 | 1.3 | 2.5 |

These come from a run with the load average near 8, and they **predate the
worker thread** (64c21d2), so "engine" there means time on the render thread.
`--bench` now runs the worker paced at 60 fps and reports the render thread's
time and the worker's time per step separately, and I haven't re-measured the
table since. The same bench inside `tools/verify.sh`, with more running beside
it, measured GPU 1.6–2.4 ms at 720p, 2.1–2.8 at 1080p and 5.1–5.8 at 4K, and a
worst engine frame of 12.3 ms (the globe). Treat the table as a floor and those
as a ceiling. Either way a frame costs well under the 16.7 ms of 60 fps at
1080p. At 4K the worst sum, engine plus GPU, is about 18 ms under load.

What **isn't** verified yet:

- **Never in a host on macOS.** I don't know yet how 53 parameters in thirteen
  groups present, whether Resolume's clock arrives in seconds or milliseconds
  (the plugin works it out by vote), or what its FFT bins really are.
- **The engine runs on a worker thread, one frame late.** Each frame draws
  the engine step started the frame before, so the picture is one frame
  behind the clock, and the very first frame shows the apparatus with no
  discharge light. The render thread only waits for whatever part of the
  engine's step (worst 11–12 ms, the globe) didn't overlap the host's own
  work. It's bit-exact against the synchronous engine one frame earlier
  (`--determinism`), but it's never run inside a host.
- **Some constants are models, not measurements.** The low-current Ayrton
  constants (D = 750 W/m sets the ladder's height), the streamer propagation
  field, the glass's coupling in the globe, a toroid treated as a sphere, and
  the line weights behind each gas's colour. AGENTS.md lists which ones were
  carried from memory.
- **The Lichtenberg figure grows over three seconds and stays lit.** A real one
  forms in nanoseconds and is a dark fossil afterwards. That bit's showbiz.
- **No racing sparks down the Tesla coil's secondary.** The Van de Graaff's
  corona is charged against the belt (C dV/dt = I − G(V − V_c)), but G is
  derived to first order in space charge and the rough-sphere factor is
  borrowed from Peek's cables (AGENTS.md).

## Browser demo

**<https://flyback-demo.stoatworks-labs.com/>**: both plugins and all five
machines, in a browser. It isn't the plugin, though. The light path is the
plugin's own GLSL, copied unedited (`demo/tools/check_shaders.py`, run by
`tools/verify.sh`, fails on any drift). The CPU engine (the Laplace solve, the
breakdown model, the machines' circuits and the controls) is a hand port to
JavaScript in `demo/engine.js` that **nothing checks but a reader**. It runs at
the plugin's own lattice sizes on the page's main thread, isn't bit-exact and
has no audio. The page lists every difference.

## Build

C++17 + GLSL 4.10, CMake, FFGL 2.1 (SDK vendored as a submodule). macOS builds
are universal (arm64 + x86_64); Windows needs GLEW via vcpkg.

    git clone --recursive https://github.com/stoatworks-labs/flyback
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cmake --install build          # both bundles, into Resolume's Extra Effects

## Building and testing

The offline harness renders the real plugin class headlessly:

    ./build/hvtest --out /tmp/f.png --preset 3 --frames 23   a frame (a Van de Graaff spark)
    ./build/hvtest --out /tmp/o.png --effect                  the effect over a test clip
    ./build/hvtest --laplace        the solver against the coaxial closed form
    ./build/hvtest --dimension      fractal dimension against DLA
    ./build/hvtest --ladder         Ayrton's L*, the climb, the restrike
    ./build/hvtest --tesla          the interrupter is a clock; channel memory
    ./build/hvtest --vdg            sparks every C V_b / I
    ./build/hvtest --kirchhoff      current in = current out
    ./build/hvtest --light          light conserved, at two rasters
    ./build/hvtest --exposure       one frame per spark, or none
    ./build/hvtest --determinism    same seed, same frames
    ./build/hvtest --over           strikes go into the picture
    ./build/hvtest --onset          the first hit fires
    ./build/hvtest --negative       every check against a wrong model
    ./build/hvtest --offline        the checks that need no GL context (what CI runs)
    ./build/hvtest --light --size 320x180   any pixel check at another raster
    ./build/hvtest --bench          GPU and engine cost
    python3 tools/sweep.py          no control is silently dead
    tools/mutate.sh                 one-character mutants must be caught
    tools/glslc.sh                  every shader through glslc (verify.sh and CI)
    tools/verify.sh                 all of it, from a fresh universal build

Filming uses the fleet's frame format and cue sheets:

    ./build/hvtest --film 960 --size 1280x720 --script docs/demo.cues \
      | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - -pix_fmt yuv420p demo.mp4

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT.

The physics comes from the papers and books ATTRIBUTIONS.md lists: the
dielectric breakdown model, Ayrton's arc, Kelvin's images, Peek's law and the
CIE observer. None of it's copied from anyone's source.
