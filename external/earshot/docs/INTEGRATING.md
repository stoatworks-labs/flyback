# Putting earshot in a plugin

This is for the author of an FFGL plugin that wants live audio. The API is
`include/earshot/earshot.h`, documented in place; this page is the order to
do things in, the thread rules, and what to show the operator.

## CMake

Put earshot in the plugin's tree, as a copied snapshot or a submodule (both
work: it generates nothing and installs nothing):

```
external/earshot/          # a copy of this repository, or a submodule
```

```cmake
add_subdirectory( external/earshot )
target_link_libraries( MyPlugin PRIVATE earshot::earshot )
```

That is all. What it brings with it:

- C++17 (`cxx_std_17` is a PUBLIC requirement), Threads, and on macOS the
  CoreAudio, AudioToolbox and CoreFoundation frameworks (PUBLIC, because a
  static library's frameworks must reach the plugin's link line).
- Position-independent code and hidden visibility: nothing of earshot's is
  exported from your bundle or DLL. `tools/consumer` proves it: the test
  module exports one symbol, its own.
- As a subdirectory earshot sets no architectures and no deployment target;
  your `CMAKE_OSX_ARCHITECTURES` (universal for the fleet) decides.
- `-DEARSHOT_CAPTURE=OFF` builds without miniaudio: `feed()` and the host
  FFT only, `listDevices()` empty, `selectDevice()` reports Failed with a
  reason.
- The tools (`estest`) are not built unless earshot is the top-level
  project.

Windows: the CMake carries `/utf-8 /W4 /permissive-` and `NOMINMAX` for
MSVC, but **earshot has never been compiled with MSVC**. Expect to be the
first.

## The calls

```cpp
#include "earshot/earshot.h"

class MyPlugin : public CFFGLPlugin
{
    earshot::Engine engine_;          // a member: allocates everything here, once
    std::vector< earshot::DeviceInfo > devices_;
    ...
};

// Constructor (and on a Rescan event): fill the device menu.
devices_ = earshot::listDevices();    // talks to the OS; never per frame; opens nothing

// When the Device parameter changes (any thread but the audio thread):
engine_.selectDevice( name );         // returns at once; the worker opens it
// "" or nullptr closes it (host FFT); earshot::kDefaultDevice is the system default

// ProcessOpenGL, once per frame:
const earshot::Frame& f = engine_.update( hostSeconds, fftBins, 64 );
// fftBins: the FF_USAGE_FFT buffer, or nullptr if the plugin did not ask for it

// If the plugin wants the waveform as a stream (a physical model, a recorder):
float l[ 2048 ], r[ 2048 ];
int n = engine_.readTap( l, r, 2048 ); // every sample since the last call, once, at Config::tapRate
```

`Config` holds the knobs. The fields above "May change at any time" are
read once at construction (`maxChannels`, `ringSeconds`); everything below
can be changed with `engine_.configure( config )` from a parameter handler,
and **never clears the audio history** (spasis's rule; `estest --params`).

## Threads

- **Render thread** (ProcessOpenGL): `update`, `frame`, `readTap`,
  `peekWaveform`. All analysis runs inside `update`; nothing there allocates,
  locks against the audio thread, or waits. The Frame is written and read on
  this thread, so reading it needs no lock. If you read it from another
  thread, copy it under your own lock.
- **Parameter thread(s)**: `selectDevice`, `configure`, `captureState`,
  `status`, `listDevices` are safe from any thread except the audio thread.
  `listDevices` talks to the OS: constructor or Rescan only.
- **Audio thread**: miniaudio's own, inside earshot. A plugin with its own
  source calls `feedBegin( rate, channels )` once and then `feed()` from
  exactly one thread.
- **The device worker** is earshot's. It starts on the first `selectDevice`
  with a name, not in the constructor, so a host that constructs instances
  only to scan them never starts a thread or touches CoreAudio.

Two plugin instances have two Engines and share nothing, two streams
included.

## Device choice as an FFGL parameter

Save the device **by name, never by its position in the list**. A position
saved in a composition reopens on whatever is in that slot tomorrow (spasis's
rule: a set saved against a Dante feed that reopens on the laptop microphone
is worse than one that says the feed is gone). Two ways:

**A text parameter** (preferred where the host shows text): the value is the
device name. Unknown names are fine: the device is Missing until it appears,
and the worker retries every 2 s.

**An option parameter**: give each element the value
`earshot::deviceKey( name )`, not its index. The key is the name's FNV-1a
hash folded to 23 bits, exact in a float, so it survives the host's save. On
load, look the saved key up in the current list:

```cpp
float saved = /* the option's value */;
for( const auto& d : devices_ )
    if( earshot::deviceKey( d.name.c_str() ) == saved )
        engine_.selectDevice( d.name.c_str() );
```

If no current device has that key, keep the saved value, show the device as
Missing, and do not fall back to the default silently. (Keep the last name
you saw for that key in your own state if you can, so a log line can say
what is missing.) Put "None (host FFT)" first in the menu, with the value 0
and `selectDevice( "" )`; put "System default" second, with
`deviceKey( kDefaultDevice )`.

Channels: earshot takes the device's first `maxChannels` channels (2 by
default; never a downmix), and the stereo pair is `pairFirst` and
`pairFirst + 1`. A plugin that offers a channel-pair parameter sets
`pairFirst`; that is a runtime field.

## What to show for each CaptureState

FFGL gives a plugin no text and no window, so the state goes to the plugin's
diagnostics log (with `status()`'s strings) and to whatever non-text signal
the plugin can draw. spasis brightens its graticule; a meter can dim; a
spectrum can draw a dashed baseline.

| State | Meaning | Analysis comes from | Show |
| --- | --- | --- | --- |
| Closed | No device chosen | the host FFT, if passed (`Source::Host`) | normal; levels are relative (`calibrated` false) |
| Opening | The worker is opening the device | the host FFT, if `fallbackWhenDeviceDown` | a brief "waiting" cue at most; usually under a second |
| Running | Samples are arriving, not all zero | the device (`Source::Device`) | normal |
| Silent | Open, running, bit-exactly zero for 3 s | the device (zeros), never the host | **the loud one**: no permission (macOS gives a host a device that returns zeros, not an error), an unpatched Dante receiver, a loopback nothing is routed to. The operator must see it is dead |
| Missing | The named device is not connected | the host FFT, if `fallbackWhenDeviceDown` | "device missing", with the name; it comes back by itself when plugged in |
| Failed | The backend refused it | the host FFT, if `fallbackWhenDeviceDown` | the reason (`Status::error`); retried every 2 s |

`toString( state )` gives the state's name for logs. `Status` has the
requested and opened names, the error, the sample rate and the channel
counts, in fixed buffers (it never allocates).

On the host fallback (`Source::Host`), only band envelopes and onsets
exist. Pitch, partials, thirds, tempo, stereo and the tap are empty
(`valid` false, counts 0), on purpose: draw nothing rather than a guess.
The host bins' law is an assumption you should expose as parameters
(`Config::hostBins`: Magnitude or Power, Linear or Log, and the sample
rate). Under the Linear law at 44.1 kHz the lowest band (30-150 Hz) gets no
bin at all: 64 linear bins start at 172 Hz.

## Results worth knowing about

- `Band::onsetCount` is monotonic: keep the last value you saw and compare,
  rather than relying on `onset` (which is "since the previous update").
- `Frame::generation` bumps whenever the stream restarts (a new device, a
  rate change, a reopen after unplugging): reset anything you accumulate.
- `Tempo::settled` means two readings a second apart agreed within 2 %; the
  first appears about 4 s into music with onsets. It never settles on
  material with no onsets (a held chord, noise), and a settled tempo is held
  through a breakdown.
- `Pitch::confidence` is S/(S+N) for a tone in noise, not a tuned score.
- Latencies (each module's header has the detail): onsets within one 21 ms
  window plus the frame; pitch about 38 ms behind the newest sample; partials
  85 ms; stereo 21 ms; thirds the filter's group delay (160 ms at 25 Hz, 4 ms
  at 1 kHz) plus the 125 ms Fast weighting; tempo 4 s to settle; the tap
  1.6 ms plus however long you wait to read it.
- `readTap`'s queue holds 65536 samples (8.2 s at 8 kHz); anything older is
  dropped and counted in `Frame::tapDropped`.
