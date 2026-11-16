# earshot

A shared live-audio front end for the Stoatworks FFGL plugins: one C++17
static library (`earshot`, alias `earshot::earshot`) that a plugin pulls in
with `add_subdirectory`. Own capture (miniaudio, from spasis), a Resolume-FFT
fallback, and pitch, partials, onsets, band envelopes, a third-octave bank,
tempo, stereo and a waveform tap. MIT, copyright Stoatworks. Local repo at
`~/dev/earshot`; intended home `github.com/stoatworks-labs/earshot`; never
pushed. No plugin uses it yet.

Read `AGENTS.md` before changing a detector, a constant, or any tolerance in
`estest`. The plugin author's guide is `docs/INTEGRATING.md`.

## Commands

- Iteration build (arm64 only; the Mac is shared):
  `cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build -j4`
  (top level defaults to Release and, without that flag, to universal)
- Everything: `tools/verify.sh` (about ten minutes; `--quick` skips the
  negative controls and the mutation test). Builds under `build-verify/`,
  logs in `build-verify/logs/`. Exit 0 only if every step that ran passed.
- The harness: `build/tools/estest/estest` runs every group in every cell
  (44.1/48 kHz x blocks 128/1031, 60 fps): about a minute.
  - one group: `estest --pitch` (groups: `--fft --pitch --partials
    --onset-prime --onset-attack --bands --thirds --tempo --stereo --tap
    --silent --ring --params --fallback --capture --alloc`; `estest --help`)
  - one cell: `--rate 48000 --block 128`
  - negative controls: `estest --negative` (every mutation must fail the
    checks it targets, and every check must pass unmutated and have a
    mutation that fails it; ~5 min); one of them: `--negative --only onsetNoMask`
  - run groups with one mutation set, to watch it fail: `estest --tempo --with tempoLegacy`
  - a file through the engine: `estest --file track.wav [--expect-bpm 125 --from 6 --fps 30]`
  - performance: `estest --perf`
  - capture devices (enumerates, opens nothing): `estest --devices`
  - live capture, by hand only (may raise a microphone prompt):
    `estest --live 10 [--device "MacBook Pro Microphone"]`
  - WAV writer/reader round trip: `estest --write-wav /tmp/x.wav`
- Mutation test: `tools/mutate.sh` (all) or `tools/mutate.sh 3 7`;
  `MUTATE_VERBOSE=1` prints each mutant's failures. Works on a scratch copy.
- Consumer smoke test (add_subdirectory into a MODULE, loaded by a host):
  `cmake -S tools/consumer -B /tmp/c -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build /tmp/c && /tmp/c/consumer-host`

## Layout

- `include/earshot/earshot.h`: the whole public API, documented.
- `include/earshot/testing.h`: harness hooks (Mutations, CaptureBackend). Plugins never include it.
- `src/`: one module per analysis (`Pitch`, `Partials`, `Onsets`, `ThirdOctave`,
  `Tempo`, `Stereo`, `Tap`, `HostFft`), `Capture` + `Ring` + `Miniaudio*`,
  `Engine.cpp` gluing them. Each module's header states its latency and thread.
- `third_party/miniaudio/`: miniaudio 0.11.25, vendored unmodified.
- `tools/estest/`: the harness. Each `Check*.cpp` opens with the tolerances
  and their sources.
- `tools/verify.sh`, `tools/mutate.sh`, `tools/consumer/`.

## Rules

- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- No allocation, lock or wait on the render or audio thread after
  construction; `estest --alloc` and `--capture` count it.
- A new claim gets a check with a written tolerance source and a negative
  control (a `Mutations` flag and a case in `negativeCases()`) that makes it
  fail in every cell. `estest --negative` fails while any check has none.
- Never open an audio device from `verify.sh` (no microphone prompts).
- `CMAKE_OSX_ARCHITECTURES` and the deployment target are only defaulted when
  earshot is the top-level project; as a subdirectory the plugin decides.
