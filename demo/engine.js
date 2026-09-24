/**
 * Flyback's CPU half, ported to JavaScript for the browser demo.
 *
 * **This is a hand port and nothing checks it but a reader.** Every function
 * below names the C++ it came from. `demo/tools/check_shaders.py` holds the
 * shaders to the plugin character for character; nothing holds this file to
 * `source/engine/` and `source/Controls.cpp`. When one of those changes, change
 * this too -- and remember that a wrong port shows up as a discharge that is
 * subtly the wrong shape, which nobody will notice.
 *
 * What is here, complete:
 *
 *   Rng.h        Pcg32 (in BigInt, so the 64-bit LCG is exact) and lowbias32
 *   Physics.*    every constant and closed form: supplies, Ayrton's arc and its
 *                extinction length, Kelvin's two-sphere image series, Peek, the
 *                corona conductance, the Tesla coil's bang, and the colours
 *                from the gases' spectral lines through the CIE observer
 *   Lattice.*    the Laplace solve: PCG with a multigrid V-cycle preconditioner
 *   Dbm.*        the breakdown model: cells, the Fenwick candidate tree, growth
 *                with quenched disorder, arcs
 *   Tree.*       Kirchhoff currents, lengths, compaction
 *   Engine.cpp   all five machines, their circuits and clocks, the shutter
 *                window, and the light shared out as I x length
 *   Controls.cpp every 0..1 conversion, Resolve() and Display()
 *   Presets.h    the five rows, and FlybackPlugin::Effective's override
 *
 * What is NOT the same, and why:
 *
 *   - **Floating point.** The plugin's lattice, node positions and currents are
 *     C++ `float`, and every float operation rounds. Here the arrays are
 *     Float32Array, so a value is rounded when it is STORED, but the arithmetic
 *     between stores is double. The model is the same; the bolt a given seed
 *     grows is not bit-for-bit the plugin's.
 *   - **No worker thread.** The plugin runs the engine a frame late on a worker
 *     (`Flyback.h`). This runs it on the page's main thread in the frame it is
 *     for -- the harness's `SetSynchronousForTest` mode.
 *   - **Paint keys** are strings of the same rounded values, not the plugin's
 *     64-bit hash. Only their equality is ever used, so this is the same test.
 *   - The harness's probes (LadderProbe's history, the event log, lastEmitted)
 *     are left out: the plugin never reads them.
 *
 * SI units throughout, as in the C++.
 */

//===========================================================================
// Rng.h
//===========================================================================
const M64 = (1n << 64n) - 1n;

export class Pcg32 {
  constructor(seed = 0x853c49e6748fea9bn, stream = 0xda3e39cb94b95bdbn) {
    this.state = 0n;
    this.inc = 0n;
    this.seed(seed, stream);
  }

  seed(seed, stream = 0xda3e39cb94b95bdbn) {
    this.state = 0n;
    this.inc = ((stream << 1n) | 1n) & M64;
    this.next();
    this.state = (this.state + BigInt.asUintN(64, seed)) & M64;
    this.next();
  }

  next() {
    const old = this.state;
    this.state = (old * 6364136223846793005n + this.inc) & M64;
    const xorshifted = Number(((old >> 18n) ^ old) >> 27n & 0xffffffffn) >>> 0;
    const rot = Number(old >> 59n);
    return ((xorshifted >>> rot) | (xorshifted << ((32 - rot) & 31))) >>> 0;
  }

  /** Uniform in [0, 1), 53 bits: 27 from the first draw, 26 from the second. */
  uniform() {
    const hi = this.next() >>> 5;
    const lo = this.next() >>> 6;
    return (hi * 67108864 + lo) * (1.0 / 9007199254740992.0);
  }
}

/** lowbias32 (Wellons). Mirrored in GLSL as `hash32` in the plugin. */
export function hash(x) {
  x >>>= 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

export function hash2(a, b) {
  return hash((a ^ hash((b + 0x9e3779b9) >>> 0)) >>> 0);
}

/** Top 24 bits into [0, 1): exact in float32. */
export function unit(h) {
  return (h >>> 8) * (1.0 / 16777216.0);
}

export function signed(h) {
  return Math.fround(2.0 * unit(h) - 1.0);
}

//===========================================================================
// Small helpers for C++ semantics.
//===========================================================================
const f32 = Math.fround;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
/** std::lround: half away from zero. */
export const lround = (x) => (x < 0 ? -Math.floor(-x + 0.5) : Math.floor(x + 0.5));
const hypot = (x, y) => Math.sqrt(x * x + y * y);
const kHuge = 1e30;

function segmentDistance(px, py, ax, ay, bx, by) {
  const dx = bx - ax, dy = by - ay;
  const l2 = dx * dx + dy * dy;
  let t = l2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0;
  t = clamp(t, 0.0, 1.0);
  return hypot(px - (ax + t * dx), py - (ay + t * dy));
}

//===========================================================================
// Physics.h / Physics.cpp
//===========================================================================
export const physics = {
  kPi: 3.14159265358979323846,
  kEpsilon0: 8.8541878128e-12,
  kAirBreakdown: 3.0e6,
  kStreamerField: 5.0e5,
  kVdgGroundRatio: 0.4,
  kPositiveIonMobility: 1.76e-4,
  kRoughestFinish: 0.82,
  kTeslaPrimaryFarads: 30e-9,
  kArcKelvin: 6500.0,
  kThermalAmps: 0.5,
};
const P = physics;

export const SupplyKind = { ZVS: 0, NST: 1, Flyback: 2, Count: 3 };
export const Gas = { Air: 0, Neon: 1, Argon: 2, NeonXenon: 3, Count: 4 };

export function peekSphere(radiusMetres) {
  const rcm = Math.max(radiusMetres * 100.0, 1e-3);
  return 27.2e5 * (1.0 + 0.54 / Math.sqrt(rcm));
}

export function nominalSupply(kind) {
  switch (kind) {
    case SupplyKind.ZVS: return { openVolts: 20e3, sourceOhms: 8e5 };
    case SupplyKind.NST: return { openVolts: 15e3, sourceOhms: 5e5 };
    case SupplyKind.Flyback: return { openVolts: 25e3, sourceOhms: 4e6 };
    default: return { openVolts: 15e3, sourceOhms: 5e5 };
  }
}

const shortCircuitAmps = (s) => s.openVolts / s.sourceOhms;
const maxPower = (s) => (s.openVolts * s.openVolts) / (4.0 * s.sourceOhms);

/** The low-current constants; Physics.h says which are sourced and which not. */
export const AYRTON = { A: 350.0, B: 1.0e3, C: 5.0, D: 750.0 };

export function arcCurrent(arc, supply, length) {
  const b = supply.openVolts - arc.A - arc.B * length;
  const c = arc.C + arc.D * length;
  const disc = b * b - 4.0 * supply.sourceOhms * c;
  if (b <= 0.0 || disc < 0.0) return 0.0;
  return (b + Math.sqrt(disc)) / (2.0 * supply.sourceOhms);
}

export function extinctionLength(arc, supply) {
  const v = supply.openVolts - arc.A;
  const alpha = arc.B * arc.B;
  const beta = 2.0 * arc.B * v + 4.0 * supply.sourceOhms * arc.D;
  const gamma = v * v - 4.0 * supply.sourceOhms * arc.C;
  if (v <= 0.0 || gamma <= 0.0) return 0.0;
  const disc = beta * beta - 4.0 * alpha * gamma;
  if (disc < 0.0) return 0.0;
  return (2.0 * gamma) / (beta + Math.sqrt(disc));
}

export const strikeGap = (supply) => supply.openVolts / P.kAirBreakdown;

export function solveTwoSpheres(a, b, gap) {
  const out = { capacitance: 0, fieldDriven: 0, fieldGrounded: 0, images: 0 };
  const k = 4.0 * P.kPi * P.kEpsilon0;
  const s = a + gap + b;
  const pa = a;
  const pb = s - b;
  const charges = [{ q: k * a, x: 0.0 }];
  let inDriven = k * a;
  let last = charges[0];
  for (let n = 0; n < 400; n += 1) {
    const r = s - last.x;
    const gb = { q: (-last.q * b) / r, x: s - (b * b) / r };
    charges.push(gb);
    const r1 = gb.x;
    const ga = { q: (-gb.q * a) / r1, x: (a * a) / r1 };
    charges.push(ga);
    inDriven += ga.q;
    last = ga;
    out.images += 2;
    if (Math.abs(ga.q) < 1e-14 * k * a) break;
  }
  let ea = 0.0, eb = 0.0;
  for (const c of charges) {
    const da = pa - c.x;
    const db = pb - c.x;
    ea += (c.q * da) / (Math.abs(da) * da * da);
    eb += (c.q * db) / (Math.abs(db) * db * db);
  }
  out.capacitance = inDriven;
  out.fieldDriven = Math.abs(ea) / k;
  out.fieldGrounded = Math.abs(eb) / k;
  return out;
}

export function vdgBreakdownVolts(spheres, a, b) {
  const va = peekSphere(a) / Math.max(spheres.fieldDriven, 1e-12);
  const vb = peekSphere(b) / Math.max(spheres.fieldGrounded, 1e-12);
  return Math.min(va, vb);
}

export const coronaConductance = (a, onsetVolts, b) =>
  (24.0 * P.kPi * P.kEpsilon0 * P.kPositiveIonMobility * a * onsetVolts) / (b * b);

export const toploadCapacitance = (radius) => 4.0 * P.kPi * P.kEpsilon0 * radius;
export const toploadVolts = (supply, radius) =>
  supply.openVolts * Math.sqrt(P.kTeslaPrimaryFarads / toploadCapacitance(radius));
export function bangJoules(supply, radius) {
  const v = toploadVolts(supply, radius);
  return 0.5 * toploadCapacitance(radius) * v * v;
}

// ---- colour -----------------------------------------------------------------
function lobe(l, mu, s1, s2) {
  const t = (l - mu) / (l < mu ? s1 : s2);
  return Math.exp(-0.5 * t * t);
}

function observer(l) {
  return [
    1.056 * lobe(l, 599.8, 37.9, 31.0) + 0.362 * lobe(l, 442.0, 16.0, 26.7) - 0.065 * lobe(l, 501.1, 20.4, 26.2),
    0.821 * lobe(l, 568.8, 46.9, 40.5) + 0.286 * lobe(l, 530.9, 16.3, 31.1),
    1.217 * lobe(l, 437.0, 11.8, 36.0) + 0.681 * lobe(l, 459.0, 26.0, 13.8),
  ];
}

function toWeights(X, Y, Z) {
  let r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
  let g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
  let b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
  const low = Math.min(r, g, b);
  if (low < 0.0) { r -= low; g -= low; b -= low; }
  const sum = r + g + b;
  if (sum <= 0.0) return { r: f32(1 / 3), g: f32(1 / 3), b: f32(1 / 3) };
  return { r: f32(r / sum), g: f32(g / sum), b: f32(b / sum) };
}

function fromLines(lines) {
  let X = 0, Y = 0, Z = 0;
  for (const [nm, weight] of lines) {
    const [x, y, z] = observer(nm);
    X += weight * x; Y += weight * y; Z += weight * z;
  }
  return toWeights(X, Y, Z);
}

// The line lists, from Physics.cpp, with its comments' provenance: air is the
// N2 second positive v'=0 progression (Nicholls 1961 Franck-Condon x nu^4) and
// N2+ 391.4 / 427.8 nm, the 391.4 ratio NOT sourced; neon, argon and xenon are
// NIST ASD strong lines with approximate weights.
const kAir = [[337.1, 1.000], [357.7, 0.503], [380.5, 0.124], [405.9, 0.0465], [434.4, 0.0179], [391.4, 0.200], [427.8, 0.053]];
const kNeon = [[585.2, 0.80], [588.2, 0.50], [594.5, 0.50], [603.0, 0.30], [607.4, 0.50], [609.6, 0.50],
  [614.3, 0.70], [616.4, 0.30], [621.7, 0.30], [626.6, 0.50], [633.4, 0.60], [638.3, 0.70],
  [640.2, 1.00], [650.6, 0.60], [659.9, 0.30], [667.8, 0.30], [692.9, 0.40], [703.2, 0.50]];
const kArgon = [[415.9, 0.20], [420.1, 0.20], [427.2, 0.10], [430.0, 0.10], [434.8, 0.30], [454.5, 0.20],
  [460.9, 0.30], [476.5, 0.40], [480.6, 0.40], [488.0, 0.60], [696.5, 0.40], [706.7, 0.30],
  [738.4, 0.30], [750.4, 0.60], [763.5, 0.80]];
const kXenon = [[450.1, 0.40], [452.5, 0.30], [462.4, 0.80], [467.1, 1.00], [473.4, 0.50],
  [480.7, 0.60], [482.9, 0.40], [484.4, 0.30], [491.7, 0.40]];

export function streamerColour(gas) {
  switch (gas) {
    case Gas.Neon: return fromLines(kNeon);
    case Gas.Argon: return fromLines(kArgon);
    case Gas.NeonXenon: {
      // Both spectra, xenon at twice the neon's line weight: a look, and the
      // plugin says so.
      const mix = [];
      for (const [nm, w] of kNeon) mix.push([nm, 0.5 * w]);
      for (const [nm, w] of kXenon) mix.push([nm, 1.0 * w]);
      return fromLines(mix);
    }
    default: return fromLines(kAir);
  }
}

export function blackbodyColour(kelvin) {
  const h = 6.62607015e-34, c = 2.99792458e8, kb = 1.380649e-23;
  let X = 0, Y = 0, Z = 0;
  for (let l = 380.0; l <= 780.0; l += 5.0) {
    const m = l * 1e-9;
    const bb = 1.0 / (Math.pow(m, 5.0) * (Math.exp((h * c) / (m * kb * kelvin)) - 1.0));
    const [x, y, z] = observer(l);
    X += bb * x; Y += bb * y; Z += bb * z;
  }
  return toWeights(X, Y, Z);
}

//===========================================================================
// Lattice.h / Lattice.cpp -- PCG with a multigrid V-cycle preconditioner.
//===========================================================================
const kCoarsestSweeps = 40;

export class Lattice {
  static goodSize(wanted, levelsWanted) {
    const step = 1 << levelsWanted;
    const a = Math.max(2, Math.trunc((Math.max(wanted, 3) - 1 + step - 1) / step));
    return a * step + 1;
  }

  constructor() {
    this.nx = 0;
    this.ny = 0;
    this.levels = [];
    this.masksDirty = true;
  }

  resize(nx, ny) {
    if (nx < 5 || ny < 5 || (nx - 1) % 4 !== 0 || (ny - 1) % 4 !== 0) return false;
    this.nx = nx;
    this.ny = ny;
    const n = nx * ny;
    this.phi = new Float32Array(n);
    this.held = new Uint8Array(n);
    this.r = new Float32Array(n);
    this.z = new Float32Array(n);
    this.p = new Float32Array(n);
    this.ap = new Float32Array(n);
    this.levels = [];
    let lx = nx, ly = ny;
    for (;;) {
      const m = lx * ly;
      const free = new Float32Array(m);
      free.fill(1);
      this.levels.push({
        nx: lx, ny: ly,
        u: new Float32Array(m), f: new Float32Array(m), r: new Float32Array(m),
        held: new Uint8Array(m), free,
      });
      if ((lx - 1) % 2 !== 0 || (ly - 1) % 2 !== 0 || Math.min(lx, ly) < 9) break;
      lx = (lx - 1) / 2 + 1;
      ly = (ly - 1) / 2 + 1;
    }
    this.rowScratch = new Float32Array(this.levels.length > 1 ? this.levels[1].nx : 1);
    for (let i = 0; i < nx; i += 1) {
      this.held[i] = 1;
      this.held[(ny - 1) * nx + i] = 1;
    }
    for (let j = 0; j < ny; j += 1) {
      this.held[j * nx] = 1;
      this.held[j * nx + nx - 1] = 1;
    }
    this.masksDirty = true;
    return true;
  }

  index(i, j) { return j * this.nx + i; }

  hold(index, value) {
    this.phi[index] = value;
    if (this.held[index]) return;
    this.held[index] = 1;
    if (!this.masksDirty) {
      this.levels[0].held[index] = 1;
      this.levels[0].free[index] = 0;
      this.markCoarse(1, index % this.nx, Math.trunc(index / this.nx));
    }
  }

  release(index) {
    const i = index % this.nx;
    const j = Math.trunc(index / this.nx);
    if (i === 0 || j === 0 || i === this.nx - 1 || j === this.ny - 1) return;
    if (this.held[index]) {
      this.held[index] = 0;
      this.masksDirty = true;
    }
  }

  guess(index, value) {
    if (!this.held[index]) this.phi[index] = value;
  }

  isHeld(index) { return this.held[index] !== 0; }
  potential(index) { return this.phi[index]; }

  markCoarse(level, i, j) {
    if (level >= this.levels.length) return;
    const c = this.levels[level];
    const x0 = i >> 1, x1 = (i + 1) >> 1;
    const y0 = j >> 1, y1 = (j + 1) >> 1;
    for (let y = y0; y <= y1; y += 1) {
      for (let x = x0; x <= x1; x += 1) {
        if (x < 0 || y < 0 || x >= c.nx || y >= c.ny) continue;
        const k = y * c.nx + x;
        if (c.held[k]) continue;
        c.held[k] = 1;
        c.free[k] = 0;
        this.markCoarse(level + 1, x, y);
      }
    }
  }

  buildMasks() {
    this.levels[0].held.set(this.held);
    for (let k = 1; k < this.levels.length; k += 1) {
      const f = this.levels[k - 1];
      const c = this.levels[k];
      c.held.fill(0);
      for (let y = 0; y < c.ny; y += 1) {
        for (let x = 0; x < c.nx; x += 1) {
          let any = x === 0 || y === 0 || x === c.nx - 1 || y === c.ny - 1;
          for (let dy = -1; dy <= 1 && !any; dy += 1) {
            for (let dx = -1; dx <= 1 && !any; dx += 1) {
              const fx = 2 * x + dx, fy = 2 * y + dy;
              if (fx >= 0 && fy >= 0 && fx < f.nx && fy < f.ny && f.held[fy * f.nx + fx]) any = true;
            }
          }
          c.held[y * c.nx + x] = any ? 1 : 0;
        }
      }
    }
    for (const l of this.levels) {
      for (let i = 0; i < l.held.length; i += 1) l.free[i] = l.held[i] ? 0 : 1;
    }
    this.masksDirty = false;
  }

  smooth(l, sweeps, redFirst) {
    const w = l.nx, u = l.u, f = l.f, m = l.free;
    for (let s = 0; s < sweeps; s += 1) {
      for (let pass = 0; pass < 2; pass += 1) {
        const colour = redFirst ? pass : 1 - pass;
        for (let y = 1; y < l.ny - 1; y += 1) {
          const x0 = 1 + ((y + colour) & 1);
          const row = y * w;
          for (let x = x0; x < w - 1; x += 2) {
            const k = row + x;
            u[k] += m[k] * (0.25 * (u[k - 1] + u[k + 1] + u[k - w] + u[k + w] - f[k]) - u[k]);
          }
        }
      }
    }
  }

  residualOf(l) {
    const w = l.nx, u = l.u, f = l.f, m = l.free, r = l.r;
    for (let y = 1; y < l.ny - 1; y += 1) {
      const row = y * w;
      for (let x = 1; x < w - 1; x += 1) {
        const k = row + x;
        r[k] = m[k] * (f[k] - (u[k - 1] + u[k + 1] + u[k - w] + u[k + w] - 4.0 * u[k]));
      }
    }
  }

  restrict(fine, coarse) {
    const fw = fine.nx, cw = coarse.nx, rr = fine.r;
    coarse.u.fill(0);
    coarse.f.fill(0);
    for (let y = 1; y < coarse.ny - 1; y += 1) {
      for (let x = 1; x < cw - 1; x += 1) {
        const c = 2 * y * fw + 2 * x;
        const v = f32(0.25 * rr[c]
          + 0.125 * (rr[c - 1] + rr[c + 1] + rr[c - fw] + rr[c + fw])
          + 0.0625 * (rr[c - fw - 1] + rr[c - fw + 1] + rr[c + fw - 1] + rr[c + fw + 1]));
        coarse.f[y * cw + x] = 4.0 * v;
      }
    }
  }

  prolongAdd(coarse, fine) {
    const fw = fine.nx, cw = coarse.nx, cu = coarse.u;
    if (this.rowScratch.length < cw) this.rowScratch = new Float32Array(cw);
    const a = this.rowScratch;
    for (let y = 1; y < fine.ny - 1; y += 1) {
      const c0 = (y >> 1) * cw;
      if (y & 1) {
        const c1 = c0 + cw;
        for (let X = 0; X < cw; X += 1) a[X] = 0.5 * (cu[c0 + X] + cu[c1 + X]);
      } else {
        for (let X = 0; X < cw; X += 1) a[X] = cu[c0 + X];
      }
      const row = y * fw, u = fine.u, m = fine.free;
      for (let x = 1; x < fw - 1; x += 1) {
        const X = x >> 1;
        const v = x & 1 ? f32(0.5 * (a[X] + a[X + 1])) : a[X];
        u[row + x] += m[row + x] * v;
      }
    }
  }

  vCycle(k) {
    const l = this.levels[k];
    if (k + 1 === this.levels.length) {
      this.smooth(l, kCoarsestSweeps / 2, true);
      this.smooth(l, kCoarsestSweeps / 2, false);
      return;
    }
    this.smooth(l, 2, true);
    this.residualOf(l);
    this.restrict(l, this.levels[k + 1]);
    this.vCycle(k + 1);
    this.prolongAdd(this.levels[k + 1], l);
    this.smooth(l, 2, false);
  }

  precondition(input, out) {
    const l = this.levels[0];
    l.u.fill(0);
    for (let i = 0; i < input.length; i += 1) l.f[i] = -input[i];
    this.vCycle(0);
    out.set(l.u);
  }

  applyA(x, y) {
    const nx = this.nx, m = this.levels[0].free;
    for (let j = 1; j < this.ny - 1; j += 1) {
      const row = j * nx;
      for (let i = 1; i < nx - 1; i += 1) {
        const k = row + i;
        y[k] = -m[k] * (x[k - 1] + x[k + 1] + x[k - nx] + x[k + nx] - 4.0 * x[k]);
      }
    }
  }

  solve(tolerance, maxIterations = 200) {
    if (this.levels.length === 0) return -1;
    if (this.masksDirty) this.buildMasks();
    const nx = this.nx, ny = this.ny, n = this.phi.length;
    const phi = this.phi, r = this.r, z = this.z, p = this.p, ap = this.ap;
    const m = this.levels[0].free;
    let worst = 0;
    r.fill(0);
    for (let j = 1; j < ny - 1; j += 1) {
      const row = j * nx;
      for (let i = 1; i < nx - 1; i += 1) {
        const k = row + i;
        r[k] = m[k] * (phi[k - 1] + phi[k + 1] + phi[k - nx] + phi[k + nx] - 4.0 * phi[k]);
        const a = Math.abs(r[k]);
        if (a > worst) worst = a;
      }
    }
    if (worst < tolerance) return 0;

    this.precondition(r, z);
    p.set(z);
    let rz = 0.0;
    for (let k = 0; k < n; k += 1) rz += r[k] * z[k];

    for (let it = 1; it <= maxIterations; it += 1) {
      this.applyA(p, ap);
      let pap = 0.0;
      for (let k = 0; k < n; k += 1) pap += p[k] * ap[k];
      if (pap <= 0.0) return -1;
      const alpha = f32(rz / pap);
      worst = 0;
      for (let k = 0; k < n; k += 1) {
        phi[k] += alpha * p[k];
        r[k] -= alpha * ap[k];
        const a = Math.abs(r[k]);
        if (a > worst) worst = a;
      }
      if (worst < tolerance) return it;
      this.precondition(r, z);
      let rzNext = 0.0;
      for (let k = 0; k < n; k += 1) rzNext += r[k] * z[k];
      const beta = f32(rzNext / rz);
      rz = rzNext;
      for (let k = 0; k < n; k += 1) p[k] = z[k] + beta * p[k];
    }
    return -1;
  }

  relax(ci, cj, radius, sweeps) {
    const nx = this.nx, phi = this.phi, held = this.held;
    const x0 = Math.max(1, ci - radius), x1 = Math.min(nx - 2, ci + radius);
    const y0 = Math.max(1, cj - radius), y1 = Math.min(this.ny - 2, cj + radius);
    for (let s = 0; s < sweeps; s += 1) {
      for (let j = y0; j <= y1; j += 1) {
        for (let i = x0; i <= x1; i += 1) {
          const k = j * nx + i;
          if (held[k]) continue;
          phi[k] = 0.25 * (phi[k - 1] + phi[k + 1] + phi[k - nx] + phi[k + nx]);
        }
      }
    }
  }
}

//===========================================================================
// Tree.h / Tree.cpp
//===========================================================================
export function makeNode() {
  return {
    cell: -1, parent: -1, x: 0, y: 0, ax: 0, ay: 0, born: 0, lastHot: 0, root: -1,
    weight: 0, current: 0, children: 0, grounded: false, gx: 0, gy: 0, alive: true,
  };
}

const kTipWeight = 1.0;

export class Tree {
  constructor() { this.nodes = []; }
  clear() { this.nodes = []; }
  empty() { return this.nodes.length === 0; }
  size() { return this.nodes.length; }

  add(node) {
    this.nodes.push(node);
    node.root = node.parent < 0 ? this.nodes.length - 1 : this.nodes[node.parent].root;
    return this.nodes.length - 1;
  }

  currents(total, groundWeight) {
    const nodes = this.nodes, n = nodes.length;
    for (const node of nodes) { node.children = 0; node.weight = 0.0; }
    for (let i = 0; i < n; i += 1) {
      if (nodes[i].alive && nodes[i].parent >= 0) nodes[nodes[i].parent].children += 1;
    }
    for (let k = n - 1; k >= 0; k -= 1) {
      const node = nodes[k];
      if (!node.alive) continue;
      if (node.children === 0) node.weight += node.grounded ? groundWeight : kTipWeight;
      else if (node.grounded) node.weight += groundWeight;
      if (node.parent >= 0) nodes[node.parent].weight += node.weight;
    }
    let rootWeight = 0.0;
    for (const node of nodes) if (node.alive && node.parent < 0) rootWeight += node.weight;
    const u = rootWeight > 0.0 ? total / rootWeight : 0.0;
    for (const node of nodes) node.current = node.alive ? f32(u * node.weight) : 0;
  }

  longestPath() {
    const nodes = this.nodes;
    const along = new Float64Array(nodes.length);
    let longest = 0.0;
    for (let i = 0; i < nodes.length; i += 1) {
      const node = nodes[i];
      if (!node.alive) continue;
      const px = node.parent >= 0 ? nodes[node.parent].x : node.ax;
      const py = node.parent >= 0 ? nodes[node.parent].y : node.ay;
      const base = node.parent >= 0 ? along[node.parent] : 0.0;
      along[i] = base + hypot(node.x - px, node.y - py);
      if (along[i] > longest) longest = along[i];
    }
    return longest;
  }

  compact() {
    const nodes = this.nodes;
    const remap = new Int32Array(nodes.length).fill(-1);
    const kept = [];
    for (let i = 0; i < nodes.length; i += 1) {
      const node = nodes[i];
      if (!node.alive) continue;
      if (node.parent >= 0) {
        const mapped = remap[node.parent];
        if (mapped < 0) continue;
        node.parent = mapped;
      }
      remap[i] = kept.length;
      kept.push(node);
    }
    for (let i = 0; i < kept.length; i += 1) {
      kept[i].root = kept[i].parent < 0 ? i : kept[kept[i].parent].root;
    }
    this.nodes = kept;
  }

  connected() {
    for (const node of this.nodes) if (node.alive && node.grounded) return true;
    return false;
  }
}

//===========================================================================
// Dbm.h / Dbm.cpp -- the dielectric breakdown model.
//===========================================================================
export const Cell = { Free: 0, Source: 1, Ground: 2, Channel: 3, Arc: 4, Held: 5, Wall: 6 };

const kDx = [1, -1, 0, 0, 1, -1, 1, -1];
const kDy = [0, 0, 1, -1, 1, 1, -1, -1];
const kBond = [1.0, 1.0, 1.0, 1.0, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951];

export function growthSettings() {
  return {
    eta: 2.0, refreshEvery: 32, relaxRadius: 8, relaxSweeps: 4, tolerance: f32(1e-5),
    stopAtGround: true, diagonal: true, maxConnections: 1 << 30, allowed: null,
    disorder: 0.5, disorderKey: 0, biasX: 0.0, biasY: 0.0,
  };
}

const lowbit = (k) => k & -k;

export class Field {
  constructor() {
    this.lattice = new Lattice();
    this.h = 1.0;
    this.x0 = 0.0;
    this.y0 = 0.0;
    this.kind = new Uint8Array(0);
    this.nodeAt = new Int32Array(0);
    this.slotOf = new Int32Array(0);
    this.cellOf = [];
    this.weight = [];
    this.fenwick = [0];
    this.freeSlots = [];
  }

  reset(widthMetres, heightMetres, cellsHigh, ringPotential) {
    cellsHigh = Math.max(cellsHigh, 16);
    this.h = heightMetres / cellsHigh;
    const ny = Lattice.goodSize(cellsHigh + 1, 4);
    const nx = Lattice.goodSize(Math.trunc(Math.ceil(widthMetres / this.h)) + 1, 4);
    if (!this.lattice.resize(nx, ny)) return false;
    this.x0 = -0.5 * this.h * (nx - 1);
    this.y0 = -0.5 * this.h * (ny - 1);
    const n = nx * ny;
    this.kind = new Uint8Array(n);
    this.nodeAt = new Int32Array(n).fill(-1);
    this.slotOf = new Int32Array(n).fill(-1);
    this.cellOf = [];
    this.weight = [];
    this.fenwick = [0];
    this.freeSlots = [];
    const L = this.lattice;
    for (let i = 0; i < nx; i += 1) {
      for (const j of [0, ny - 1]) {
        this.kind[L.index(i, j)] = Cell.Wall;
        L.hold(L.index(i, j), ringPotential);
      }
    }
    for (let j = 0; j < ny; j += 1) {
      for (const i of [0, nx - 1]) {
        this.kind[L.index(i, j)] = Cell.Wall;
        L.hold(L.index(i, j), ringPotential);
      }
    }
    this.guessAll(ringPotential);
    return true;
  }

  get nx() { return this.lattice.nx; }
  get ny() { return this.lattice.ny; }
  step() { return this.h; }
  X(i) { return this.x0 + this.h * i; }
  Y(j) { return this.y0 + this.h * j; }

  cellAt(x, y) {
    const i = lround((x - this.x0) / this.h);
    const j = lround((y - this.y0) / this.h);
    if (i < 1 || j < 1 || i > this.nx - 2 || j > this.ny - 2) return -1;
    return this.lattice.index(i, j);
  }

  paintable(index) {
    if (index < 0) return false;
    const k = this.kind[index];
    return k !== Cell.Channel && k !== Cell.Arc && k !== Cell.Wall;
  }

  paintSource(index) {
    if (!this.paintable(index)) return;
    this.kind[index] = Cell.Source;
    this.lattice.hold(index, 1.0);
  }

  paintGround(index, potential) {
    if (!this.paintable(index)) return;
    this.kind[index] = Cell.Ground;
    this.lattice.hold(index, potential);
  }

  paintHeld(index, potential) {
    if (!this.paintable(index)) return;
    this.kind[index] = Cell.Held;
    this.lattice.hold(index, potential);
  }

  clearConductors() {
    for (let k = 0; k < this.kind.length; k += 1) {
      if (this.kind[k] !== Cell.Wall) {
        this.kind[k] = Cell.Free;
        this.nodeAt[k] = -1;
        this.lattice.release(k);
      }
    }
  }

  adopt(tree) {
    this.nodeAt.fill(-1);
    for (let k = 0; k < this.kind.length; k += 1) {
      if (this.kind[k] === Cell.Channel || this.kind[k] === Cell.Arc) {
        this.kind[k] = Cell.Free;
        this.lattice.release(k);
      }
    }
    const nodes = tree.nodes;
    for (let n = 0; n < nodes.length; n += 1) {
      const node = nodes[n];
      if (!node.alive) continue;
      if (node.parent >= 0 && !nodes[node.parent].alive) { node.alive = false; continue; }
      const cell = node.cell;
      if (cell < 0 || this.kind[cell] !== Cell.Free) { node.alive = false; continue; }
      this.kind[cell] = Cell.Channel;
      this.nodeAt[cell] = n;
      this.lattice.hold(cell, 1.0);
    }
  }

  forget(node) {
    if (node.cell < 0 || (this.kind[node.cell] !== Cell.Channel && this.kind[node.cell] !== Cell.Arc)) return;
    this.kind[node.cell] = Cell.Free;
    this.nodeAt[node.cell] = -1;
    this.lattice.release(node.cell);
  }

  conduct(tree, tip, endPotential) {
    const nodes = tree.nodes;
    const path = [];
    for (let n = tip; n >= 0; n = nodes[n].parent) path.push(n);
    const along = new Float64Array(path.length);
    let total = 0.0;
    for (let k = path.length - 1; k >= 0; k -= 1) {
      const node = nodes[path[k]];
      const px = node.parent >= 0 ? nodes[node.parent].x : node.ax;
      const py = node.parent >= 0 ? nodes[node.parent].y : node.ay;
      total += Math.hypot(node.x - px, node.y - py);
      along[k] = total;
    }
    const end = nodes[tip];
    total += Math.hypot(end.gx - end.x, end.gy - end.y);
    for (let k = 0; k < path.length; k += 1) {
      const cell = nodes[path[k]].cell;
      if (cell < 0) continue;
      const f = total > 0.0 ? along[k] / total : 1.0;
      this.kind[cell] = Cell.Arc;
      this.lattice.hold(cell, f32(1.0 - (1.0 - endPotential) * f));
    }
  }

  guessAll(value) {
    for (let k = 0; k < this.kind.length; k += 1) {
      if (this.kind[k] === Cell.Free) this.lattice.guess(k, value);
    }
  }

  // ---- candidates ------------------------------------------------------------
  /** Returns the weight; the parent cell lands in this.parentCell. */
  weightOf(cell, settings) {
    const L = this.lattice, nx = L.nx, kind = this.kind;
    const i = cell % nx, j = Math.trunc(cell / nx);
    let parentCell = -1;
    let bestK = -1;
    const ways = settings.diagonal ? 8 : 4;
    for (let k = 0; k < ways; k += 1) {
      const nc = (j + kDy[k]) * nx + (i + kDx[k]);
      const c = kind[nc];
      if (c !== Cell.Channel && c !== Cell.Source) continue;
      if (bestK < 0 || kBond[k] < kBond[bestK]
        || (kBond[k] === kBond[bestK] && c === Cell.Channel && kind[parentCell] === Cell.Source)) {
        bestK = k;
        parentCell = nc;
      }
    }
    this.parentCell = parentCell;
    if (bestK < 0) return 0.0;

    const phi = clamp(L.phi[cell], 0.0, 1.0);
    const field = (1.0 - phi) / kBond[bestK];
    if (field <= 0.0) return 0.0;
    let w = settings.eta === 0.0 ? 1.0 : Math.pow(field, settings.eta);
    if (settings.disorder > 0.0) {
      const h1 = hash2(cell >>> 0, settings.disorderKey);
      const h2 = hash((h1 ^ 0x68e31da4) >>> 0);
      const u1 = ((h1 >>> 8) + 0.5) * (1.0 / 16777216.0);
      const u2 = (h2 >>> 8) * (1.0 / 16777216.0);
      const xi = Math.sqrt(-2.0 * Math.log(u1)) * Math.cos(6.283185307179586 * u2);
      w *= Math.exp(settings.disorder * xi);
    }
    if (settings.biasX !== 0.0 || settings.biasY !== 0.0) {
      w *= Math.exp(settings.biasX * kDx[bestK] + settings.biasY * kDy[bestK]);
    }
    return w;
  }

  setWeight(slot, w) {
    const delta = w - this.weight[slot];
    this.weight[slot] = w;
    const fen = this.fenwick;
    for (let k = slot + 1; k < fen.length; k += lowbit(k)) fen[k] += delta;
  }

  pick(u) {
    const fen = this.fenwick;
    let pos = 0;
    let step = 1;
    while (step * 2 < fen.length) step *= 2;
    for (; step > 0; step = Math.trunc(step / 2)) {
      const next = pos + step;
      if (next < fen.length && fen[next] <= u) {
        pos = next;
        u -= fen[next];
      }
    }
    const n = this.weight.length;
    let slot = Math.min(pos, n - 1);
    for (let tries = 0; tries < n; tries += 1) {
      if (this.cellOf[slot] >= 0 && this.weight[slot] > 0.0) return slot;
      slot = slot === 0 ? n - 1 : slot - 1;
    }
    return -1;
  }

  addCandidate(cell) {
    if (this.slotOf[cell] >= 0) return;
    let slot;
    if (this.freeSlots.length > 0) {
      slot = this.freeSlots.pop();
    } else {
      slot = this.cellOf.length;
      this.cellOf.push(-1);
      this.weight.push(0.0);
      if (this.fenwick.length < this.cellOf.length + 1) {
        let capacity = 1;
        while (capacity < this.cellOf.length + 1) capacity *= 2;
        this.fenwick = new Array(capacity + 1).fill(0.0);
        while (this.weight.length < capacity) this.weight.push(0.0);
        while (this.cellOf.length < capacity) this.cellOf.push(-1);
        for (let s = capacity - 1; s >= slot + 1; s -= 1) this.freeSlots.push(s);
        const fen = this.fenwick;
        for (let s = 0; s < capacity; s += 1) {
          if (this.weight[s] !== 0.0) {
            for (let k = s + 1; k < fen.length; k += lowbit(k)) fen[k] += this.weight[s];
          }
        }
      }
    }
    this.cellOf[slot] = cell;
    this.slotOf[cell] = slot;
  }

  removeCandidate(cell) {
    const slot = this.slotOf[cell];
    if (slot < 0) return;
    this.setWeight(slot, 0.0);
    this.cellOf[slot] = -1;
    this.slotOf[cell] = -1;
    this.freeSlots.push(slot);
  }

  rebuild(settings) {
    for (const cell of this.cellOf) if (cell >= 0) this.slotOf[cell] = -1;
    this.cellOf = [];
    this.weight = [];
    this.fenwick = [0.0];
    this.freeSlots = [];

    const L = this.lattice, nx = L.nx, ny = L.ny, kind = this.kind;
    const ways = settings.diagonal ? 8 : 4;
    const allowed = settings.allowed;
    for (let j = 1; j < ny - 1; j += 1) {
      for (let i = 1; i < nx - 1; i += 1) {
        const cell = j * nx + i;
        if (kind[cell] !== Cell.Free) continue;
        if (allowed !== null && !allowed[cell]) continue;
        for (let k = 0; k < ways; k += 1) {
          const c = kind[(j + kDy[k]) * nx + (i + kDx[k])];
          if (c === Cell.Channel || c === Cell.Source) {
            this.addCandidate(cell);
            break;
          }
        }
      }
    }

    const fen = this.fenwick;
    fen.fill(0.0);
    for (let s = 0; s < this.cellOf.length; s += 1) {
      this.weight[s] = this.cellOf[s] >= 0 ? this.weightOf(this.cellOf[s], settings) : 0.0;
    }
    for (let s = 0; s < this.weight.length && s + 1 < fen.length; s += 1) {
      fen[s + 1] += this.weight[s];
      const up = s + 1 + lowbit(s + 1);
      if (up < fen.length) fen[up] += fen[s + 1];
    }
  }

  touchesGround(cell) {
    const L = this.lattice, nx = L.nx;
    const i = cell % nx, j = Math.trunc(cell / nx);
    for (let k = 0; k < 8; k += 1) {
      const n = (j + kDy[k]) * nx + (i + kDx[k]);
      if (this.kind[n] === Cell.Ground || (this.kind[n] === Cell.Wall && L.phi[n] <= f32(0.25))) return n;
    }
    return -1;
  }

  grow(tree, maxSites, lengthBudget, rng, now, settings) {
    const result = { grown: 0, length: 0.0, reachedGround: false, groundNode: -1, connections: 0 };
    if (maxSites <= 0 || lengthBudget <= 0.0) return result;

    const L = this.lattice;
    L.solve(settings.tolerance);
    this.rebuild(settings);

    const nx = L.nx;
    let sinceSolve = 0;
    const reach = settings.relaxRadius;
    const ways = settings.diagonal ? 8 : 4;
    const allowed = settings.allowed;

    while (result.grown < maxSites && result.length < lengthBudget) {
      let total = 0.0;
      const fen = this.fenwick;
      if (fen.length > 1) {
        for (let k = fen.length - 1; k > 0; k -= lowbit(k)) total += fen[k];
      }
      if (!(total > 0.0)) break;

      const slot = this.pick(rng.uniform() * total);
      if (slot < 0) break;
      const cell = this.cellOf[slot];

      this.weightOf(cell, settings);
      const parentCell = this.parentCell;
      if (parentCell < 0) {
        this.removeCandidate(cell);
        continue;
      }

      const i = cell % nx, j = Math.trunc(cell / nx);
      const node = makeNode();
      node.cell = cell;
      node.x = f32(this.X(i));
      node.y = f32(this.Y(j));
      node.born = now;
      node.lastHot = now;
      if (this.kind[parentCell] === Cell.Channel) {
        node.parent = this.nodeAt[parentCell];
      } else {
        node.parent = -1;
        node.ax = f32(this.X(parentCell % nx));
        node.ay = f32(this.Y(Math.trunc(parentCell / nx)));
      }
      const px = node.parent >= 0 ? tree.nodes[node.parent].x : node.ax;
      const py = node.parent >= 0 ? tree.nodes[node.parent].y : node.ay;

      const groundCell = this.touchesGround(cell);
      if (groundCell >= 0) {
        node.grounded = true;
        node.gx = f32(this.X(groundCell % nx));
        node.gy = f32(this.Y(Math.trunc(groundCell / nx)));
      }

      const index = tree.add(node);
      this.kind[cell] = Cell.Channel;
      this.nodeAt[cell] = index;
      L.hold(cell, 1.0);
      this.removeCandidate(cell);

      result.grown += 1;
      result.length += Math.hypot(node.x - px, node.y - py);

      if (node.grounded) {
        result.reachedGround = true;
        result.groundNode = index;
        result.connections += 1;
        if (settings.stopAtGround || result.connections >= settings.maxConnections) break;
        this.conduct(tree, index, L.phi[groundCell]);
        this.rebuild(settings);
        continue;
      }

      L.relax(i, j, reach, settings.relaxSweeps);
      for (let k = 0; k < ways; k += 1) {
        const n = (j + kDy[k]) * nx + (i + kDx[k]);
        if (this.kind[n] === Cell.Free && (allowed === null || allowed[n])) this.addCandidate(n);
      }

      sinceSolve += 1;
      if (sinceSolve >= settings.refreshEvery) {
        L.solve(settings.tolerance);
        sinceSolve = 0;
        this.rebuild(settings);
        continue;
      }

      const x0w = Math.max(1, i - reach - 1), x1w = Math.min(nx - 2, i + reach + 1);
      const y0w = Math.max(1, j - reach - 1), y1w = Math.min(L.ny - 2, j + reach + 1);
      for (let y = y0w; y <= y1w; y += 1) {
        for (let x = x0w; x <= x1w; x += 1) {
          const c = y * nx + x;
          const s = this.slotOf[c];
          if (s < 0) continue;
          this.setWeight(s, this.weightOf(c, settings));
        }
      }
    }
    return result;
  }
}

//===========================================================================
// Engine.h / Engine.cpp
//===========================================================================
export const Machine = { Ladder: 0, Tesla: 1, VanDeGraaff: 2, Globe: 3, Lichtenberg: 4, Count: 5 };

export function sceneHeight(machine) {
  switch (machine) {
    case Machine.Ladder: return 0.52;
    case Machine.Tesla: return 2.00;
    case Machine.VanDeGraaff: return 0.70;
    case Machine.Globe: return 0.50;
    case Machine.Lichtenberg: return 0.36;
    default: return 1.0;
  }
}

export const ShapeKind = { Capsule: 0, Disc: 1, Ring: 2, Box: 3 };
export const Material = { Dull: 0, Metal: 1, Copper: 2, Glass: 3, Floor: 4, Acrylic: 5 };

/** Luminous radius: 1 mm at 0.1 A, going as sqrt(I). */
function luminousRadius(amps) {
  return f32(1.0e-3 * Math.sqrt(Math.max(amps, 0.0) / 0.1));
}

/** A Frame: segments packed seven floats each, shapes as objects. */
export class Frame {
  constructor() {
    this.segments = [];
    this.shapes = [];
    this.joules = 0.0;
    this.events = 0;
    this.sceneHeight = 1.0;
  }

  get segmentCount() { return this.segments.length / 7; }

  pushSegment(x0, y0, x1, y1, joules, radius, thermal) {
    this.segments.push(x0, y0, x1, y1, joules, radius, thermal);
  }

  pushShape(kind, x0, y0, x1, y1, r, material) {
    this.shapes.push({ kind, x0, y0, x1, y1, r, material });
  }
}

/** Mix(): the paint key is compared for equality only; same rounding. */
const keyOf = (...values) => values.map((v) => lround(v * 1e6)).join(',');

class MachineBase {
  constructor() {
    this.field = new Field();
    this.tree = new Tree();
    this.rng = new Pcg32();
    this.s = null;
    this.configured = false;
    this.paintKey = null;
  }

  setup(next) {
    const seedChanged = !this.configured || next.seed !== this.s.seed;
    const latticeChanged = !this.configured || next.cellsHigh !== this.s.cellsHigh
      || Math.abs(next.aspect - this.s.aspect) > 1e-6;
    this.s = next;
    if (seedChanged) {
      this.rng.seed(BigInt.asUintN(64, 0x9e3779b97f4a7c15n * BigInt(this.s.seed + 1)) + BigInt(this.s.machine));
    }
    this.configure();
    const key = this.paintKeyOf();
    if (latticeChanged || key !== this.paintKey) {
      this.repaint(latticeChanged);
      this.paintKey = key;
    }
    this.configured = true;
  }

  configure() {}
  ringPotential() { return 0.0; }
  fieldWidth() { return this.W(); }
  rebase() {}

  H() { return sceneHeight(this.s.machine); }
  W() { return this.H() * this.s.aspect; }

  frameToScene(fx, fy) {
    const s = this.s;
    const u = (fx - 0.5) * s.aspect - s.posX;
    const v = fy - 0.5 - s.posY;
    const c = Math.cos(-s.rotation), sn = Math.sin(-s.rotation);
    const k = this.H() / Math.max(s.scale, 1e-3);
    return [(c * u - sn * v) * k, (sn * u + c * v) * k];
  }

  sceneToFrame(x, y) {
    const s = this.s;
    const k = Math.max(s.scale, 1e-3) / this.H();
    const c = Math.cos(s.rotation), sn = Math.sin(s.rotation);
    const u = (c * x - sn * y) * k + s.posX;
    const v = (sn * x + c * y) * k + s.posY;
    return [u / s.aspect + 0.5, v + 0.5];
  }

  paintCell(cell, paint, potential) {
    if (paint === 'source') this.field.paintSource(cell);
    else if (paint === 'ground') this.field.paintGround(cell, potential);
    else this.field.paintHeld(cell, potential);
  }

  paintCapsule(x0, y0, x1, y1, r, paint, potential) {
    const reach = Math.max(r, 0.72 * this.field.step());
    this.forBox(Math.min(x0, x1) - reach, Math.min(y0, y1) - reach, Math.max(x0, x1) + reach, Math.max(y0, y1) + reach,
      (cell, x, y) => {
        if (segmentDistance(x, y, x0, y0, x1, y1) <= reach) this.paintCell(cell, paint, potential);
      });
  }

  paintDisc(cx, cy, r, paint, potential) {
    this.paintCapsule(cx, cy, cx, cy, r, paint, potential);
  }

  forBox(xa, ya, xb, yb, fn) {
    const F = this.field, h = F.step();
    const i0 = Math.max(1, Math.trunc(Math.floor((xa - F.X(0)) / h)));
    const i1 = Math.min(F.nx - 2, Math.trunc(Math.ceil((xb - F.X(0)) / h)));
    const j0 = Math.max(1, Math.trunc(Math.floor((ya - F.Y(0)) / h)));
    const j1 = Math.min(F.ny - 2, Math.trunc(Math.ceil((yb - F.Y(0)) / h)));
    for (let j = j0; j <= j1; j += 1) {
      for (let i = i0; i <= i1; i += 1) fn(j * F.nx + i, F.X(i), F.Y(j));
    }
  }

  paintClip() {
    const s = this.s;
    if (s.clip === null || s.clipW <= 0 || s.clipH <= 0) return;
    const mask = s.clip, F = this.field;
    for (let j = 1; j < F.ny - 1; j += 1) {
      for (let i = 1; i < F.nx - 1; i += 1) {
        const cell = j * F.nx + i;
        if (F.kind[cell] !== Cell.Free) continue;
        const [fx, fy] = this.sceneToFrame(F.X(i), F.Y(j));
        if (fx < 0.0 || fy < 0.0 || fx >= 1.0 || fy >= 1.0) continue;
        const mx = Math.min(s.clipW - 1, Math.trunc(fx * s.clipW));
        const my = Math.min(s.clipH - 1, Math.trunc(fy * s.clipH));
        if (mask[my * s.clipW + mx]) F.paintGround(cell, 0.0);
      }
    }
  }

  repaint(resetLattice) {
    if (resetLattice) {
      this.field.reset(Math.min(this.W(), this.fieldWidth()), this.H(), this.s.cellsHigh, this.ringPotential());
      this.tree.clear();
    } else {
      this.field.clearConductors();
    }
    this.paintApparatus();
    this.paintClip();
    this.field.adopt(this.tree);
    this.tree.compact();
    this.field.adopt(this.tree);
  }

  clipKey() {
    return this.s.clip !== null ? `c${this.s.clipStamp}` : '';
  }

  /** Engine.cpp's Emit: Kirchhoff, then I x length, jittered and subdivided. */
  emit(from, amps, groundWeight, light, out) {
    from.currents(amps, groundWeight);
    const nodes = from.nodes;
    if (nodes.length === 0 || light <= 0.0) return;

    const h = this.field.step();
    const k = Math.imul(this.s.seed, 2654435761 | 0) >>> 0;
    const jittered = (n) => {
      const c = n.cell >>> 0;
      return [n.x + 0.3 * h * signed(hash2(c, k)), n.y + 0.3 * h * signed(hash2(c, (k + 1) >>> 0))];
    };

    const pieces = [];
    let norm = 0.0;
    for (const n of nodes) {
      if (!n.alive || n.current <= 0.0) continue;
      const [x1, y1] = jittered(n);
      let x0, y0;
      if (n.parent >= 0) [x0, y0] = jittered(nodes[n.parent]);
      else { x0 = n.ax; y0 = n.ay; }
      pieces.push({ x0, y0, x1, y1, amps: n.current, key: n.cell >>> 0 });
      norm += n.current * hypot(x1 - x0, y1 - y0);
      if (n.grounded) {
        pieces.push({ x0: x1, y0: y1, x1: n.gx, y1: n.gy, amps: n.current, key: ((n.cell >>> 0) ^ 0x55555555) >>> 0 });
        norm += n.current * hypot(n.gx - x1, n.gy - y1);
      }
    }
    if (norm <= 0.0) return;

    const px = new Float64Array(5), py = new Float64Array(5);
    for (const p of pieces) {
      px[0] = p.x0; py[0] = p.y0; px[4] = p.x1; py[4] = p.y1;
      const L = hypot(p.x1 - p.x0, p.y1 - p.y0);
      const nx = L > 0.0 ? -(p.y1 - p.y0) / L : 0.0;
      const ny = L > 0.0 ? (p.x1 - p.x0) / L : 0.0;
      const a = 0.16 * L * signed(hash2(p.key, (k + 7) >>> 0));
      px[2] = 0.5 * (px[0] + px[4]) + nx * a;
      py[2] = 0.5 * (py[0] + py[4]) + ny * a;
      for (let q = 0; q < 2; q += 1) {
        const lo = q * 2, hi = lo + 2;
        const b = 0.08 * L * signed(hash2(p.key, (k + 11 + q) >>> 0));
        px[lo + 1] = 0.5 * (px[lo] + px[hi]) + nx * b;
        py[lo + 1] = 0.5 * (py[lo] + py[hi]) + ny * b;
      }
      let sub = 0.0;
      for (let q = 0; q < 4; q += 1) sub += hypot(px[q + 1] - px[q], py[q + 1] - py[q]);
      const share = (light * p.amps * L) / norm;
      const radius = luminousRadius(p.amps);
      const thermal = f32(p.amps / (p.amps + P.kThermalAmps));
      for (let q = 0; q < 4; q += 1) {
        const piece = hypot(px[q + 1] - px[q], py[q + 1] - py[q]);
        const j = sub > 0.0 ? (share * piece) / sub : share * 0.25;
        out.pushSegment(px[q], py[q], px[q + 1], py[q + 1], j, radius, thermal);
      }
    }
    out.joules += light;
  }

  static exposed(a, b, exposeFrom) {
    return Math.max(0.0, b - Math.max(a, exposeFrom));
  }

  growth(eta, stopAtGround) {
    const g = growthSettings();
    g.eta = eta;
    g.stopAtGround = stopAtGround;
    g.disorderKey = this.rng.next();
    return g;
  }
}

// ---- Tesla coil ---------------------------------------------------------------
class TeslaMachine extends MachineBase {
  static kToroidY = 0.05;
  static kSecondaryR = 0.065;
  static kBaseY = -0.62;
  static kFloorY = -0.72;
  static kTargetR = 0.04;
  static kBaseHalf = 0.20;
  static kMinorRatio = 0.32;
  static kBangSeconds = 100e-6;
  static kReach = 2.50;
  static kGroundWeight = 20.0;

  constructor() {
    super();
    this.anchor = 0.0;
    this.bangIndex = 0;
    this.bangPeriod = -1.0;
    this.lastBang = -kHuge;
    this.pending = [];
  }

  minor() { return TeslaMachine.kMinorRatio * this.s.topload; }

  paintKeyOf() {
    const s = this.s;
    const parts = [1, s.topload, s.target];
    if (s.target === 2) parts.push(s.targetX, s.targetY);
    parts.push(s.posX, s.posY, s.scale, s.rotation);
    return keyOf(...parts) + this.clipKey();
  }

  targetScene() { return this.frameToScene(this.s.targetX, this.s.targetY); }

  paintApparatus() {
    const T = TeslaMachine, s = this.s;
    this.paintCapsule(-s.topload, T.kToroidY, s.topload, T.kToroidY, this.minor(), 'source', 1.0);
    const top = T.kToroidY - this.minor();
    this.forBox(-T.kSecondaryR, T.kBaseY, T.kSecondaryR, top, (cell, x, y) => {
      const u = clamp((y - T.kBaseY) / (top - T.kBaseY), 0.0, 1.0);
      this.paintCell(cell, 'held', f32(0.97 * Math.sin(0.5 * P.kPi * u)));
    });
    this.forBox(-T.kBaseHalf, T.kBaseY - 0.06, T.kBaseHalf, T.kBaseY, (cell) => this.paintCell(cell, 'ground', 0.0));
    if (s.target === 1) {
      this.forBox(-kHuge, -kHuge, kHuge, T.kFloorY, (cell) => this.paintCell(cell, 'ground', 0.0));
    } else if (s.target === 2) {
      const [tx, ty] = this.targetScene();
      this.paintDisc(tx, ty, T.kTargetR, 'ground', 0.0);
    }
  }

  configure() {
    const period = this.s.bps > 0.0 ? 1.0 / this.s.bps : 0.0;
    if (period !== this.bangPeriod) {
      this.anchor = this.lastBang > -kHuge ? this.lastBang : this.anchor;
      this.bangIndex = 0;
      this.bangPeriod = period;
    }
  }

  topVolts() { return toploadVolts(this.s.supply, this.s.topload); }
  bangEnergy() { return bangJoules(this.s.supply, this.s.topload); }
  bangAmps() { return (toploadCapacitance(this.s.topload) * this.topVolts()) / TeslaMachine.kBangSeconds; }
  maxLength() { return this.topVolts() / P.kStreamerField; }

  restart(t) {
    this.tree.clear();
    this.field.adopt(this.tree);
    this.anchor = t;
    this.bangIndex = 0;
    this.lastBang = -kHuge;
    this.pending = [];
  }

  fire(t) { this.pending.push(t); }

  rebase(t) {
    this.anchor = t;
    this.bangIndex = 0;
    this.pending = [];
  }

  step(t0, t1, exposeFrom, out) {
    const s = this.s;
    if (this.bangPeriod > 0.0 && this.anchor + (this.bangIndex + 1) / s.bps < t0 - 1e-9) this.rebase(t0);
    for (;;) {
      let next = kHuge;
      let clock = false;
      if (this.bangPeriod > 0.0) {
        next = this.anchor + (this.bangIndex + 1) / s.bps;
        clock = true;
      }
      let fired = kHuge;
      if (this.pending.length > 0) fired = Math.min(...this.pending);
      const t = Math.min(next, fired);
      if (t > t1) break;
      if (fired <= next) {
        this.pending.splice(this.pending.indexOf(fired), 1);
        clock = false;
      }
      if (clock) this.bangIndex += 1;
      this.bang(t, t > exposeFrom, out);
    }
  }

  bang(t, exposed, out) {
    const T = TeslaMachine, s = this.s;
    const dt = t - this.lastBang;
    if (!this.tree.empty()) {
      const nodes = this.tree.nodes;
      const rootAmps = new Float32Array(nodes.length);
      for (let i = 0; i < nodes.length; i += 1) if (nodes[i].parent < 0) rootAmps[i] = nodes[i].current;
      for (const n of nodes) {
        const root = n.root >= 0 ? rootAmps[n.root] : 0.0;
        const f = root > 0.0 ? n.current / root : 0.0;
        if (!(dt < s.memory * f)) {
          n.alive = false;
          this.field.forget(n);
        }
      }
      for (const n of nodes) {
        if (n.alive && n.parent >= 0 && !nodes[n.parent].alive) {
          n.alive = false;
          this.field.forget(n);
        }
      }
      this.tree.compact();
      this.field.adopt(this.tree);
    }

    const longest = this.tree.longestPath();
    const budget = Math.min(T.kReach * s.reach, Math.max(0.0, this.maxLength() - longest));
    if (budget > 0.25 * this.field.step() && !this.tree.connected()) {
      this.field.grow(this.tree, 100000, budget, this.rng, t, this.growth(s.eta, true));
    }
    for (const n of this.tree.nodes) n.lastHot = t;

    const joules = this.bangEnergy();
    this.emit(this.tree, this.bangAmps(), T.kGroundWeight, joules * s.efficiency, exposed ? out : new Frame());
    if (exposed) out.events += 1;
    this.lastBang = t;

    if (this.tree.connected()) for (const n of this.tree.nodes) n.grounded = false;
  }

  shapes(out) {
    const T = TeslaMachine, s = this.s;
    const r = f32(s.topload), m = f32(this.minor());
    const top = f32(T.kToroidY - this.minor());
    if (s.target === 1) out.pushShape(ShapeKind.Box, -50, -50, 50, T.kFloorY, 0, Material.Floor);
    out.pushShape(ShapeKind.Box, -T.kBaseHalf, T.kBaseY - 0.06, T.kBaseHalf, T.kBaseY, 0, Material.Dull);
    out.pushShape(ShapeKind.Box, -T.kSecondaryR, T.kBaseY, T.kSecondaryR, top, 0, Material.Copper);
    out.pushShape(ShapeKind.Capsule, -r, T.kToroidY, r, T.kToroidY, m, Material.Metal);
    if (s.target === 2) {
      const [tx, ty] = this.targetScene();
      out.pushShape(ShapeKind.Capsule, tx, ty, tx, T.kFloorY, 0.008, Material.Metal);
      out.pushShape(ShapeKind.Disc, tx, ty, 0, 0, T.kTargetR, Material.Metal);
    }
  }
}

// ---- Jacob's ladder -------------------------------------------------------------
class LadderMachine extends MachineBase {
  static kBottomY = -0.19;
  static kBottomGap = 0.003;
  static kRodRadius = 0.003;
  static kSubstep = 0.001;
  static kGroundWeight = 50.0;
  static kLayer = 0.005;

  constructor() {
    super();
    this.lit = false;
    this.extinction = 0.0;
    this.strikeGapM = 0.0;
    this.rootSpeed = 0.9;
    this.column = [];
    this.allowed = null;
    this.fireAt = -kHuge;
    this.paintCounter = 0;
  }

  half() { return 0.5 * this.s.rodSpread; }
  rodA() {
    const L = LadderMachine;
    const x0 = -0.5 * L.kBottomGap, y0 = L.kBottomY;
    return [x0, y0, x0 - this.s.rodLength * Math.sin(this.half()), y0 + this.s.rodLength * Math.cos(this.half())];
  }
  rodB() {
    const L = LadderMachine;
    const x0 = 0.5 * L.kBottomGap, y0 = L.kBottomY;
    return [x0, y0, x0 + this.s.rodLength * Math.sin(this.half()), y0 + this.s.rodLength * Math.cos(this.half())];
  }
  onRod(a, y) {
    const [x0, y0, x1, y1] = a ? this.rodA() : this.rodB();
    const t = clamp((y - y0) / (y1 - y0), 0.0, 1.0);
    return { x: x0 + t * (x1 - x0), y: y0 + t * (y1 - y0) };
  }
  project(a, px, py) {
    const [x0, y0, x1, y1] = a ? this.rodA() : this.rodB();
    const dx = x1 - x0, dy = y1 - y0;
    const t = ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy);
    const c = clamp(t, 0.0, 1.0);
    return [x0 + c * dx, y0 + c * dy];
  }

  ringPotential() { return f32(0.5); }
  fieldWidth() { return this.H(); }
  paintKeyOf() { this.paintCounter += 1; return `L${this.paintCounter}`; }

  paintApparatus() {
    const L = LadderMachine;
    const [ax0, ay0, ax1, ay1] = this.rodA();
    const [bx0, by0, bx1, by1] = this.rodB();
    this.paintCapsule(ax0, ay0, ax1, ay1, L.kRodRadius, 'held', 1.0);
    if (this.column.length > 0) {
      const fx = this.column[0].x, fy = this.column[0].y, h = this.field.step();
      this.forBox(fx - 2 * h, fy - 2 * h, fx + 2 * h, fy + 2 * h, (cell, x, y) => {
        if (this.field.kind[cell] === Cell.Held && hypot(x - fx, y - fy) <= 1.6 * h) this.field.paintSource(cell);
      });
    }
    this.paintCapsule(bx0, by0, bx1, by1, L.kRodRadius, 'ground', 0.0);
  }

  configure() {
    this.extinction = extinctionLength(AYRTON, this.s.supply);
    this.strikeGapM = strikeGap(this.s.supply);
  }

  restart() {
    this.column = [];
    this.tree.clear();
    this.lit = false;
  }

  fire(t) { this.fireAt = t; }

  length() {
    let l = 0.0;
    const c = this.column;
    for (let i = 1; i < c.length; i += 1) l += hypot(c[i].x - c[i - 1].x, c[i].y - c[i - 1].y);
    return l;
  }

  strike() {
    const L = LadderMachine;
    if (L.kBottomGap > this.strikeGapM) return false;
    const a = this.onRod(true, L.kBottomY);
    const b = this.onRod(false, L.kBottomY);
    this.column = [];
    const n = 6;
    for (let i = 0; i <= n; i += 1) this.column.push({ x: a.x + ((b.x - a.x) * i) / n, y: a.y + ((b.y - a.y) * i) / n });
    this.lit = true;
    return true;
  }

  extinguish() {
    this.lit = false;
    this.column = [];
  }

  advect(dt) {
    const L = LadderMachine, s = this.s, c = this.column;
    const n = c.length;
    if (n < 2) return;
    const k = clamp(this.rootSpeed, 0.0, 1.0);
    for (let i = 1; i + 1 < n; i += 1) {
      const p = c[i];
      let [qx, qy] = this.project(true, p.x, p.y);
      const da = hypot(p.x - qx, p.y - qy);
      [qx, qy] = this.project(false, p.x, p.y);
      const db = hypot(p.x - qx, p.y - qy);
      const f = 1.0 - (1.0 - k) * Math.exp(-Math.min(da, db) / L.kLayer);
      p.x += s.wind * f * dt;
      p.y += s.rise * f * dt;
    }
    c[0] = this.onRod(true, c[0].y + k * s.rise * dt);
    c[n - 1] = this.onRod(false, c[n - 1].y + k * s.rise * dt);

    const most = 0.25 * this.field.step();
    const fine = [];
    for (let i = 0; i < c.length; i += 1) {
      if (i > 0) {
        const a = c[i - 1], b = c[i];
        const l = hypot(b.x - a.x, b.y - a.y);
        const kk = Math.trunc(l / most);
        for (let q = 1; q <= kk; q += 1) {
          const t = q / (kk + 1);
          fine.push({ x: a.x + t * (b.x - a.x), y: a.y + t * (b.y - a.y) });
        }
      }
      fine.push(c[i]);
    }
    this.column = fine;
  }

  offTheTop() {
    const [, , , y1] = this.rodA();
    return this.column[0].y >= y1 - 1e-9;
  }

  regrow(t) {
    this.tree.clear();
    if (this.column.length < 2) return;
    this.repaint(false);
    const F = this.field, h = F.step();
    this.allowed = new Uint8Array(F.nx * F.ny);
    for (let i = 1; i < this.column.length; i += 1) {
      const a = this.column[i - 1], b = this.column[i];
      this.forBox(Math.min(a.x, b.x) - 2 * h, Math.min(a.y, b.y) - 2 * h, Math.max(a.x, b.x) + 2 * h,
        Math.max(a.y, b.y) + 2 * h, (cell, x, y) => {
          if (segmentDistance(x, y, a.x, a.y, b.x, b.y) <= 1.8 * h) this.allowed[cell] = 1;
        });
    }
    const g = this.growth(this.s.eta, true);
    g.allowed = this.allowed;
    const cap = Math.trunc((6.0 * this.length()) / h) + 16;
    const r = F.grow(this.tree, cap, kHuge, this.rng, t, g);
    if (!r.reachedGround) this.tree.clear();
  }

  emitArc(light, amps, out) {
    if (light <= 0.0) return;
    if (!this.tree.empty() && this.tree.connected()) {
      this.emit(this.tree, amps, LadderMachine.kGroundWeight, light, out);
      return;
    }
    const total = this.length();
    if (total <= 0.0) return;
    const c = this.column;
    for (let i = 1; i < c.length; i += 1) {
      const a = c[i - 1], b = c[i];
      const l = hypot(b.x - a.x, b.y - a.y);
      out.pushSegment(a.x, a.y, b.x, b.y, (light * l) / total, luminousRadius(amps), f32(amps / (amps + P.kThermalAmps)));
    }
    out.joules += light;
  }

  step(t0, t1, exposeFrom, out) {
    const L = LadderMachine, s = this.s;
    let light = 0.0;
    let amps = 0.0;
    if (!this.lit && this.column.length === 0) this.strike();

    const steps = Math.max(1, Math.trunc(Math.ceil((t1 - t0) / L.kSubstep - 1e-9)));
    const dt = (t1 - t0) / steps;
    for (let k = 0; k < steps; k += 1) {
      const a = t0 + k * dt, b = a + dt;
      if (this.fireAt > -kHuge && this.fireAt <= b) {
        if (this.lit) this.extinguish();
        this.fireAt = -kHuge;
        this.strike();
      }
      if (!this.lit) {
        if (!this.strike()) continue;
      }
      const before = this.length();
      this.advect(dt);
      const after = this.length();
      const lStar = this.extinction;

      const top = after < lStar && this.offTheTop();
      if (after >= lStar || top) {
        const f = top ? 1.0 : after > before ? clamp((lStar - before) / (after - before), 0.0, 1.0) : 1.0;
        const te = a + f * dt;
        const I = arcCurrent(AYRTON, s.supply, Math.min(before, lStar));
        const Pw = (s.supply.openVolts - I * s.supply.sourceOhms) * I;
        light += Pw * MachineBase.exposed(a, te, exposeFrom) * s.efficiency;
        amps = Math.max(amps, I);
        this.emitArc(light, I, out);
        light = 0.0;
        this.extinguish();
        this.strike();
        continue;
      }

      const I = arcCurrent(AYRTON, s.supply, after);
      const power = (s.supply.openVolts - I * s.supply.sourceOhms) * I;
      light += power * MachineBase.exposed(a, b, exposeFrom) * s.efficiency;
      amps = Math.max(amps, I);
    }

    if (this.lit && this.column.length > 0) {
      this.regrow(t1);
      this.emitArc(light, amps, out);
    } else {
      this.tree.clear();
    }
    if (light > 0.0) out.events += 1;
  }

  shapes(out) {
    const L = LadderMachine;
    const [ax0, ay0, ax1, ay1] = this.rodA();
    const [bx0, by0, bx1, by1] = this.rodB();
    const r = L.kRodRadius;
    out.pushShape(ShapeKind.Capsule, ax0, ay0, ax1, ay1, r, Material.Metal);
    out.pushShape(ShapeKind.Capsule, bx0, by0, bx1, by1, r, Material.Metal);
    out.pushShape(ShapeKind.Capsule, ax0 - 0.012, ay0, ax0 - 0.012, L.kBottomY - 0.06, r, Material.Metal);
    out.pushShape(ShapeKind.Capsule, ax0, ay0, ax0 - 0.012, ay0, r, Material.Metal);
    out.pushShape(ShapeKind.Capsule, bx0 + 0.012, by0, bx0 + 0.012, L.kBottomY - 0.06, r, Material.Metal);
    out.pushShape(ShapeKind.Capsule, bx0, by0, bx0 + 0.012, by0, r, Material.Metal);
    out.pushShape(ShapeKind.Box, -0.09, L.kBottomY - 0.10, 0.09, L.kBottomY - 0.06, 0, Material.Dull);
  }
}

// ---- Van de Graaff ---------------------------------------------------------------
class VdgMachine extends MachineBase {
  static kCentreY = 0.06;
  static kBaseY = -0.28;
  static kSparkSeconds = 1e-6;
  static kGroundWeight = 20.0;
  static kSubstep = 1e-5;

  constructor() {
    super();
    this.spheres = null;
    this.breakdown = 1.0;
    this.volts = 0.0;
    this.coronaAmps = 0.0;
    this.fireFlag = false;
    this.corona = new Tree();
    this.sparkAllowed = null;
  }

  groundDistance() { return VdgMachine.kCentreY - VdgMachine.kBaseY; }
  coronaOnset() { return clamp(this.s.finish, P.kRoughestFinish, 1.0) * this.breakdown; }
  conductance() { return coronaConductance(this.s.sphere, this.coronaOnset(), this.groundDistance()); }
  B() { return P.kVdgGroundRatio * this.s.sphere; }
  Ax() { return -(0.5 * this.s.gap + this.s.sphere); }
  Bx() { return 0.5 * this.s.gap + this.B(); }
  fieldWidth() { return 1.6 * this.H(); }

  paintKeyOf() { return keyOf(3, this.s.sphere, this.s.gap) + this.clipKey(); }

  paintApparatus() {
    const V = VdgMachine, s = this.s;
    this.paintDisc(this.Ax(), V.kCentreY, s.sphere, 'source', 1.0);
    this.paintDisc(this.Bx(), V.kCentreY, this.B(), 'ground', 0.0);
    const h = this.field.step();
    this.sparkAllowed = new Uint8Array(this.field.nx * this.field.ny).fill(1);
    this.forBox(this.Ax() - s.sphere - 3 * h, V.kCentreY - s.sphere - 3 * h, this.Ax() + s.sphere + 3 * h, V.kCentreY + s.sphere + 3 * h,
      (cell, x, y) => {
        const dx = x - this.Ax(), dy = y - V.kCentreY, r = hypot(dx, dy);
        if (r > s.sphere && r <= s.sphere + 2.5 * h && dx < r * Math.cos((50.0 * P.kPi) / 180.0)) this.sparkAllowed[cell] = 0;
      });
    this.paintCapsule(this.Bx(), V.kCentreY, this.Bx(), V.kBaseY, 0.010, 'ground', 0.0);
    this.forBox(-kHuge, -kHuge, kHuge, V.kBaseY, (cell) => this.paintCell(cell, 'ground', 0.0));
  }

  configure() {
    this.spheres = solveTwoSpheres(this.s.sphere, this.B(), this.s.gap);
    this.breakdown = vdgBreakdownVolts(this.spheres, this.s.sphere, this.B());
  }

  restart() {
    this.volts = 0.0;
    this.tree.clear();
    this.corona.clear();
    this.fireFlag = false;
  }

  fire() { this.fireFlag = true; }

  spark(t, v, exposed, out) {
    const V = VdgMachine, s = this.s;
    this.tree.clear();
    this.field.adopt(this.tree);
    const g = this.growth(s.eta, true);
    g.allowed = this.sparkAllowed;
    this.field.grow(this.tree, 4000, kHuge, this.rng, t, g);
    const joules = 0.5 * this.spheres.capacitance * v * v;
    this.tree.currents(1.0, V.kGroundWeight);
    if (exposed) {
      this.emit(this.tree, (this.spheres.capacitance * v) / V.kSparkSeconds, V.kGroundWeight, joules * s.efficiency, out);
      out.events += 1;
    }
    for (const n of this.tree.nodes) this.field.forget(n);
    this.tree.clear();
    this.volts = 0.0;
  }

  step(t0, t1, exposeFrom, out) {
    const V = VdgMachine, s = this.s;
    const C = this.spheres.capacitance;
    const Vc = this.coronaOnset();
    const G = this.conductance();
    const I = Math.max(s.belt, 0.0);
    let coronaJoules = 0.0;
    let t = t0;
    while (t < t1 - 1e-12) {
      const dt = Math.min(V.kSubstep, t1 - t);
      let v = this.volts + (I * dt) / C;
      if (v > Vc && Vc < this.breakdown && G > 0.0) v = (this.volts + (dt / C) * (I + G * Vc)) / (1.0 + (dt * G) / C);
      if (v >= this.breakdown && v > this.volts) {
        const f = (this.breakdown - this.volts) / (v - this.volts);
        const ts = t + f * dt;
        this.spark(ts, this.breakdown, ts > exposeFrom, out);
        this.volts = 0.0;
        t = ts;
        continue;
      }
      const ic = G * Math.max(0.0, v - Vc);
      coronaJoules += v * ic * MachineBase.exposed(t, t + dt, exposeFrom);
      this.volts = v;
      t += dt;
    }
    this.coronaAmps = G * Math.max(0.0, this.volts - Vc);
    if (this.fireFlag) {
      this.fireFlag = false;
      if (this.volts > 0.2 * this.breakdown) this.spark(t1, this.volts, true, out);
    }

    const sites = lround(28.0 * s.reach * Math.min(1.0, this.coronaAmps / Math.max(I, 1e-12)));
    this.corona.clear();
    if (sites > 0) {
      const g = this.growth(1.0, true);
      this.field.grow(this.corona, sites, kHuge, this.rng, t1, g);
      this.emit(this.corona, this.coronaAmps, V.kGroundWeight, coronaJoules * s.efficiency, out);
      for (const n of this.corona.nodes) this.field.forget(n);
    }
  }

  shapes(out) {
    const V = VdgMachine, s = this.s;
    const ax = this.Ax(), bx = this.Bx(), y = V.kCentreY;
    out.pushShape(ShapeKind.Box, -50, -50, 50, V.kBaseY - 0.05, 0, Material.Floor);
    out.pushShape(ShapeKind.Box, this.Ax() - 0.2, V.kBaseY - 0.05, this.Bx() + 0.1, V.kBaseY, 0, Material.Dull);
    out.pushShape(ShapeKind.Capsule, ax, y, ax, V.kBaseY, 0.30 * s.sphere, Material.Acrylic);
    out.pushShape(ShapeKind.Capsule, bx, y, bx, V.kBaseY, 0.008, Material.Metal);
    out.pushShape(ShapeKind.Disc, ax, y, 0, 0, s.sphere, Material.Metal);
    out.pushShape(ShapeKind.Disc, bx, y, 0, 0, this.B(), Material.Metal);
  }
}

// ---- Plasma globe ------------------------------------------------------------------
class GlobeMachine extends MachineBase {
  static kCentreY = 0.03;
  static kElectrode = 0.12;
  static kGlassPotential = f32(0.35);
  static kFingerRadius = 0.025;
  static kGroundWeight = 40.0;

  constructor() {
    super();
    this.surge = false;
  }

  fieldWidth() { return this.H(); }

  paintKeyOf() {
    const [fx, fy] = this.finger();
    return keyOf(4, this.s.globe, this.s.finger ? 1.0 : 0.0, fx, fy) + this.clipKey();
  }

  finger() {
    const s = this.s, G = GlobeMachine;
    let [fx, fy] = this.frameToScene(s.fingerX, s.fingerY);
    if (s.clip !== null && s.clipW > 0) {
      let sx = 0, sy = 0, n = 0;
      for (let j = 0; j < s.clipH; j += 1) {
        for (let i = 0; i < s.clipW; i += 1) {
          if (s.clip[j * s.clipW + i]) {
            sx += (i + 0.5) / s.clipW;
            sy += (j + 0.5) / s.clipH;
            n += 1;
          }
        }
      }
      if (n > 0) [fx, fy] = this.frameToScene(sx / n, sy / n);
    }
    const dx = fx, dy = fy - G.kCentreY;
    const d = Math.max(hypot(dx, dy), 1e-6);
    return [(dx / d) * s.globe, G.kCentreY + (dy / d) * s.globe];
  }

  paintApparatus() {
    const G = GlobeMachine, s = this.s;
    const h = this.field.step();
    const thick = Math.max(0.004, 1.6 * h);
    const [fgx, fgy] = this.finger();
    this.forBox(-s.globe - thick, G.kCentreY - s.globe - thick, s.globe + thick, G.kCentreY + s.globe + thick, (cell, x, y) => {
      const r = hypot(x, y - G.kCentreY);
      if (r < s.globe || r > s.globe + thick) return;
      const touched = s.finger && hypot(x - fgx, y - fgy) <= G.kFingerRadius + h;
      this.paintCell(cell, 'ground', touched ? 0.0 : G.kGlassPotential);
    });
    this.paintDisc(0.0, G.kCentreY, G.kElectrode * s.globe, 'source', 1.0);
    this.paintCapsule(0.0, G.kCentreY - G.kElectrode * s.globe, 0.0, G.kCentreY - s.globe, 0.006, 'held', 1.0);
  }

  filaments() {
    return clamp(lround(2.0 + this.s.supply.openVolts / 3500.0), 3, 14);
  }

  restart() {
    this.tree.clear();
    this.field.adopt(this.tree);
  }

  fire() { this.surge = true; }

  step(t0, t1, exposeFrom, out) {
    const G = GlobeMachine, s = this.s;
    let nodes = this.tree.nodes;
    if (this.surge) {
      this.surge = false;
      this.tree.clear();
    } else if (nodes.length > 0) {
      const keep = new Uint8Array(nodes.length);
      for (let i = 0; i < nodes.length; i += 1) {
        if (nodes[i].grounded) for (let n = i; n >= 0; n = nodes[n].parent) keep[n] = 1;
      }
      const f = 1.0 - Math.exp(-(t1 - t0) / Math.max(s.memory, 1e-4));
      for (let i = 0; i < nodes.length; i += 1) {
        if (!nodes[i].grounded) continue;
        const path = [];
        for (let n = i; n >= 0; n = nodes[n].parent) path.push(n);
        const cut = f * unit(this.rng.next());
        const drop = Math.max(1, Math.trunc(Math.ceil(cut * path.length)));
        for (let k = 0; k < drop && k < path.length; k += 1) keep[path[k]] = 0;
      }
      for (let i = 0; i < nodes.length; i += 1) {
        nodes[i].alive = keep[i] !== 0;
        nodes[i].grounded = false;
      }
      this.tree.compact();
    }
    this.field.adopt(this.tree);
    this.tree.compact();
    this.field.adopt(this.tree);

    const want = this.filaments();
    const cap = Math.trunc(160 * s.reach);
    for (let fil = 0; fil < want; fil += 1) {
      const before = this.tree.size();
      const g = this.growth(s.eta, true);
      const r = this.field.grow(this.tree, cap, kHuge, this.rng, t1, g);
      let grown = this.tree.nodes;
      if (!r.reachedGround) {
        for (let i = before; i < grown.length; i += 1) {
          grown[i].alive = false;
          this.field.forget(grown[i]);
        }
        this.tree.compact();
        break;
      }
      const onPath = new Uint8Array(grown.length);
      for (let n = r.groundNode; n >= 0; n = grown[n].parent) onPath[n] = 1;
      for (let i = before; i < grown.length; i += 1) {
        if (!onPath[i]) {
          grown[i].alive = false;
          this.field.forget(grown[i]);
        }
      }
      this.tree.compact();
      this.field.adopt(this.tree);
      grown = this.tree.nodes;
      for (let i = 0; i < grown.length; i += 1) {
        if (grown[i].grounded) this.field.conduct(this.tree, i, G.kGlassPotential);
      }
    }
    const power = maxPower(s.supply);
    const exposure = MachineBase.exposed(t0, t1, exposeFrom);
    this.emit(this.tree, power / s.supply.openVolts, G.kGroundWeight, power * exposure * s.efficiency, out);
    if (exposure > 0.0) out.events += 1;
    nodes = null;
  }

  shapes(out) {
    const G = GlobeMachine;
    const g = f32(this.s.globe);
    const y = G.kCentreY;
    out.pushShape(ShapeKind.Box, -0.55 * g, y - 1.40 * g, 0.55 * g, y - 0.94 * g, 0, Material.Dull);
    out.pushShape(ShapeKind.Capsule, 0.0, y - G.kElectrode * g, 0.0, y - g, 0.012, Material.Glass);
    out.pushShape(ShapeKind.Disc, 0.0, y, 0, 0, G.kElectrode * g, Material.Glass);
    out.pushShape(ShapeKind.Ring, 0.0, y, 0.004, 0, g, Material.Glass);
  }
}

// ---- Lichtenberg figure ---------------------------------------------------------------
class LichtenbergMachine extends MachineBase {
  static kHalf = 0.165;
  static kGrowSeconds = 3.0;
  static kSites = 1600;
  static kGroundWeight = 20.0;

  constructor() {
    super();
    this.started = 0.0;
    this.restartAt = -kHuge;
  }

  fieldWidth() { return this.H(); }
  paintKeyOf() { return keyOf(5, this.s.origin) + this.clipKey(); }

  paintApparatus() {
    const Lb = LichtenbergMachine, h = this.field.step();
    if (this.s.origin === 0) {
      this.paintDisc(0.0, 0.0, 1.5 * h, 'source', 1.0);
      this.forBox(-kHuge, -kHuge, kHuge, kHuge, (cell, x, y) => {
        if (hypot(x, y) >= Lb.kHalf) this.paintCell(cell, 'ground', 0.0);
      });
    } else {
      this.forBox(-kHuge, -kHuge, kHuge, -Lb.kHalf, (cell, x) => {
        if (Math.abs(x) <= Lb.kHalf) this.paintCell(cell, 'source', 1.0);
      });
      this.forBox(-kHuge, Lb.kHalf, kHuge, kHuge, (cell) => this.paintCell(cell, 'ground', 0.0));
      this.forBox(-kHuge, -Lb.kHalf, kHuge, Lb.kHalf, (cell, x, y) => {
        if (Math.abs(x) > Lb.kHalf) this.paintCell(cell, 'held', f32(0.5 - (0.5 * y) / Lb.kHalf));
      });
    }
  }

  budget() { return lround(LichtenbergMachine.kSites * this.s.reach); }

  restart(t) {
    this.tree.clear();
    this.field.adopt(this.tree);
    this.started = t;
  }

  fire(t) { this.restartAt = t; }

  step(t0, t1, exposeFrom, out) {
    const Lb = LichtenbergMachine, s = this.s;
    if (this.restartAt > -kHuge) {
      this.restart(this.restartAt);
      this.restartAt = -kHuge;
    }
    const want = Math.min(this.budget(), Math.trunc(Math.ceil((this.budget() * (t1 - this.started)) / Lb.kGrowSeconds)));
    const have = this.tree.size();
    if (want > have && !this.tree.connected()) {
      this.field.grow(this.tree, want - have, kHuge, this.rng, t1, this.growth(s.eta, true));
    }
    const power = maxPower(s.supply);
    const exposure = MachineBase.exposed(t0, t1, exposeFrom);
    this.emit(this.tree, shortCircuitAmps(s.supply), Lb.kGroundWeight, power * exposure * s.efficiency, out);
    if (exposure > 0.0 && !this.tree.empty()) out.events += 1;
  }

  shapes(out) {
    const k = LichtenbergMachine.kHalf + 0.012;
    out.pushShape(ShapeKind.Box, -k, -k, k, k, 0, Material.Acrylic);
    if (this.s.origin === 0) out.pushShape(ShapeKind.Disc, 0, 0, 0, 0, 0.004, Material.Metal);
    else out.pushShape(ShapeKind.Box, -k, -k - 0.006, k, -k, 0, Material.Metal);
  }
}

// ---- the engine ---------------------------------------------------------------------
export class Engine {
  constructor() {
    this.machines = [new LadderMachine(), new TeslaMachine(), new VdgMachine(), new GlobeMachine(), new LichtenbergMachine()];
    this.active = Machine.Count;
    this.clock = -1.0;
    this.fireFlag = false;
    this.restartFlag = true;
    this.settings = null;
    this.lastMillis = 0;
  }

  configure(next) {
    this.settings = next;
    const m = clamp(next.machine, Machine.Ladder, Machine.Lichtenberg);
    if (m !== this.active) {
      this.active = m;
      this.restartFlag = true;
    }
    this.machines[this.active].setup(next);
  }

  fire() { this.fireFlag = true; }
  restart() { this.restartFlag = true; }

  /** The active machine's lattice size, for the page's statistics line. */
  lattice() {
    const F = this.machines[this.active]?.field;
    return F ? [F.nx, F.ny] : [0, 0];
  }

  advance(now, framePeriod, out) {
    const start = performance.now();
    out.segments.length = 0;
    out.shapes.length = 0;
    out.joules = 0.0;
    out.events = 0;

    const m = this.machines[this.active];
    out.sceneHeight = sceneHeight(this.active);

    if (this.clock < 0.0 || now < this.clock || now - this.clock > 0.5 || this.restartFlag) {
      if (this.restartFlag || this.clock < 0.0) m.restart(now);
      else m.rebase(now);
      this.restartFlag = false;
      this.clock = now;
      if (this.fireFlag) {
        this.fireFlag = false;
        m.fire(now);
      }
      m.shapes(out);
      this.lastMillis = performance.now() - start;
      return;
    }

    if (this.fireFlag) {
      this.fireFlag = false;
      m.fire(now);
    }

    const period = Math.max(framePeriod, now - this.clock);
    const exposeFrom = now - clamp(this.settings.shutter, 0.0, 1.0) * period;
    m.step(this.clock, now, exposeFrom, out);
    m.shapes(out);
    this.clock = now;
    this.lastMillis = performance.now() - start;
  }
}

//===========================================================================
// Controls.h / Controls.cpp
//===========================================================================
export const PT = {};
[
  'PRESET', 'MACHINE', 'FIRE', 'SEED',
  'SUPPLY', 'VOLTAGE', 'IMPEDANCE',
  'BRANCHING', 'DETAIL', 'MEMORY', 'REACH',
  'ROD_SPREAD', 'ROD_LENGTH', 'RISE', 'WIND',
  'BPS', 'TOPLOAD', 'TARGET', 'TARGET_X', 'TARGET_Y',
  'BELT', 'SPHERE', 'GAP', 'FINISH',
  'GLOBE', 'FINGER', 'FINGER_X', 'FINGER_Y',
  'ORIGIN',
  'AUDIO', 'AUDIO_FIRES', 'AUDIO_DRIVE',
  'EFFICIENCY', 'GLOW', 'GAS', 'SHUTTER', 'PERSISTENCE', 'APPARATUS', 'BACK_R', 'BACK_G', 'BACK_B',
  'DETECT', 'THRESHOLD', 'ILLUMINATION', 'MIX',
  'POS_X', 'POS_Y', 'SCALE', 'ROTATION',
].forEach((name, i) => { PT[name] = i; });
export const PT_COUNT = Object.keys(PT).length;

export function nominalFor(machine) {
  switch (machine) {
    case Machine.Ladder: return { eta: 4.0, cells: 128, memory: 0.020, camera: 0.900, supply: SupplyKind.NST };
    case Machine.Tesla: return { eta: 1.6, cells: 136, memory: 0.020, camera: 0.400, supply: SupplyKind.NST };
    case Machine.VanDeGraaff: return { eta: 3.5, cells: 144, memory: 0.020, camera: 4.000, supply: SupplyKind.NST };
    case Machine.Globe: return { eta: 2.0, cells: 96, memory: 0.080, camera: 3.000, supply: SupplyKind.Flyback };
    case Machine.Lichtenberg: return { eta: 1.0, cells: 176, memory: 0.020, camera: 1.000, supply: SupplyKind.ZVS };
    default: return { eta: 1.6, cells: 136, memory: 0.020, camera: 0.400, supply: SupplyKind.NST };
  }
}

export function option(value, count) {
  return clamp(lround(value), 0, count - 1);
}

const geometric = (v, lo, hi) => lo * Math.pow(hi / lo, clamp(v, 0.0, 1.0));
const lerp = (a, b, t) => a + (b - a) * t;

export const relativeFromParam = (v, range) => Math.pow(range, 2.0 * clamp(v, 0.0, 1.0) - 1.0);
export function bpsFromParam(v) {
  if (v < f32(0.02)) return 0.0;
  return geometric(f32((v - f32(0.02)) / f32(0.98)), 5.0, 1000.0);
}
export const beltFromParam = (v) => geometric(v, 1e-6, 50e-6);
export const shutterFromParam = (v) => 0.02 + 0.98 * clamp(v, 0.0, 1.0);
export function persistenceFromParam(v) {
  if (v < f32(0.02)) return 0.0;
  return geometric(f32((v - f32(0.02)) / f32(0.98)), 0.005, 0.5);
}
export const topFromParam = (v) => lerp(0.06, 0.35, clamp(v, 0.0, 1.0));
export const sphereFromParam = (v) => lerp(0.05, 0.30, clamp(v, 0.0, 1.0));
export const gapFromParam = (v) => geometric(v, 0.01, 0.20);
export const finishFromParam = (v) => lerp(P.kRoughestFinish, 1.0, clamp(v, 0.0, 1.0));
export const globeFromParam = (v) => lerp(0.08, 0.24, clamp(v, 0.0, 1.0));
export const spreadFromParam = (v) => (lerp(8.0, 40.0, clamp(v, 0.0, 1.0)) * P.kPi) / 180.0;
export const rodLengthFromParam = (v) => lerp(0.25, 0.60, clamp(v, 0.0, 1.0));
export const riseFromParam = (v) => geometric(v, 0.2, 3.0);
export const windFromParam = (v) => lerp(-0.5, 0.5, clamp(v, 0.0, 1.0));

/**
 * Resolve(): the params, already through the preset override, to the engine's
 * Settings and the renderer's Look. `p` is indexed by PT; `level` is the audio
 * level, always 0 here.
 */
export function resolve(p, level, aspect) {
  const m = option(p[PT.MACHINE], Machine.Count);
  const n = nominalFor(m);
  const s = {};
  s.machine = m;
  s.seed = 1 + Math.trunc(Math.floor(f32(clamp(p[PT.SEED], 0.0, 1.0) * f32(999.0))));

  const kind = option(p[PT.SUPPLY], SupplyKind.Count);
  const supply = nominalSupply(kind);
  const drive = 1.0 + clamp(p[PT.AUDIO_DRIVE], 0.0, 1.0) * clamp(level, 0.0, 1.0);
  supply.openVolts *= relativeFromParam(p[PT.VOLTAGE], 2.0) * drive;
  supply.sourceOhms *= relativeFromParam(p[PT.IMPEDANCE], 4.0);
  s.supply = supply;

  s.eta = n.eta * relativeFromParam(p[PT.BRANCHING], 2.0);
  s.cellsHigh = clamp(lround(n.cells * relativeFromParam(p[PT.DETAIL], 2.0)), 32, 512);
  s.memory = n.memory * relativeFromParam(p[PT.MEMORY], 4.0);
  s.reach = relativeFromParam(p[PT.REACH], 4.0);

  s.rodSpread = spreadFromParam(p[PT.ROD_SPREAD]);
  s.rodLength = rodLengthFromParam(p[PT.ROD_LENGTH]);
  s.rise = riseFromParam(p[PT.RISE]);
  s.wind = windFromParam(p[PT.WIND]);

  s.bps = bpsFromParam(p[PT.BPS]);
  s.topload = topFromParam(p[PT.TOPLOAD]);
  s.target = option(p[PT.TARGET], 3);
  s.targetX = clamp(p[PT.TARGET_X], 0.0, 1.0);
  s.targetY = clamp(p[PT.TARGET_Y], 0.0, 1.0);

  s.belt = beltFromParam(p[PT.BELT]);
  s.sphere = sphereFromParam(p[PT.SPHERE]);
  s.gap = gapFromParam(p[PT.GAP]);
  s.finish = finishFromParam(p[PT.FINISH]);

  s.globe = globeFromParam(p[PT.GLOBE]);
  s.finger = p[PT.FINGER] > 0.5;
  s.fingerX = clamp(p[PT.FINGER_X], 0.0, 1.0);
  s.fingerY = clamp(p[PT.FINGER_Y], 0.0, 1.0);

  s.origin = option(p[PT.ORIGIN], 2);

  s.efficiency = 0.01 * relativeFromParam(p[PT.EFFICIENCY], 10.0);
  s.shutter = shutterFromParam(p[PT.SHUTTER]);
  s.aspect = aspect;

  s.posX = (clamp(p[PT.POS_X], 0.0, 1.0) - 0.5) * aspect;
  s.posY = clamp(p[PT.POS_Y], 0.0, 1.0) - 0.5;
  s.scale = relativeFromParam(p[PT.SCALE], 4.0);
  s.rotation = (clamp(p[PT.ROTATION], 0.0, 1.0) - 0.5) * 2.0 * P.kPi;

  s.clip = null;
  s.clipW = 0;
  s.clipH = 0;
  s.clipStamp = 0;

  const look = {
    sceneHeight: sceneHeight(m),
    posX: s.posX, posY: s.posY, scale: s.scale, rotation: s.rotation,
    camera: f32(n.camera),
    glow: f32(f32(0.9) * clamp(p[PT.GLOW], 0.0, 1.0)),
    gas: streamerColour(option(p[PT.GAS], Gas.Count)),
    arc: blackbodyColour(P.kArcKelvin),
    apparatus: p[PT.APPARATUS] > 0.5 ? 1.0 : 0.0,
    background: [clamp(p[PT.BACK_R], 0, 1), clamp(p[PT.BACK_G], 0, 1), clamp(p[PT.BACK_B], 0, 1)],
    illumination: f32(40.0 * clamp(p[PT.ILLUMINATION], 0.0, 1.0)),
    mix: clamp(p[PT.MIX], 0.0, 1.0),
    decay: 0,
    clearHistory: false,
  };

  return {
    engine: s,
    look,
    persistenceSeconds: f32(persistenceFromParam(p[PT.PERSISTENCE])),
    detect: option(p[PT.DETECT], 3),
    threshold: clamp(p[PT.THRESHOLD], 0.0, 1.0),
    audioFires: clamp(p[PT.AUDIO_FIRES], 0.0, 1.0),
  };
}

const deg = (rad) => (rad * 180.0) / P.kPi;
const sign = (v, digits) => `${v >= 0 ? '+' : ''}${v.toFixed(digits)}`;

/** Display(): the string the plugin hands the host, or '' for "no unit". */
export function display(index, p) {
  const r = resolve(p, 0.0, 16.0 / 9.0);
  const s = r.engine;
  switch (index) {
    case PT.SEED: return `${s.seed}`;
    case PT.VOLTAGE: return `${(s.supply.openVolts / 1000.0).toFixed(1)} kV`;
    case PT.IMPEDANCE: return `${(s.supply.sourceOhms / 1e6).toFixed(2)} MOhm`;
    case PT.BRANCHING: return `eta ${s.eta.toFixed(2)}`;
    case PT.DETAIL: return `${s.cellsHigh} sites`;
    case PT.MEMORY: return `${(s.memory * 1000.0).toFixed(0)} ms`;
    case PT.REACH: return `x${s.reach.toFixed(2)}`;
    case PT.ROD_SPREAD: return `${deg(s.rodSpread).toFixed(0)} deg`;
    case PT.ROD_LENGTH: return `${s.rodLength.toFixed(2)} m`;
    case PT.RISE: return `${s.rise.toFixed(2)} m/s`;
    case PT.WIND: return `${sign(s.wind, 2)} m/s`;
    case PT.BPS: return s.bps <= 0.0 ? 'off' : `${s.bps.toFixed(0)} BPS`;
    case PT.TOPLOAD: return `${s.topload.toFixed(2)} m`;
    case PT.BELT: return `${(s.belt * 1e6).toFixed(1)} uA`;
    case PT.SPHERE: return `${s.sphere.toFixed(2)} m`;
    case PT.GAP: return `${(s.gap * 100.0).toFixed(1)} cm`;
    case PT.FINISH: return s.finish >= 0.99995 ? 'polished' : `m ${s.finish.toFixed(4)}`;
    case PT.GLOBE: return `${s.globe.toFixed(2)} m`;
    case PT.EFFICIENCY: return `${(s.efficiency * 100.0).toFixed(2)} %`;
    case PT.GLOW: return `${(r.look.glow * 100.0).toFixed(0)} %`;
    case PT.SHUTTER: return `${(s.shutter * 360.0).toFixed(0)} deg`;
    case PT.PERSISTENCE: return r.persistenceSeconds <= 0.0 ? 'off' : `${(r.persistenceSeconds * 1000.0).toFixed(0)} ms`;
    case PT.ILLUMINATION: return `x${r.look.illumination.toFixed(1)}`;
    case PT.SCALE: return `x${s.scale.toFixed(2)}`;
    case PT.ROTATION: return `${deg(s.rotation).toFixed(0)} deg`;
    default: return '';
  }
}

//===========================================================================
// Presets.h, and FlybackPlugin::Effective.
//===========================================================================
/** Which parameter each preset column drives, in presets::Param order. */
export const PRESET_COLUMNS = [
  PT.MACHINE, PT.SUPPLY, PT.VOLTAGE, PT.IMPEDANCE, PT.BRANCHING, PT.MEMORY, PT.REACH, PT.ROD_SPREAD, PT.ROD_LENGTH,
  PT.RISE, PT.WIND, PT.BPS, PT.TOPLOAD, PT.TARGET, PT.BELT, PT.SPHERE, PT.GAP, PT.FINISH, PT.GLOBE,
  PT.FINGER, PT.ORIGIN, PT.EFFICIENCY, PT.GLOW, PT.GAS, PT.SHUTTER, PT.PERSISTENCE,
];

//                      Mach Sup  Volt Imp  Brn  Mem  Rch  Spread  RodLen  Rise    Wind BPS     Top     Tgt Belt    Sphere  Gap     Fin  Globe   Fgr Org Eff  Glow  Gas Shut Pers
export const PRESETS = [
  { name: 'Tesla Coil', v: [1, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3750, 0.4286, 0.5943, 0.5, 0.6078, 0.3103, 1, 0.5886, 0.4000, 0.7686, 1.0, 0.7500, 0, 0, 0.5, 0.45, 0, 1.0, 0.30] },
  { name: "Jacob's Ladder", v: [0, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3750, 0.4286, 0.5943, 0.5, 0.6078, 0.3103, 1, 0.5886, 0.4000, 0.7686, 1.0, 0.7500, 0, 0, 0.5, 0.50, 0, 1.0, 0.10] },
  { name: 'Van de Graaff', v: [2, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3750, 0.4286, 0.5943, 0.5, 0.6078, 0.3103, 1, 0.5886, 0.4000, 0.7686, 1.0, 0.7500, 0, 0, 0.5, 0.50, 0, 1.0, 0.40] },
  { name: 'Plasma Globe', v: [3, 2, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3750, 0.4286, 0.5943, 0.5, 0.6078, 0.3103, 1, 0.5886, 0.4000, 0.7686, 1.0, 0.7500, 0, 0, 0.5, 0.55, 3, 1.0, 0.30] },
  { name: 'Lichtenberg', v: [4, 0, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3750, 0.4286, 0.5943, 0.5, 0.6078, 0.3103, 1, 0.5886, 0.4000, 0.7686, 1.0, 0.7500, 0, 0, 0.5, 0.40, 0, 1.0, 0.30] },
];

/**
 * FlybackPlugin::Effective for every index at once: the raw params (as C++
 * floats) with the selected preset row laid over its columns.
 */
export function effective(raw) {
  const out = raw.map((v) => f32(v));
  const preset = option(out[PT.PRESET], PRESETS.length + 1);
  if (preset > 0) {
    const row = PRESETS[preset - 1];
    for (let c = 0; c < PRESET_COLUMNS.length; c += 1) out[PRESET_COLUMNS[c]] = f32(row.v[c]);
  }
  return out;
}
