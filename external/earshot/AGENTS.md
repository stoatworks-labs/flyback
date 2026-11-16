# earshot: why it is the way it is

The command reference is `CLAUDE.md`. This file is the reasoning: the
decisions, what was measured to make them, and the traps. Read it before you
change a detector, a constant, or a tolerance in `estest`.

Status, 2026-11-16: a local library repo (`~/dev/earshot`), intended home
`github.com/stoatworks-labs/earshot`, never pushed. No plugin uses it yet.
Everything below was measured by `estest` on synthetic signals on one Apple
Silicon Mac (macOS 26.4), plus pattern's release-video soundtrack for the
tempo. **MSVC has never compiled it**, and **live capture has never been
exercised**, from a host or otherwise: the miniaudio backend has enumerated
this Mac's 11 capture devices (`estest --devices`, names, channel counts and
distinct keys, no stream opened) and has never opened a stream. Everything
about the device worker was checked against a scripted backend that stands
in for the OS.

## What it is for

FFGL gives a plugin one audio input: `FF_USAGE_FFT`, 64 bins of a mono sum
with an unmeasured law. The fleet keeps reinventing what that cannot give it:
spasis (a stereo scope) opened its own miniaudio device; pattern needed onsets
that hear the first beat and a tempo at the right metre; needle needs a
third-octave bank whose low bands are 6 Hz wide; woofer needs the waveform
as a stream. earshot is that once: one CMake target, pulled in by
`add_subdirectory`, that a plugin owns one `Engine` of.

The public API is in `include/earshot/earshot.h` and is meant to be read top
to bottom. A plugin makes about five calls: construct, `listDevices`,
`selectDevice(name)`, `update(hostSeconds, bins, 64)` once a frame, and
`readTap` if it wants the waveform. `docs/INTEGRATING.md` is the plugin
author's guide.

## Decisions

### Capture (from spasis, one thing fixed)

- **Devices by name, never by index.** A named device that is absent is
  Missing and retried every 2 s; it is never swapped for the default. The
  scripted-backend check (`capture.missing`) asserts the default is never
  even asked for. An option parameter saves `deviceKey(name)`, FNV-1a folded
  to 23 bits so it is exact in a float.
- **The first n channels, never a downmix** (`Config::maxChannels`, read at
  construction). The pair is `pairFirst`, `pairFirst + 1`.
- **Silent** = open, running, bit-exactly zero for 3 s of *audio* (frames
  delivered, not wall time), judged on the raw device buffer before slicing,
  so a live channel outside the pair keeps it Running. A level threshold was
  rejected: -120 dBFS room noise is a quiet passage, not a dead device (the
  `silentByLevel` control fails `silent.quiet`).
- **Opening is off the caller's thread** (the fix: spasis opened inside
  ProcessOpenGL and measured CoreAudio blocking the render thread). A worker
  thread does every open and close; it starts on the first `selectDevice`
  with a name, not at construction, because hosts construct instances they
  never render. `capture.slowopen`: a 300 ms open, `selectDevice` 0.01 ms,
  worst `update` 0.7 ms (the limit is 100 ms, a third of the open: see the
  wall-time trap).
- **A parameter change never clears the ring.** `configure()` stores a
  pending copy under a mutex; `update()` takes it with `try_lock` (never
  waits; a busy writer means next frame). Analysers reconfigure in place.
- **The ring** is spasis's seqlock with an epoch, changed so that a reader
  validates against the writer's *claim* (set before the copy) as well as
  the published count; a reader that is lapped during its copy is refused.
  `ring.race` streamed 210 M frames from a racing thread: 0 torn reads
  accepted.
- A render thread that falls behind catches up at most 0.5 s; beyond that it
  skips to the newest half second, counts the gap in `Frame::gapFrames`, and
  re-primes the onsets (flux across a gap is not an onset).

### Fallback: Resolume's FFT

Band envelopes and onsets only. Pitch, partials, thirds, tempo, stereo and
the tap stay EMPTY (spasis's rule: a plausible guess is worse than an empty
display; the `fallbackInventsPitch` control fails `fallback.empty`). The bin
law is `Config::hostBins`, an assumption the plugin exposes (needle's and
pattern's `Bin Law`, pattern's `Bin Value`): Magnitude or Power, Linear or
Log, and a sample rate, 44.1 kHz unless the plugin says otherwise. The
onsets are pattern v0.1.1's linear flux against 3x an adaptive floor,
primed on frame one with the floor at an eighth of the band level, with
crossing semantics; a repeated host time (dt = 0) moves nothing, and a host
clock jump over 1 s re-primes.

Used when no device is chosen (Closed), and while a chosen one is Opening,
Missing or Failed if `fallbackWhenDeviceDown`. **Never while Silent**: a
Silent device is open, and the operator must see that it is dead.

### Pitch: MPM, two passes

McLeod's NSDF via FFT, the first key maximum within 0.9 of the highest,
parabolic refinement. The confidence is the NSDF peak, which for a sine in
noise is S/(S+N) by construction, so `estest` checks that relation rather
than a tuned figure.

Noise was the problem: the NSDF's noise-times-noise term grows with
bandwidth, and a 110 Hz sine at 10 dB SNR read 17 cents off full band.
Pass one runs low-passed at 2.5 x pitchMaxHz (4th-order Butterworth, RBJ
biquads in double, 256 samples of warm-up history), pass two at 8 x the
pitch pass one found; 2.7 cents worst now. Pass two is kept only within
50 cents of pass one. That guard is defensive: no signal in the harness
makes pass two disagree (residue tones with every partial above pass two's
band included), so the mutation that removes it survives and was retired
from `mutate.sh` rather than claimed.

Each newest-window analysis runs as soon as ITS window is available: the
pitch was first gated on the 186 ms partials window and stayed unvoiced for
the first frames.

### Partials

QIFFT on a Hann window's log magnitude. The frequency tolerance is not a
guess: Hann's transform gives the parabola's bias (0.0160 bin worst); the
harness measures 0.0159.

### Onsets on the audio path: rebuilt

pattern's detector (linear flux against 3x its mean, floor seeded at 1/8)
was the starting point and failed on real-looking material in three ways,
each measured:

1. **A held triad fired about eight onsets a second.** Linear flux is
   dominated by loud steady partials whose bins jitter. Now: log flux,
   log(1 + g|X|) with the knee at -110 dB per bin (at -70 dB a 70 dB noise
   swell fired dozens of times); the SuperFlux maximum filter (each bin
   against the largest of its own and its neighbours' previous values,
   Boeck & Widmer, DAFx 2013); and a threshold of max(1, mean + 12 MAD).
   Twelve deviations sounds absurd, and is what the measurement said: steady
   music's flux is heavy-tailed. At k = 8, 11 false onsets on 12 s of white
   noise; at 12, one. Floors of 2 or 3 made the kick inaudible in the
   30-150 Hz band.
2. **The 1/8 seed is ten times too high for log flux**: the first kick at
   250 ms went unheard (flux 3.2 against a threshold of 4.8). Now the first
   hop primes, the next 50 ms only measure, and the floor is a cumulative
   average for the first second, then a 1 s one-pole. Primed kick heard in
   8-13 ms.
3. **pattern's broadband lesson, properly**: a hard attack's first hop is a
   click across the spectrum. The masking gate lets a band fire only if the
   energy that rose in it this hop (bin by bin) is within 30 dB of the
   largest rise in any band, and never below -80 dB. Bin by bin, because a
   kick rose in one bin while the bass note's phase took energy out of the
   next and the band total called it no rise. A masked hop does not use up
   the crossing. The -80 dB floor, because on digital silence the first hop
   caught a 90 Hz attack's first half millisecond, a -100 dB sliver whose
   spectrum is as wide as the band plan, and lit the 4-16 kHz band at
   44.1 kHz only.

One onset per crossing (the flux must fall below the threshold before the
band can fire again), and a 50 ms refractory.

**What "a click's reach" means in the checks.** A step attack is a real
broadband event, not splatter to be suppressed: a cosine-phase step put
energy 7-16 dB down two bands away in the first hop. So `onset.attack.*`
use raised-cosine rises (1 ms is hard) and allow a band to fire if it lies
within r = 2/rise + 2/hop + 2 bin widths of the tone (the main-lobe
half-widths of the attack envelope, the first hop's snippet, and the
analysis window). Beyond that only sidelobes remain, and that is what the
gate must remove. A dead stop (1 ms release) is a transient too and may
fire within its reach (`onset.attack.cut`); an ending is not suppressed.

A rule tried and removed: referencing the gate to the largest movement up
OR down (so a release's splatter is masked by the note's own fall). No
check could tell it from rises-only once the -80 dB floor was in, so it
went: code no check needs is not kept.

### Band envelopes

Parseval-correct band levels (window energy corrected; a sine reads its
amplitude to 1e-6 dB on or between bins), one-pole attack and release.

### Third-octave bank

IEC 61260-1 base-ten bands (G = 10^0.3, fm = 1000 x 10^(x/10)), ISO 266 R10
labels, as many as fit under 0.95 of Nyquist (30 at 44.1 kHz, 31 at 48).
Butterworth band-pass, bilinear with both edges pre-warped, so the -3.01 dB
points land on the IEC edges at any rate (off by under 1e-9 dB). Double
precision: the 20 Hz band's poles sit 0.0004 from the unit circle.

**Judged against the class 1 mask, which found a real defect**: pre-warping
stretches a top band's analog prototype (the 20 kHz band at 48 kHz becomes
two octaves wide in the analog domain), and a third-order skirt that wide
missed class 1's lower-skirt minimum by 7-11 dB. `orderFor()` now raises
the prototype order where the warped skirt needs it (four bands per rate go
to fourth order); smallest margin on the skirts is 0.89 dB.

The mask is **IEC 61260:1995 class 1** (table 1, as reproduced in IS 6964,
the freely readable copy), mapped to a third octave by the 1995 breakpoint
rule, interpolated linearly in lg Omega between breakpoints (the standard
tabulates breakpoints; straight lines between them on a log axis is our
reading, stated as an assumption). IEC 61260-1:2014 was not consulted.
Only the analytic response is judged against the mask; the running filters
are then checked end to end against that analytic response.

Level: mean square through IEC 61672's Fast (125 ms), primed with the first
block. Peak hold and fall in audio time. `thirds.ballistics` times both
after a cut: the Fast decay is 34.744 dB/s (10 lg e / 0.125 s; limit
0.1 dB/s), the hold lasts thirdHoldSeconds after the last peak to within a
frame, then falls at thirdFallDbPerSecond.

### Tempo: pattern v0.1.1's detector, plus an onset gate

Three registers (< 400 Hz, 400 Hz-5 kHz, above) of log flux on a 100 Hz
grid built from audio hops (so the frame rate cannot move it), standardised,
S(T) = 0.5 R(T/2) + R(T) + R(2T), family under a 120 BPM prior then the
fastest level scoring 0.7 of the best. Constants unchanged from pattern.

Added: **a reading needs six onset hops in the six-second window.** A held
chord settled at 167 BPM with confidence 0.71: standardising each register
to unit variance amplified the partials beating in the flux. A published
tempo stays through a breakdown and is replaced only by two new agreeing
readings.

pattern v0.1.0's detector is kept as the `tempoLegacy` control; on the
four-on-the-floor at 125 it reads 62.5, as it did on pattern's release
video. On that video's real soundtrack earshot reads 124.96, every settled
frame from 6 s within 0.22 BPM, at 30 and at 60 fps.

### Stereo and tap

Stereo: spasis's width formula in the time domain, correlation 0 when a
side is silent (not 0/0), balance clamped to +-60 dB, over the newest
2048 samples. The tap is a resampler on the absolute sample clock: output k
is the input at exactly k / tapRate (position as integer plus remainder, so
it never drifts), Kaiser-windowed sinc, beta 8, ten zero crossings, cutoff
0.4 x tapRate: flat to 0.0004 dB at 2 kHz, 87 dB of alias rejection at
4.4 kHz, at 8 kHz. Queue of 65536 samples; overflow is counted in
`Frame::tapDropped`.

## Would this hold at another sample rate and block size?

The fleet's "two rasterisers" idea, for audio. Every per-cell check runs at
44.1 and 48 kHz, each at a 128-sample block and an awkward prime one (1031),
with `update()` at 60 fps; the tempo also at 24 and 30 fps. A claim that
holds in one cell and not another is a bug in the claim or the code. What
this found: the -100 dB first-hop sliver (44.1 kHz only); the warped top
bands (worse at 48 kHz, where the 20 kHz band exists); the maximum filter's
benefit, which shows only at 48 kHz in the harness's material.

## Tolerances

Every check states its tolerance and where it came from, in a comment above
the check in `tools/estest/Check*.cpp`. The kinds:

- **Derived** from the method: MPM's parabola bias (1 cent), Hann QIFFT's
  bias, Kaiser's stopband (A = beta/0.1102 + 8.7), the -3.01 dB of a
  pre-warped Butterworth, the 125 ms one-pole's ripple at 2f, the tempo grid
  (half a cell at four lags is at most 0.63 BPM at 174, so 1 BPM).
- **Statistical**, at 4 sigma: band levels of white noise
  (E[level^2] = 4 c sigma^2 / N), sample correlation of independent noise
  (1/sqrt N).
- **Standards**: the class 1 mask; the 5-cent pitch JND.
- **Design limits**, said as such: one false onset per 12 s of white noise
  (below).

## Known limits

- **White noise fires about one onset per 12 s** in the 30-150 Hz band, at
  both rates: three bins whose log flux is a sum of differences of
  chi-square variables, a tail that reaches mean + 12 MAD once in a few
  thousand hops. Tonal and mixed material fires none. `onset.steady` allows
  one, and says why.
- **The maximum filter's benefit is statistical**: on the harness's noisy
  pads it removes four false onsets in 36 s at 48 kHz and makes no
  difference at 44.1. Its negative control is the one marked "one cell is
  enough".
- **A device that stops calling back without reporting it** stays Running:
  Silent counts zeros delivered, and a stalled device delivers nothing.
  Nothing in the harness covers a stall.
- **Under the Linear host-bin law the 30-150 Hz band gets no bin at all**
  (bin 0's centre is 172 Hz at 44.1 kHz). That is the honest answer for 64
  linear bins; the Log law gives it bins.
- **update() cost**: mean 0.6 ms on this Mac for a 60 fps frame of 48 kHz
  stereo with everything on; one run on a loaded machine saw a 5.3 ms frame.
  The tempo detection runs once per audio second (~1 ms).
- MSVC never compiled. The code avoids the known traps (no M_PI, no VLAs, no
  GCC attributes outside the consumer test, `/utf-8 /permissive-` set), but
  that is a statement of intent, not a result.

## Negative controls and the mutation test

`estest --negative`: 50 `Mutations` flags (include/earshot/testing.h) in 51
cases, each a bug the fleet shipped, a design this repo rejected, or a
classic slip (biquads in float, a float twiddle recurrence, an allocation
per frame, a device swapped for the default); every check a case targets
must FAIL in every cell. All 51 are caught. The run begins with every group
unmutated and ends with coverage: each of the 65 checks must pass unmutated
and be the target of a caught case, or `--negative` fails. All 65 are.

Two controls are weaker than the rest, and say so:

- `onsetPlainFlux` (no maximum filter) fails `onset.steady` at 48 kHz only
  (the filter's benefit is statistical; Known limits).
- **The five tempo grooves are one claim**, tempo on grooves, and its control
  is pattern v0.1.0 (`tempoLegacy`), which misreads four of them in every
  cell. Nothing tried misreads the triplet shuffle at 110: the slowest
  metrical level, level ratios from 0.3 to 0.99, no tempo prior, no R(T/2)
  term, registers at equal weight, an onset gate that counts only the kick
  band all read 110 within 0.03 BPM. For this detector it is a groove with
  no metrical ambiguity, kept as a guard for ternary subdivision.
  `tempoSlowestLevel` fails kick-128 on its own.

Before the coverage pass, 27 cases covered 34 checks. Sweeping every flag
across every group showed many incidental failures (pattern's seed also
fails the clap test, but only at 48 kHz; a broken level fails the envelope
timing because the 63 % point is never reached); a control counts only for
the checks it fails in every cell, and the timing claims got their own.

`tools/mutate.sh`: fifteen source edits the harness has never heard of
(a parabola's sign, a time constant, an ISO label, a seqlock's lap check...),
each built in a scratch copy; estest must fail. Fifteen of fifteen killed.

## Traps

- **Wall time measures the scheduler.** `capture.slowopen` limited
  `update()` to 5 ms; on this shared Mac, with other builds and a release
  running, one update took 31 ms, and the check failed twice in a day (once
  inside an unrelated negative-control run). The claim is that the render
  thread never waits for the open, and a waiting one takes the whole
  300 ms, so the limit is now 100 ms. The unloaded figures are unchanged.
- **A sub-check that builds its own Engine must hand it the mutations.**
  `fallback.law`'s Power-law half constructed an Engine without them, so no
  control could reach it. Found when every check had to have a control.
- **A refusal for the wrong reason passes.** `ring.race`'s "a reset
  invalidates an old reader" read frames the reset had removed, so the
  frame count refused the read whatever the epoch said. It now writes them
  again first; only the epoch can refuse.
- **A test double that tidies up hides the bug.** The scripted backend's
  destructor closed its stream, so "destroying the Engine closes the device"
  passed when the Engine never closed it. The double now records a stream
  still running when it is destroyed.
- **macOS make compares mtimes to the second.** mutate.sh first rewrote a
  file in the same second its object was built; make called it up to date
  and the previous mutant was tested instead; a restored file kept its
  mutant's object. The script now waits the second out both ways.
- **The compiler may delete a new-expression** whose result is unused
  (C++14 allocation elision). The allocation counter's control read 0 until
  it called `::operator new` directly.
- **A sine's switch-on rings a narrow filter**, and the 125 ms weighting
  forgets at only 34.7 dB a second: at 1.5 s the ring still read 0.8 dB into
  a -78 dB skirt reading. Read skirts at 3 s.
- **A step attack is a click** whose energy really reaches other bands; a
  test that calls it splatter is wrong, not the detector. Use rises.
- **A detector that always answers**: unit-variance standardisation makes
  any flux periodic-looking. Test silence-like inputs for "never settles".
- **Header comments drift**: the tap header claimed a 3.2 kHz passband (the
  -6 dB point) and `readTap` one second of queue (it is 65536 samples).
  Measure, then write the comment.
- **zsh is not bash**: the scripts are bash (`#!/bin/bash`); run them, do not
  source them.
- **The shared machine**: iterate arm64-only in `build/`; `verify.sh` builds
  its own directories under `build-verify/`.

## Credits

spasis (capture, ring, Silent, the width formula, the device-by-name rule)
and pattern v0.1.1 (the onset priming idea, the host-path detector, the
tempo detector) are Stoatworks repos; miniaudio is David Reid's, vendored
unmodified. See ATTRIBUTIONS.md.
