# earshot

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. What its offline
> harness (`estest`) proves, on synthetic audio, at 44.1 and 48 kHz and at two
> block sizes each: pitch within **0.1 cent** on sines from 40 Hz to 2 kHz and
> within **2.7 cents** at 10 dB SNR, with the confidence equal to S/(S+N) within
> 0.0004; onsets that hear a kick **8-13 ms** after the stream starts (no
> deaf first second) and fire **nothing** on held chords, vibrato or a 70 dB
> swell; a third-octave bank whose every band sits **inside the IEC 61260
> class 1 mask** (0.89 dB to spare on the skirts); a tempo within **0.09 BPM**
> on five grooves by 4 s, including the four-on-the-floor at 125 that pattern
> v0.1.0 read as 62, and within 0.22 BPM on pattern's real release-video
> soundtrack; a waveform tap that is the input sample for sample; and **no
> allocation** on the render or audio thread. Every one of its 65 checks
> has a negative control, a planted defect it must fail, and all 51 do (one
> only at one sample rate, a statistical effect; one, for tempo, fails four
> of the five grooves but never the shuffle); fifteen further planted bugs
> are all caught.
> **No plugin uses it yet. It has never opened a live audio device**, from a
> host or anywhere else, and **it has never been compiled with MSVC**.

A shared live-audio front end for the Stoatworks FFGL plugins. One C++17
library, pulled into a plugin with one `add_subdirectory`, that opens its own
capture device and hands the plugin, once a frame, what the fleet's
audio-reactive plugins keep needing and Resolume's FFT cannot give them.

## The one idea

An FFGL plugin gets one audio input: `FF_USAGE_FFT`, 64 bins of a mono sum,
no phase, no channels, no time domain, and a bin law nobody has measured.
That is enough for a bar that bounces. It is not enough for a tuner, a
third-octave analyser whose 25 Hz band is 6 Hz wide, a stereo scope, a
tracker that hears the first beat, or a loudspeaker model that needs the
waveform as a stream. Each plugin that wanted one of those reinvented its
own capture. earshot is that work done once, with the fleet's lessons kept:

- **Devices by name, never by index**, and a missing device is Missing, not
  silently swapped for the laptop microphone (spasis).
- **Silent is a state**: a device that opens and returns bit-exact zeros
  (no microphone permission, an unpatched Dante receiver) is shown as dead,
  not papered over (spasis).
- **Opening a device never blocks the render thread** (spasis's measured
  bug, fixed here with a worker thread).
- **Onsets are primed on the first frame** (pattern's deaf-for-1.5-s bug),
  and a hard attack's click lights only the bands it really reaches
  (pattern's broadband lesson).
- **A tempo at the right metre** (pattern v0.1.0 read 62 for 125).
- With no device chosen, it falls back to the host's FFT for band envelopes
  and onsets, and leaves everything else **empty** rather than guessed.

## What a plugin gets

Every frame, from `engine.update( hostSeconds, fftBins, 64 )`:

| | What | Latency |
| --- | --- | --- |
| Pitch | MPM, 40 Hz-2 kHz, a confidence that is S/(S+N), octave-safe | ~38 ms |
| Partials | the strongest sines, QIFFT-refined, labelled by harmonic | 85 ms |
| Onsets | per band, primed on frame one, masked against clicks | one 21 ms window |
| Band envelopes | Parseval-correct levels, attack/release | one hop |
| Thirds | ISO 266 / IEC 61260 base-ten bank, 20 Hz-20 kHz, Fast weighting, peak hold | filter delay + 125 ms |
| Tempo | pattern v0.1.1's three-register detector, gated on onsets | settles in ~4 s |
| Stereo | correlation, width, balance, levels of a chosen channel pair | 21 ms |
| Waveform tap | the pair resampled to a fixed rate, as a stream | 1.6 ms |

The calls, the thread rules, how to save the device choice in a composition,
and what to show for each capture state are in
[docs/INTEGRATING.md](docs/INTEGRATING.md). The API is one header,
[include/earshot/earshot.h](include/earshot/earshot.h), documented in place.

```cmake
add_subdirectory( external/earshot )
target_link_libraries( MyPlugin PRIVATE earshot::earshot )
```

## Status

**2026-11-16: a library with a harness, and no user yet.** Local repository;
intended home `github.com/stoatworks-labs/earshot`.

Verified by measurement on one Apple Silicon Mac (macOS 26.4), every check in
four cells (44.1 and 48 kHz, blocks of 128 and 1031 samples, 60 fps), with
`tools/verify.sh` green (universal build, estest native and under Rosetta,
and again built without fused multiply-add):

- **Pitch.** Sines 40 Hz-2 kHz: worst 0.10 cents (limit 1, from MPM's
  parabola bias). Missing fundamental (harmonics 2-6): 0.02 cents. Residue
  tones (harmonics 9-12, 15-19): 0.01 cents. Sub-octave and strong second
  harmonic traps: 2.6 cents (limit 5). A sine in white noise at 10 dB SNR:
  2.7 cents, confidence within 0.0004 of S/(S+N). Vibrato: always inside the
  window's instantaneous range.
- **Partials.** Frequency within 0.0159 bin (Hann QIFFT theory: 0.0160).
- **Onsets.** A kick 0.25 s after the stream starts on a playing mix: heard
  in 8-13 ms; the mix itself fires nothing. 1 ms attacks on 90 Hz, 300 Hz,
  1.5 kHz, 8 kHz: each heard within 6.3 ms, nothing beyond the click's
  physical reach. 30 and 80 ms rises: one onset each. Claps: every band they
  reach. 12 s each of noisy pads, a 41 Hz bass, sung vibrato, an organ
  chord: no onsets. White noise: one per 12 s in the lowest band (a known
  limit, AGENTS.md).
- **Thirds.** Mid-bands exact, ISO labels, 30 bands at 44.1 kHz and 31 at
  48. Every band's response inside IEC 61260:1995 class 1 on 4,000 points
  per band: 0.89 dB to spare on the skirts; edge attenuation 3.0099-3.0103 dB
  (class 1 allows 2.0-5.0). The running filters match that response end to
  end within 0.0004 dB. The Fast level decays at 34.744 dB/s (the 125 ms
  time constant's 34.74), and the peak hold holds for its second and falls
  at its 20 dB/s.
- **Tempo.** Five synthetic grooves (four-on-the-floor 125, kick 128, break
  96, drum and bass 174, triplet shuffle 110): settled at 4.0 s, every frame
  from 6 s within 0.09 BPM; the same at 24, 30 and 60 fps. White noise and a
  held chord never settle. pattern's release-video soundtrack: 124.96 BPM,
  every frame from 6 s within 0.22 BPM, at 30 and 60 fps.
- **Stereo and tap.** Correlation, width and balance exact to 1e-5 on tones;
  the tap equal to the analytic signal within 2.3e-5 of full scale, 87 dB of
  alias rejection, flat to 0.0004 dB at 2 kHz.
- **Capture logic**, against a scripted device: a 300 ms open never blocks
  `update()` (worst 0.7 ms unloaded); a missing device is retried and opens at 2.1 s,
  the default never asked for; unplugged, Missing in 7 ms and back as a new
  stream; Silent at exactly 3.000 s of zeros, never on -120 dBFS noise;
  210 million frames through the ring from a racing thread, no torn read; a
  parameter change keeps the audio history and the settled tempo.
- **No allocation** in `feed()`, `update()`, `readTap()` or the device
  callback.

Not verified:

- **It has never opened a live device.** The miniaudio backend has listed
  this Mac's 11 capture devices and nothing more; `estest --live` exists and
  has not been run.
- **No plugin uses it**, so it has never run inside Resolume.
- **MSVC has never compiled it.** It is written to be portable; that is
  intent, not a result.
- Everything is synthetic audio except one real track (the tempo).

## Building and testing

```
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build -j4
build/tools/estest/estest          # every check, every cell, about a minute
tools/verify.sh                    # everything, about ten minutes
```

`CLAUDE.md` is the command reference; `AGENTS.md` is why everything is the
way it is, and the traps.

## Licence

MIT, copyright Stoatworks. miniaudio (vendored) is public domain or MIT-0.
spasis and pattern, which much of this comes from, are MIT. See
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).
