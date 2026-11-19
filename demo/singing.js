/**
 * The singing coil, for the browser demo: the plugin's pitch input in
 * WebAssembly, the audio it is fed, and the lamp.
 *
 * Three pieces, and they are not equally faithful:
 *
 *   The pitch is the plugin's. `flyback-sing.wasm` is source/Singing.cpp
 *   (sing::PitchInput: Apply, Feed, Update, the law and RateFor's fold) and
 *   earshot's snapshot under it, compiled unmodified by
 *   demo/tools/build-wasm.sh; demo/wasm/glue.cpp exposes the three calls the
 *   plugin's ProcessOpenGL makes, and demo/tools/check_shaders.py fails when
 *   one of its inputs has changed since it was built. The Note this hands
 *   back -- the held pitch, the rate in bangs a second, the lamp -- is the
 *   C++'s, with WebAssembly's arithmetic (no fused multiply-add, musl's libm).
 *
 *   The audio is the harness's way, not a host's. Nothing here asks for a
 *   microphone or lists a device: the module has no capture backend, and the
 *   pitch input runs under FLYBACK_NO_DEVICES=1, as hvtest --tone / --wav runs
 *   it, and is handed samples through earshot's feed(). Up to the page's clock
 *   before each frame (hvtest's SingEngine: before frame i, every sample up to
 *   i / fps, audio time 0 at the stream's first frame), the test tune
 *   (tune.js) or a file the visitor chose, decoded by the browser, looped.
 *   Nothing is played aloud.
 *
 *   The interrupter the Note drives is NOT here and not compiled: it is
 *   Engine.cpp's StepSung, ported by hand into engine.js with the rest of the
 *   engine, and nothing checks it but a reader.
 *
 * The lamp's fragment shader is source/render/Lamp.cpp's kLampFragment,
 * verbatim (check_shaders.py compares it); its colours and placement
 * (LampPainter::Colour and ::Place) are ported by hand below.
 *
 * Nothing exists until Pitch Source is first switched on, as in the plugin:
 * with it Off the page never fetches the module, makes no pitch input and
 * draws no lamp, and the picture path is the one it always was.
 */

import { renderTune, TUNE_RATE } from './tune.js';
import { Program } from './vendor/gl.js';

/** The page's Pitch Source menu (the plugin's is Off, System default, then devices). */
export const PITCH_SOURCES = ['Off (BPS)', 'Test tune', 'Your audio file'];
export const SOURCE_OFF = 0;
export const SOURCE_TUNE = 1;
export const SOURCE_FILE = 2;

/** sing::Lamp. */
export const Lamp = { Off: 0, Running: 1, Silent: 2, Missing: 3, Waiting: 4 };
export const LAMP_WORDS = ['off', 'green: samples arriving', 'red: open, and silent', 'blinking red: missing', 'amber: nothing open yet'];

/** What fs_update writes, in glue.cpp's order. */
const NOTE_FIELDS = ['active', 'voiced', 'sounding', 'hz', 'rate', 'frameHz', 'levelDb', 'confidence', 'lamp', 'blinkOn', 'generation'];
const BOOL_FIELDS = new Set(['active', 'voiced', 'sounding', 'blinkOn']);

/** sing::Note with the mode off: Update's answer when !Active(). */
export const OFF_NOTE = Object.freeze({
  active: false, voiced: false, sounding: false, hz: 0, rate: 0, frameHz: 0, levelDb: -200, confidence: 0,
  lamp: Lamp.Off, blinkOn: true, generation: 0,
});

/** sing::PitchInput::Update's blink: 2 Hz on the host's clock. */
const blinkAt = (seconds) => (Math.abs(seconds) % 0.5) < 0.25;

/**
 * The mode on with nothing to ask yet -- the module still loading, or a
 * frame drawn again while paused before any audio was analysed: what Update
 * says while the pitch input has no engine (Waiting), at rate 0.
 */
function waitingNote(seconds, lamp = Lamp.Waiting) {
  return { ...OFF_NOTE, active: true, lamp, blinkOn: blinkAt(seconds) };
}

const FEED_CHUNK = 4096; // frames per fs_feed call
const FEED_MAX_SECONDS = 2.0; // further behind than this and the rest is dropped, as a reader that fell behind drops it

/**
 * The plugin's pitch input and what it hears.
 *
 * `frame()` is ProcessOpenGL's singing block -- `pitch.Apply( value )`, then
 * `pitch.Update( now, r.octaves, r.gateDb )` -- with the harness's feed before
 * it. A new stream (the source changed, the tune restarted, a new file, a new
 * plugin instance) is the plugin's own path: Pitch Source to Off and back to
 * System default, so PitchInput::Select ends the feed and forgets the note,
 * and the next Feed is earshot's feedBegin, a new generation.
 */
export class Singer {
  /**
   * @param {object} [options]
   * @param {() => Promise<object>} [options.load] makes the emscripten module
   *   (the page: import('./flyback-sing.js'); a test hands it the bytes)
   */
  constructor({ load } = {}) {
    this.load = load ?? (async () => (await import('./flyback-sing.js')).default());
    this.module = null;
    this.loading = null;
    this.error = null;
    this.pitch = 0;
    this.applied = 0;
    this.scratch = 0;
    this.tune = null;
    this.file = null; // { name, samples (stereo interleaved), frames, channels: 2, rate, sourceChannels, seconds }
    this.fileEpoch = 0;
    this.note = OFF_NOTE;
    this.stream = null; // { source, epoch, t0, fed }
    this.lastTime = null;
  }

  /** A new plugin instance (the Plugin switch): a new pitch input next frame. */
  reset() {
    if (this.module && this.pitch) this.module._fs_delete(this.pitch);
    this.pitch = 0;
    this.applied = 0;
    this.stream = null;
    this.lastTime = null;
    this.note = OFF_NOTE;
  }

  /** The visitor's file, already decoded: see decodeFile. A new stream. */
  setFile(file) {
    this.file = file;
    this.fileEpoch += 1;
  }

  /** Start fetching the module (once). Resolves when it is usable or has failed. */
  ensureModule() {
    if (this.module || this.error) return Promise.resolve(this.module);
    if (!this.loading) {
      this.loading = Promise.resolve()
        .then(() => this.load())
        .then((module) => {
          module._fs_prepare();
          this.scratch = module._malloc(FEED_CHUNK * 2 * 4);
          this.module = module;
          return module;
        })
        .catch((error) => {
          this.error = error;
          // eslint-disable-next-line no-console
          console.error('flyback: the pitch module could not load', error);
          return null;
        });
    }
    return this.loading;
  }

  signalFor(source) {
    if (source === SOURCE_TUNE) {
      this.tune ??= renderTune();
      return this.tune;
    }
    return this.file;
  }

  /**
   * One frame. `source` is the page's Pitch Source index; `time` the page's
   * clock (the host's, in seconds); `still` a frame drawn again while paused,
   * which feeds nothing and asks nothing (the host would not render the same
   * instant twice). Returns the Note.
   */
  frame({ source, time, still, octaves, gateDb }) {
    if (source === SOURCE_OFF) {
      // Off: the device closed and the engine kept, as PitchInput::Apply does.
      if (this.pitch && this.applied !== 0) this.module._fs_apply(this.pitch, 0);
      this.applied = 0;
      this.stream = null;
      this.lastTime = time;
      this.note = OFF_NOTE;
      return this.note;
    }

    if (!this.module) {
      this.ensureModule();
      this.note = waitingNote(time, this.error ? Lamp.Missing : Lamp.Waiting);
      return this.note;
    }
    const m = this.module;
    if (!this.pitch) this.pitch = m._fs_new();

    const key = m._fs_default_key();
    const epoch = source === SOURCE_FILE ? this.fileEpoch : 0;
    const restarted = this.lastTime !== null && time < this.lastTime;
    const s = this.stream;
    if (this.applied !== key || !s || s.source !== source || s.epoch !== epoch || restarted) {
      if (this.applied === key) m._fs_apply(this.pitch, 0);
      m._fs_apply(this.pitch, key);
      this.applied = key;
      this.stream = { source, epoch, t0: time, fed: 0 };
    }

    if (still) {
      if (!this.note.active) this.note = waitingNote(time);
      return this.note;
    }

    const signal = this.signalFor(source);
    if (!signal || signal.frames === 0) {
      // A file chosen in the menu but none given: nothing arrives, as from a
      // device that has not opened, and the plugin says Waiting. Not asked of
      // Update: under FLYBACK_NO_DEVICES Select never reaches earshot, so a
      // feed that stopped would still report its last stream's note.
      this.note = waitingNote(time);
      this.lastTime = time;
      return this.note;
    }
    this.feed(this.stream, signal, time);
    const at = m._fs_update(this.pitch, time, octaves, gateDb) >> 3;
    const note = {};
    NOTE_FIELDS.forEach((name, i) => {
      const v = m.HEAPF64[at + i];
      note[name] = BOOL_FIELDS.has(name) ? v !== 0 : v;
    });
    this.note = note;
    this.lastTime = time;
    return note;
  }

  /** Every sample up to the page's clock, through feed(), looped. */
  feed(stream, signal, time) {
    const m = this.module;
    const target = Math.round((time - stream.t0) * signal.rate);
    if (target - stream.fed > FEED_MAX_SECONDS * signal.rate) stream.fed = target - FEED_MAX_SECONDS * signal.rate;
    const heap = m.HEAPF32;
    const base = this.scratch >> 2;
    while (stream.fed < target) {
      const n = Math.min(FEED_CHUNK, target - stream.fed);
      for (let k = 0; k < n; k += 1) {
        const at = ((stream.fed + k) % signal.frames) * 2;
        heap[base + 2 * k] = signal.samples[at];
        heap[base + 2 * k + 1] = signal.samples[at + 1];
      }
      m._fs_feed(this.pitch, this.scratch, n, signal.rate, 2);
      stream.fed += n;
    }
  }

  /** Singing.cpp's counts: engines, device listings, selects, feedBegins, updates. */
  counters() {
    if (!this.module) return null;
    const at = this.module._fs_counters() >> 2;
    const h = this.module.HEAP32;
    return { engines: h[at], lists: h[at + 1], selects: h[at + 2], feeds: h[at + 3], updates: h[at + 4] };
  }

  /** Seconds of audio fed in the current stream. */
  fedSeconds() {
    const s = this.stream;
    const signal = s ? this.signalFor(s.source) : null;
    return s && signal ? s.fed / signal.rate : 0;
  }
}

/**
 * Decode a file the visitor chose. An OfflineAudioContext renders to memory,
 * not to an output device, and decodeAudioData resamples to its rate (the
 * tune's, 48 kHz). A mono file goes to both channels; of more than two, the
 * first two, as earshot takes the first channels of a device. Nothing leaves
 * the page and nothing is played.
 */
export async function decodeFile(file) {
  const bytes = await file.arrayBuffer();
  const context = new OfflineAudioContext(2, 1, TUNE_RATE);
  const buffer = await context.decodeAudioData(bytes);
  const left = buffer.getChannelData(0);
  const right = buffer.numberOfChannels > 1 ? buffer.getChannelData(1) : left;
  const frames = buffer.length;
  const samples = new Float32Array(2 * frames);
  for (let i = 0; i < frames; i += 1) {
    samples[2 * i] = left[i];
    samples[2 * i + 1] = right[i];
  }
  return { name: file.name, samples, frames, channels: 2, rate: TUNE_RATE, sourceChannels: buffer.numberOfChannels, seconds: frames / TUNE_RATE };
}

//---------------------------------------------------------------------------
// The lamp: render/Lamp.cpp.
//---------------------------------------------------------------------------

// kLampFragment, source/render/Lamp.cpp. Do not edit here: check_shaders.py
// compares it with the C++ character for character.
const LAMP_FRAGMENT = `#version 410 core

uniform vec2 Centre;//window pixels
uniform float Radius;//the lit disc, pixels
uniform vec3 Colour;

out vec4 fragColour;

void main()
{
	float d     = length( gl_FragCoord.xy - Centre );
	float lit   = clamp( Radius - d + 0.5, 0.0, 1.0 );
	float bezel = clamp( 1.6 * Radius - d + 0.5, 0.0, 1.0 );
	fragColour  = vec4( Colour * lit + vec3( 0.03, 0.03, 0.035 ) * ( bezel - lit ), bezel );
}
`;

export { LAMP_FRAGMENT };

const f32 = Math.fround;

/** LampPainter::Colour: display values; unlit grey for a blinking lamp in its dark half. */
export function lampColour(state, blinkOn) {
  let r = 0.12, g = 0.12, b = 0.12; // unlit
  switch (state) {
    case Lamp.Running: r = 0.10; g = 0.85; b = 0.25; break;
    case Lamp.Silent: r = 0.90; g = 0.08; b = 0.05; break;
    case Lamp.Missing: if (blinkOn) { r = 0.90; g = 0.08; b = 0.05; } break;
    case Lamp.Waiting: if (blinkOn) { r = 1.00; g = 0.55; b = 0.05; } break;
    default: break;
  }
  return [f32(r), f32(g), f32(b)];
}

/**
 * LampPainter::Place, for a viewport (x, y, w, h): never smaller than three
 * pixels, 1.2% of the frame's height above 250 lines, one radius of margin
 * outside the bezel. Window pixels, GL's y up. In float, as the C++.
 */
export function lampPlace(x, y, w, h) {
  const radius = Math.max(3.0, f32(f32(0.012) * h));
  return {
    radius,
    x: f32(x + f32(f32(2.6) * radius)),
    y: f32((y + h) - f32(f32(2.6) * radius)),
  };
}

/**
 * LampPainter::Draw over WebGL2: over everything, into the canvas, ONLY while
 * the mode is on. The shader compiles on first use, and a lamp that will not
 * compile is logged and skipped, never fatal, as in the plugin.
 */
export class LampPainter {
  constructor(gl, quad, quadVertex) {
    this.gl = gl;
    this.quad = quad;
    this.quadVertex = quadVertex;
    this.program = null;
    this.tried = false;
    this.last = null;
  }

  ensure() {
    if (this.tried) return this.program !== null;
    this.tried = true;
    try {
      this.program = new Program(this.gl, this.quadVertex, LAMP_FRAGMENT, 'lamp');
    } catch (error) {
      // eslint-disable-next-line no-console
      console.error("flyback: the singing lamp's shader failed to compile: the coil sings without it", error);
      this.program = null;
    }
    return this.program !== null;
  }

  draw(state, blinkOn, width, height) {
    this.last = null;
    if (state === Lamp.Off || width <= 0 || height <= 0) return;
    if (!this.ensure()) return;
    const gl = this.gl;
    const p = lampPlace(0, 0, width, height);
    const rgb = lampColour(state, blinkOn);
    const reach = f32(f32(1.6) * p.radius + 1.0);
    const x0 = Math.max(0, Math.floor(p.x - reach));
    const y0 = Math.max(0, Math.floor(p.y - reach));
    const x1 = Math.min(width, Math.ceil(p.x + reach));
    const y1 = Math.min(height, Math.ceil(p.y + reach));
    if (x1 <= x0 || y1 <= y0) return;

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(x0, y0, x1 - x0, y1 - y0);
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.ONE, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    const s = this.program.use();
    s.set('Centre', p.x, p.y);
    s.set('Radius', p.radius);
    s.set('Colour', rgb[0], rgb[1], rgb[2]);
    this.quad.draw();
    gl.useProgram(null);
    // What ScopedGLState puts back in the plugin.
    gl.disable(gl.BLEND);
    gl.viewport(0, 0, width, height);
    this.last = { ...p, rgb, state };
  }
}
