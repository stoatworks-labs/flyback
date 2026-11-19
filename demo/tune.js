/**
 * The singing coil's test tune: a melody this page synthesises, not a
 * recording of anyone's music.
 *
 * Plain sine tones at −12 dBFS, written out below as a score, rendered here
 * sample by sample at 48 kHz, in stereo with both channels the same. It is
 * built to show the law the plugin's pitch input follows (Singing.h), not to
 * be music:
 *
 *   - a rising arpeggio, A3 C4 E4 A4, legato: each new pitch is a voiced frame
 *     that sets the note, and the interrupter's phase runs on (no restart);
 *   - rests between phrases, silence well below any Gate: the coil stops and
 *     forgets the note, and the next phrase starts on its own pitch;
 *   - a glide, G3 to G4 in a second and a half: the rate follows it frame by
 *     frame, as a staircase at the frame rate;
 *   - a falling line of short, detached notes: the coil starts and stops with
 *     each, which is how music makes a singing coil flash;
 *   - A2 (110 bangs a second: short, fresh streamers), then A5 (880: long,
 *     re-used ones), then E6, 1318.5 Hz, above the coil's 1000 and folded an
 *     octave down to 659.3 bangs a second.
 *
 * Every pitch is between 110 and 1318.5 Hz, inside earshot's 40 to 2000 Hz.
 * Notes start and end with an 8 ms raised-cosine ramp (a step in level is a
 * click, broadband, which a pitch tracker hears as a burst of unvoiced
 * sound), and the phase is continuous through a legato change of pitch.
 *
 * `renderTune()` is plain arithmetic in double, rounded to float on the way
 * into the buffer, with no randomness, so the same samples come out every
 * time. demo/tools (not served) writes the same samples to a WAV to compare
 * the page's module with the plugin's harness (hvtest --demo-music).
 */

export const TUNE_RATE = 48000;
export const TUNE_LEVEL = 0.25; // a sine's peak: −12 dBFS
const RAMP = 0.008; // seconds

const A2 = 110.0, G3 = 196.0, A3 = 220.0, B3 = 246.94, C4 = 261.63, D4 = 293.66, E4 = 329.63, G4 = 392.0;
const A4 = 440.0, A5 = 880.0, E6 = 1318.51;

/**
 * The score, in seconds. A note is [hz, seconds] (a glide is [from, to,
 * seconds], exponential in frequency); 0 is a rest. Consecutive notes with no
 * rest between them are one legato phrase.
 */
export const SCORE = [
  [A3, 0.5], [C4, 0.5], [E4, 0.5], [A4, 1.5],
  0.5,
  [G3, G4, 1.5], [G4, 0.5],
  0.5,
  [E4, 0.375], 0.125, [D4, 0.375], 0.125, [C4, 0.375], 0.125, [B3, 0.375], 0.125,
  0.25,
  [A2, 1.0],
  0.25,
  [A5, 1.0],
  0.25,
  [E6, 0.5],
  1.0,
];

/** The score as phrases: { start, notes: [{ start, end, from, to }] }, seconds. */
function phrases() {
  const out = [];
  let t = 0;
  let current = null;
  for (const item of SCORE) {
    if (typeof item === 'number') {
      current = null;
      t += item;
      continue;
    }
    const [from, to, seconds] = item.length === 3 ? item : [item[0], item[0], item[1]];
    if (!current) {
      current = { start: t, end: t, notes: [] };
      out.push(current);
    }
    current.notes.push({ start: t, end: t + seconds, from, to });
    t += seconds;
    current.end = t;
  }
  return { phrases: out, seconds: t };
}

export const TUNE_SECONDS = phrases().seconds;

/** The tune, interleaved stereo float at TUNE_RATE: { samples, frames, channels, rate }. */
export function renderTune() {
  const { phrases: list, seconds } = phrases();
  const frames = Math.round(seconds * TUNE_RATE);
  const samples = new Float32Array(2 * frames);
  for (const phrase of list) {
    let phase = 0.0; // cycles, continuous through the phrase
    const first = Math.round(phrase.start * TUNE_RATE);
    const last = Math.min(frames, Math.round(phrase.end * TUNE_RATE));
    let n = 0;
    for (let i = first; i < last; i += 1) {
      const t = i / TUNE_RATE;
      while (n < phrase.notes.length - 1 && t >= phrase.notes[n].end) n += 1;
      const note = phrase.notes[n];
      const u = Math.min(1, Math.max(0, (t - note.start) / (note.end - note.start)));
      const hz = note.from * Math.pow(note.to / note.from, u);
      const into = t - phrase.start, left = phrase.end - t;
      const edge = Math.min(1, into / RAMP, left / RAMP);
      const gain = edge >= 1 ? 1 : 0.5 - 0.5 * Math.cos(Math.PI * Math.max(0, edge));
      const v = TUNE_LEVEL * gain * Math.sin(2 * Math.PI * phase);
      samples[2 * i] = v;
      samples[2 * i + 1] = v;
      phase += hz / TUNE_RATE;
      phase -= Math.floor(phase);
    }
  }
  return { samples, frames, channels: 2, rate: TUNE_RATE };
}
