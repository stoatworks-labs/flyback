/*
    The page's way in to the singing coil's pitch input: what the plugin's
    ProcessOpenGL does with `sing::PitchInput`, as C functions JavaScript can
    call. (woofer's and rubens's glue, cut down to the one class flyback's page
    needs: the rest of the plugin in the browser is a JavaScript port.)

    Nothing here is the pitch input, and nothing here does its work. That is
    `source/Singing.cpp` -- PitchInput's Apply, Feed and Update, the law (a
    voiced frame sets the note, unvoiced sound above the Gate sustains it,
    below the Gate is a rest that forgets it) and RateFor's fold into 5..1000
    -- and earshot's snapshot under it (8124191, unpatched: MPM pitch with an
    NSDF clarity, its capture state and its generation count), compiled
    UNMODIFIED into the same WebAssembly module as this file (the list is in
    demo/tools/build-wasm.sh). This file only plays the plugin's part:

      - `fs_new` constructs a PitchInput, as FlybackPlugin's constructor does;
      - `fs_apply` hands it the Pitch Source value, as ProcessOpenGL does every
        frame (`pitch.Apply( effective[ PT_PITCH_SOURCE ], rescanPending )`;
        the page has no Rescan Inputs, so `rescan` is always false);
      - `fs_update` is `pitch.Update( now, r.octaves, r.gateDb )`, and hands
        the page the Note the plugin turns into `Settings::singRate`.

    One thing here is the plugin's HARNESS rather than the plugin: the audio.
    hvtest --tone / --wav never opens a device. It sets FLYBACK_NO_DEVICES=1,
    so PitchInput::Select records the request and never calls selectDevice,
    and hands the samples to `PitchInput::Feed`, which is earshot's
    `feedBegin` and `feed()`, the path a device's samples take inside earshot.
    So does this page (`fs_prepare`, `fs_feed`): the test tune, or a file the
    visitor chose, decoded by the browser.

    No audio device exists in this module: earshot is built with
    EARSHOT_CAPTURE=OFF (src/NoCapture.cpp; miniaudio is not compiled, and
    miniaudio.h is never included), so there is no backend to list or open,
    and FLYBACK_NO_DEVICES=1 keeps the pitch input from asking. `fs_counters`
    hands the page Singing.cpp's own counts of every call that could reach a
    device, so the page can show them: the listings and selects stay 0.
*/
#include "Singing.h"

#include <earshot/earshot.h>

#include <emscripten/emscripten.h>

#include <cstdlib>

using namespace flyback;

namespace
{
/// One Note, flattened for the page (it reads HEAPF64 at the pointer).
/// The order is the page's: demo/singing.js, NOTE_FIELDS.
double g_note[ 12 ] = {};
int g_counters[ 5 ]  = {};
} // namespace

extern "C"
{
/// Before the first instance: FLYBACK_NO_DEVICES=1, hvtest's switch (its
/// ScopedNoDevices, and verify.sh's for oxbow), so the pitch input never lists
/// and never selects. A build with no capture backend could open nothing
/// anyway; this keeps it from asking.
EMSCRIPTEN_KEEPALIVE void fs_prepare()
{
	setenv( "FLYBACK_NO_DEVICES", "1", 1 );
}

EMSCRIPTEN_KEEPALIVE sing::PitchInput* fs_new()
{
	return new sing::PitchInput();
}

EMSCRIPTEN_KEEPALIVE void fs_delete( sing::PitchInput* pitch )
{
	delete pitch;
}

/// System default's element value, the key hvtest's --set "Pitch Source=System
/// default" sets (earshot::deviceKey( kDefaultDevice )).
EMSCRIPTEN_KEEPALIVE float fs_default_key()
{
	return earshot::deviceKey( earshot::kDefaultDevice );
}

/// The Pitch Source value, as ProcessOpenGL applies it: 0 is Off. Returns
/// whether the menu changed (it does on the first switch-on; the page has no
/// device menu to refill).
EMSCRIPTEN_KEEPALIVE int fs_apply( sing::PitchInput* pitch, float value )
{
	return pitch->Apply( value, false ) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int fs_started( sing::PitchInput* pitch )
{
	return pitch->Started() ? 1 : 0;
}

/// Audio as if from a device: interleaved float frames.
EMSCRIPTEN_KEEPALIVE void fs_feed( sing::PitchInput* pitch, const float* interleaved, int frames, int rate, int channels )
{
	pitch->Feed( interleaved, frames, rate, channels );
}

/// pitch.Update( now, octaves, gateDb ), flattened.
EMSCRIPTEN_KEEPALIVE const double* fs_update( sing::PitchInput* pitch, double now, int octaves, double gateDb )
{
	const sing::Note n = pitch->Update( now, octaves, gateDb );
	g_note[ 0 ]  = n.active ? 1.0 : 0.0;
	g_note[ 1 ]  = n.voiced ? 1.0 : 0.0;
	g_note[ 2 ]  = n.sounding ? 1.0 : 0.0;
	g_note[ 3 ]  = n.hz;
	g_note[ 4 ]  = n.rate;
	g_note[ 5 ]  = n.frameHz;
	g_note[ 6 ]  = n.levelDb;
	g_note[ 7 ]  = n.confidence;
	g_note[ 8 ]  = static_cast< double >( static_cast< int >( n.lamp ) );
	g_note[ 9 ]  = n.blinkOn ? 1.0 : 0.0;
	g_note[ 10 ] = static_cast< double >( n.generation );
	g_note[ 11 ] = 0.0;
	return g_note;
}

/// Singing.cpp's process-wide counts: engines, device listings, selects,
/// feedBegins, earshot updates.
EMSCRIPTEN_KEEPALIVE const int* fs_counters()
{
	const sing::Counters c = sing::CountersForTest();
	g_counters[ 0 ]        = c.engines;
	g_counters[ 1 ]        = c.lists;
	g_counters[ 2 ]        = c.selects;
	g_counters[ 3 ]        = c.feeds;
	g_counters[ 4 ]        = c.updates;
	return g_counters;
}
} // extern "C"
