#!/usr/bin/env python3
"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders. And a machine-specific control that the engine never reads looks
exactly as healthy.

Most of Flyback's controls belong to one machine, so each is swept in the
context where it can act: Rod Spread on the Jacob's ladder, Belt Current on the
Van de Graaff, Target X only with Target on Point, the Over group through the
effect over a clip, Audio Fires with a beat fed in. A control that changes
nothing in its own context is dead.

It also asks for more than "changed a pixel": a control whose ends differ by
less than `--floor` (mean 8-bit difference per channel) is reported as barely
alive. vectrix's lesson: a control can be alive, correct and still useless.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N]
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Machine indices, as the Machine dropdown numbers them.
LADDER, TESLA, VDG, GLOBE, LICHTENBERG = 0, 1, 2, 3, 4

# name -> (low setting, high setting, context settings, extra flags)
# Frames: 48 at 60 fps is 0.8 s -- two Van de Graaff sparks, a ladder climb,
# a Lichtenberg figure a quarter grown.
SWEEP = {
    "Preset": ("0", "3", [], []),
    "Machine": ("0", "4", [], []),
    "Fire": (None, None, [f"Machine={TESLA}", "BPS=0"], ["fire"]),
    "Seed": ("0", "1", [], []),
    "Supply": ("0", "2", [f"Machine={LADDER}"], []),
    "Voltage": ("0", "1", [], []),
    "Source Impedance": ("0", "1", [f"Machine={LADDER}"], []),
    "Branching": ("0", "1", [], []),
    "Detail": ("0", "1", [], []),
    "Channel Memory": ("0", "1", [], []),
    "Reach": ("0", "1", [], []),
    "Rod Spread": ("0", "1", [f"Machine={LADDER}"], []),
    "Rod Length": ("0", "1", [f"Machine={LADDER}"], []),
    "Rise Speed": ("0", "1", [f"Machine={LADDER}"], []),
    "Wind": ("0.5", "0.9", [f"Machine={LADDER}"], []),
    "BPS": ("0.2", "1", [], []),
    "Topload Size": ("0", "1", [], []),
    "Target": ("0", "2", [], []),
    "Target X": ("0.3", "0.8", ["Target=2"], []),
    "Target Y": ("0.2", "0.7", ["Target=2"], []),
    "Belt Current": ("0.3", "1", [f"Machine={VDG}"], []),
    "Sphere Size": ("0", "1", [f"Machine={VDG}"], []),
    "Gap": ("0", "1", [f"Machine={VDG}"], []),
    "Globe Size": ("0", "1", [f"Machine={GLOBE}"], []),
    "Finger": ("0", "1", [f"Machine={GLOBE}"], []),
    "Finger X": ("0.2", "0.8", [f"Machine={GLOBE}", "Finger=1"], []),
    "Finger Y": ("0.2", "0.8", [f"Machine={GLOBE}", "Finger=1"], []),
    "Origin": ("0", "1", [f"Machine={LICHTENBERG}"], []),
    # The beat's second hit is at 0.5 s, frame 30: end on it.
    "Audio Fires": ("0", "1", [f"Machine={TESLA}", "BPS=0"], ["beat", "frames=31"]),
    "Audio Drive": ("0", "1", [], ["beat"]),
    "Efficiency": ("0", "1", [], []),
    "Glow": ("0", "1", [], []),
    "Gas": ("0", "1", [], []),
    "Shutter": ("0", "1", [f"Machine={VDG}", "Belt Current=1"], []),
    "Persistence": ("0", "1", [], []),
    "Show Apparatus": ("0", "1", [], []),
    "Background": ("0", "1", [], []),
    "Background Green": ("0", "1", [], []),
    "Background Blue": ("0", "1", [], []),
    "Detect On": ("0", "2", [], ["effect"]),
    "Ground Threshold": ("0.1", "0.9", [], ["effect"]),
    "Illumination": ("0", "1", [], ["effect"]),
    "Mix": ("0", "1", [], ["effect"]),
    "Position X": ("0.3", "0.7", [], []),
    "Position Y": ("0.3", "0.7", [], []),
    "Scale": ("0.3", "0.7", [], []),
    "Rotation": ("0.3", "0.7", [], []),
}

# Parameters with no pixel to sweep: the FFT buffer (its float is
# meaningless; Audio Fires and Audio Drive are its sweepable proof) and the
# About block (a text line and browser buttons).
SKIP = {"Audio", "About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for hvtest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(hvtest, out, settings, extra, frames=48):
    for e in extra:
        if e.startswith("frames="):
            frames = int(e.split("=")[1])
    args = [str(hvtest), "--out", str(out), "--size", "320x180", "--frames", str(frames)]
    if "effect" in extra:
        args.append("--effect")
    if "beat" in extra:
        args.append("--beat")
    if "fire" in extra:
        args += ["--fire", str(frames - 1)]
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hvtest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(hvtest):
    result = subprocess.run([str(hvtest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hvtest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(hvtest, scratch, name):
    low, high, context, extra = SWEEP[name]
    a = scratch / f"{abs(hash(name))}-a.png"
    b = scratch / f"{abs(hash(name))}-b.png"
    if "fire" in extra:
        before = render(hvtest, a, context, [e for e in extra if e != "fire"])
        after = render(hvtest, b, context, extra)
    else:
        before = render(hvtest, a, context + [f"{name}={low}"], extra)
        after = render(hvtest, b, context + [f"{name}={high}"], extra)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    hvtest = build / "hvtest"
    if not hvtest.exists():
        print(f"{hvtest} not found", file=sys.stderr)
        return 1

    declared = parameters(hvtest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    stale = [n for n in SWEEP if n not in declared]
    if stale:
        print(f"the sweep names parameters that do not exist: {', '.join(stale)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        names = [n for n in declared if n in SWEEP]
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(hvtest, scratch, n), names))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:18s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:18s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:18s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept, {len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
              "the C++, or an engine setting nothing reads. Both are silent everywhere else.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
