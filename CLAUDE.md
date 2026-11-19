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
- The singing coil: `--tone 220[@-20]` (a 10 s sine, dBFS) or `--wav PATH`, with
  `--set "Pitch Source=System default"` (the harness's scripted devices; no
  CoreAudio unless `HVTEST_REAL_DEVICES=1`). Works with `--out`, `--film`,
  `--pipe` and `--bench`
- Film: `./build/hvtest --film 960 --size 1280x720 --script docs/demo.cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - -pix_fmt yuv420p demo.mp4`
- Film a clip through the effect: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/hvtest --pipe --effect --size WxH [--script cues] | ffmpeg ...`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  raw host values as `--set` (0..1 for a slider, the index for an option;
  `--list` shows them). Values interpolate linearly
  between a name's cues and hold before the first and after the last, so a step
  needs two cues a frame apart -- an option index interpolated is a different
  option on the way; Fire is thresholded at 0.5, so a press is `29 Fire 0 / 30 Fire 1
  / 31 Fire 0`. Frame *n* is clocked at n / 60 s. An unknown name exits 2 before
  any frame; a partial frame at EOF ends the stream with exit 0; a frame that
  fails to render, or a reader that hangs up (`| head -c 1`), exits 1 with a
  message on stderr (SIGPIPE is ignored, so never a silent 141).

## Verify
- Everything: `tools/verify.sh` (a few minutes: reserved words, glslc, the SDK pin, the earshot snapshot, a FRESH universal build, lipo, plugMain, plists, ad-hoc codesign, `oxbow probe` and `selftest` on both bundles, the CoreAudio trap on the mode-off path, every check, every pixel check again at 320x180, the singing GL checks on Apple's software renderer, the negative controls at both, `--offline`, the pipe's exit codes (singing too), the sweep, the bench at 720p and 1080p)
- Physics, no GPU: `--laplace`, `--dimension`, `--ladder`, `--tesla`, `--vdg`, `--kirchhoff`
- Pixels, at their development rasters: `--light` (640x360 + 1920x1080), `--exposure` (480x270 + 1280x720), `--determinism` (480x270), `--over` (640x360), `--onset`, `--state` (320x180). Any of them with `--size WxH` runs at that raster instead; verify.sh adds `--size 320x180`, CI's raster
- Registration: `--defaults` (preset 1 = the constructor), `--names` (16 bytes, unique)
- The singing coil: `--sing`, `--sing-clock`, `--sing-memory`, `--device` (no GL); `--sing-picture`, `--off` (the mode off changes nothing and reaches no earshot), `--lamp`, `--resize-sing` (GL). `HVTEST_RENDERER=software` runs any GL check on Apple's software renderer
- The music the law was chosen on: `--demo-music "/Applications/Resolume Arena/media/AV"` (a report, not a check)
- The CoreAudio trap: `build/libcatrap.dylib` (tools/catrap) under `DYLD_INSERT_LIBRARIES` with `CATRAP_LOG=file`; `hvtest --trap-probe` is its negative control and refuses to run without it
- The checks can fail: `--negative` (31 wrong models; takes `--size`, and `--only a,b`); `tools/mutate.sh [prefix]` (18 one-character mutants, 3 of them in shipped GLSL, under 3 minutes, not in verify.sh)
- No GL context (CI): `./build/hvtest --offline` -- the checks main()'s one table marks as needing no GL, with their negative controls; it says loudly that the pixel checks did not run. Shaders without a driver: `tools/glslc.sh` (verify.sh and CI both call it)
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
- The engine runs on a worker thread, one frame late: `ProcessOpenGL` for
  frame n collects the step started at n-1, draws it, and submits frame n's
  (`Flyback.h`, "The engine's worker thread"). The first frame has no light.
  The harness runs it synchronously (`SetSynchronousForTest`) except in
  `--determinism`'s worker half and `--bench`.
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
- Public at https://github.com/stoatworks-labs/flyback, released v0.1.0
  (2026-09-24) and v0.2.0 (2026-11-19, the singing coil), registered on the
  website. CI and release workflows run on GitHub (the repo is public).
- **The singing coil** (`Singing.*`, AGENTS.md): Pitch Source, Rescan Inputs,
  Octave, Gate are ids 54-57, AFTER About. Never renumber. With Pitch Source
  Off nothing may reach earshot or CoreAudio (`--off`, and verify.sh's trap):
  no engine, no listDevices, no selectDevice, no extra GL call. earshot is a
  snapshot (`external/earshot/SNAPSHOT`, commit 8124191, from
  https://github.com/stoatworks-labs/earshot): never edit it here.
  `FLYBACK_SINGING` is ON on every platform since v0.2.0 (OFF builds a stub
  with the same parameters).

## Not done yet
- No OFX port. The README's bench table predates the worker thread. What
  Resolume has shown on each platform is the README's Status.
- The singing coil has never heard a real input in a check (scripted devices
  and synthetic tones only), and Windows is built by CI only.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by the backend
  (`sync-about.py`, `sync-attributions.py`): never edit them here.

## Browser demo
- `demo/` is the page at https://flyback-demo.stoatworks-labs.com (Cloudflare
  Worker `flyback-demo`, a ROUTE on a proxied AAAA 100:: record -- the zone's
  custom domains are full; `wrangler.toml` says why). Deploy:
  `cf-run npx wrangler deploy`; `.github/workflows/deploy.yml` redeploys on push
  to main and checks the live `<head>`.
- `demo/plugin.js` holds the nine shaders VERBATIM; `demo/tools/check_shaders.py`
  (in verify.sh) fails on drift. Change `source/render/Shaders.cpp` -> copy it.
- `demo/engine.js` is a hand port of `source/engine/`, `Controls.cpp` and
  `Presets.h`. Nothing checks it: change the C++, change the port.
- `demo/vendor/` is the shared kit: never edit it, re-vendor with
  `stoatworks-backend/resolume-demo/sync.sh`.

## Diagnostics

`source/Diag.{h,cpp}` -- log file only, no crash handler. It records which of
the seven shaders failed to compile, the GL version and renderer, and the host
clock's unit once voted on.

    ~/Library/Logs/flyback/flyback.YYYY-MM-DD.log
