# flyback

High-voltage discharges -- Jacob's ladder, Tesla coil, Van de Graaff, plasma
globe, Lichtenberg figure -- all grown by the dielectric breakdown model, as
**two** FFGL plugins for Resolume Arena/Avenue: a source (`SW Flyback`, `HV01`)
and an effect that strikes into the clip (`SW Flyback Over`, `HV02`). C++/GLSL,
CMake MODULE -> universal `.bundle` (macOS) + Windows `.dll`. MIT. Bundle ids
`com.stoatworks.ffgl.flyback` and `.flyback.over`.

Read `AGENTS.md` before touching the solver, the growth, a machine's circuit or
the light's accounting.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build` (not in a session: it writes into Arena's Extra Effects)
- A frame: `./build/hvtest --out /tmp/f.png --preset 1 --frames 150` (`--machine N`, `--effect`, `--size WxH`)
- A Van de Graaff spark: `--preset 3 --frames 23`; press Fire on a frame: `--fire N`
- List parameters (with the host's display strings): `./build/hvtest --list`
- Set anything by name: `--set "Branching=0.3" --set "Machine=4"`
- Film: `./build/hvtest --film 960 --size 1280x720 --script docs/demo.cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - -pix_fmt yuv420p demo.mp4`
- Film a clip through the effect: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/hvtest --pipe --effect --size WxH [--script cues] | ffmpeg ...`

## Verify
- Everything: `tools/verify.sh` (a few minutes: reserved words, glslc, the SDK pin, a FRESH universal build, lipo, plugMain, plists, ad-hoc codesign, `oxbow probe` and `selftest` on both bundles, every check, the negative controls, the sweep, the bench)
- Physics, no GPU: `--laplace`, `--dimension`, `--ladder`, `--tesla`, `--vdg`, `--kirchhoff`
- Pixels, two rasters: `--light`, `--exposure`; also `--determinism`, `--over`, `--onset`, `--state`
- Registration: `--defaults` (preset 1 = the constructor), `--names` (16 bytes, unique)
- The checks can fail: `--negative` (13 wrong models); `tools/mutate.sh` (7 one-character mutants, ~1 min, not in verify.sh)
- No dead controls: `python3 tools/sweep.py` (each control swept on the machine it belongs to)
- Cost: `./build/hvtest --bench [--machine N] [--set ...]`

## Notes
- **The potential is solved, not summed.** `engine/Lattice` is PCG with a
  multigrid V-cycle preconditioner. Kim-Sewall-Lin point-charge superposition
  was measured and is not the breakdown model (AGENTS.md).
- **Growth**: one site at a time; a local Gauss-Seidel window after each, a
  full solve every 32. Eight neighbours. Quenched disorder in the breakdown
  threshold (`GrowthSettings::disorder`) so the lattice's axes do not show.
- **The lattice is in metres of scene**; Detail sets sites across the scene's
  height. The physics checks never see a pixel.
- **Light is joules**, deposited by an exact segment-Gaussian closed form into
  RGBA32F, then a 2x2-SUM glow pyramid in frame units. Everything on the light
  path is float32: half float's smallest normal (6.1e-5) is above a pixel's
  joules.
- **Relative controls**: Voltage, Impedance, Branching, Detail, Memory, Reach,
  Efficiency, Scale are trims around each machine's / supply's nominal
  (`Controls.cpp`, `NominalFor`). Presets are an OVERRIDE (graticule's model):
  `FlybackPlugin::Effective` lays the row over the params at read time.
- The engine runs on the render thread (`Engine::Advance` in `ProcessOpenGL`);
  its time is `Engine::LastMillis()`.
- Over: the clip is thresholded onto a 160-wide mask and read back through a
  PBO pair, a frame late; bright cells become ground.
- All ranged host parameters are 0..1, converted in `Controls.cpp`;
  `GetParameterDisplay` resolves on demand and never calls the base class.
- GLSL reserved words (`patch sample input output filter common active half layout flat`) must not be identifiers -- `half` was used once here; `verify.sh` greps.
- Randomness is PCG32 and lowbias32 on the CPU; the renderer's sub-lattice
  jitter is keyed by lattice cell. No `fract(sin())`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block.
- `flyback_core` is an OBJECT library; each registration is listed only in its
  own MODULE target.
- Universal build: check with `lipo`, never the log.
- Local repo only: no GitHub remote, no tag, not registered on the website.

## Not done yet
- Never loaded into Resolume (oxbow probe + selftest only). No OFX port,
  browser demo or user guide. Never built on Windows. The engine is not on a
  worker thread.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies (`guide = ""`).

## Diagnostics

`source/Diag.{h,cpp}` -- log file only, no crash handler. It records which of
the seven shaders failed to compile, the GL version and renderer, and the host
clock's unit once voted on.

    ~/Library/Logs/flyback/flyback.YYYY-MM-DD.log
