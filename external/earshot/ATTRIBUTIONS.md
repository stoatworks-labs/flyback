# Attributions

earshot is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

## Code we vendored

### miniaudio — David Reid

<https://github.com/mackron/miniaudio>  
Version: 0.11.25 (2026-03-04), `third_party/miniaudio/miniaudio.h`, unmodified  
Licence: your choice of public domain (the Unlicense) or MIT No Attribution
(MIT-0); the full text is at the end of the header and in
`third_party/miniaudio/LICENSE`  
Copyright: David Reid (the MIT-0 alternative; 2026 in the header, 2025 in the upstream LICENSE file)

Capture device enumeration and the audio callback, on CoreAudio, WASAPI and
ALSA. Compiled as C++ in `src/MiniaudioImpl.cpp`, with its decoding,
encoding, generation, resource-manager, node-graph and engine parts
disabled (`src/MiniaudioConfig.h`). Its warnings are silenced for that
one file; they are not ours to fix.

## Code we derived from other people's work

### Capture, ring, Silent state and the stereo width — Stoatworks spasis

<https://github.com/stoatworks-labs/spasis>  
Licence: MIT  
Copyright: Stoatworks Labs

`src/Capture.*`, `src/Ring.h`, `src/Miniaudio.cpp` and the width formula in
`src/Stereo.cpp`. spasis's capture rules are kept: devices remembered by
name, never by index, and a missing one never swapped for the default; the
first n channels, never a downmix; Silent as open, running and bit-exactly
zero for three seconds, judged on the raw device buffer; the audio thread
never allocates or locks. Changed: opening and closing moved to a worker
thread (spasis opened on the render thread and listed this as its next
fix), and the ring's reader validates against the writer's claim as well as
its published count. `src/Dsp.h`'s constants and `src/Fft.*` follow the
same repo's conventions.

### Onset priming, the host-FFT onset detector and the tempo detector — Stoatworks pattern

<https://github.com/stoatworks-labs/pattern>  
Licence: MIT  
Copyright: Stoatworks Labs

`src/Tempo.*` is pattern v0.1.1's `Tracker::DetectTempo` (three registers,
S(T) = 0.5 R(T/2) + R(T) + R(2T), family then fastest level, its constants
unchanged), fed from earshot's own audio flux, with an onset gate added.
`src/HostFft.*` is pattern's linear-flux onset detector on the host's bins,
primed on the first frame. The idea of priming on frame one is pattern's;
the audio-path detector in `src/Onsets.*` replaced its seed (see AGENTS.md).
pattern v0.1.0's tempo detector survives only as the harness's negative
control.

## Methods from the literature

- Pitch: P. McLeod and G. Wyvill, "A smarter way to find pitch", ICMC 2005
  (the normalised square difference function and its key-maximum rule).
- Onsets: S. Boeck and G. Widmer, "Maximum filter vibrato suppression for
  onset detection", DAFx 2013 (the maximum filter on the previous frame).
- Partials: quadratically interpolated FFT peaks on the log magnitude
  (J. O. Smith, Spectral Audio Signal Processing, "Quadratic Interpolation
  of Spectral Peaks").
- Filters: R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad
  filter coefficients" (the pitch path's low-passes).
- Tap: J. F. Kaiser's window and his stopband formula,
  A = beta / 0.1102 + 8.7 dB.

## Standards

- ISO 266 (preferred frequencies) and IEC 61260-1 (base-ten octave-band
  filters): the third-octave bank's centres and edges. The harness judges
  the bank against IEC 61260:1995's class 1 limits, read from IS 6964 (the
  Bureau of Indian Standards' adoption of that table). No text of either
  standard is reproduced here beyond the breakpoint values the harness
  needs.
- IEC 61672-1's Fast time weighting (125 ms) for the third-octave levels.
