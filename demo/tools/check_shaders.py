"""The demo's shaders must be the plugin's shaders, character for character,
and the demo's WebAssembly must be built from the plugin's current sources.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted, the .wasm
is stale, or a served file names a device API.

------------------------------------------------------------------- why

`demo/plugin.js` holds nine GLSL stages and so does `source/render/Shaders.cpp`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of the page is that it runs the plugin's own
light path rather than something reimplemented to look similar, so the claim
needs something enforcing it.

Nothing else can. `hvtest` drives the real plugin class and has no idea this
page exists, and `tools/glslc.sh` compiles the C++ copies and never looks at
the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment that has been updated on one side and not the other
is exactly the drift worth catching, because comments in this repo carry the
reasoning: why the across profile is renormalised, why the pyramid sums rather
than averages, why nothing on the light path may be half float.

The one transformation is a decode, not a normalisation. The segment fragment
quotes `half` in a comment with backticks, and a backtick cannot appear raw
inside a JavaScript template literal, so `plugin.js` escapes it as \\`. This
unescapes that and *rejects any other backslash on the JS side*; there are none
anywhere in the C++ shaders, so a second escape could only be somebody hiding a
difference.

The singing coil adds three more (2026-11-18):

- **The lamp's shader.** `demo/singing.js` holds `LAMP_FRAGMENT`, the body of
  `kLampFragment` in `source/render/Lamp.cpp` (the lamp lives outside
  Shaders.cpp on purpose, so the nine above stay the light path's), compared
  the same way. Its colours and placement are ported by hand and not checked.

- **No device.** No file the page serves may name getUserMedia,
  enumerateDevices or mediaDevices, or make a realtime AudioContext: the page
  never asks for a microphone, never lists an audio device and plays nothing
  aloud (it decodes a visitor's file in an OfflineAudioContext, which renders
  to memory), and this keeps it so. (woofer's rule, with the AudioContext.)

- **The WebAssembly.** `demo/flyback-sing.wasm` is source/Singing.cpp (the
  pitch input), source/Diag.cpp and earshot's snapshot, compiled by
  `demo/tools/build-wasm.sh` and committed, because the deploy has no build
  step. Building it needs emscripten, which verify.sh cannot assume, so this
  checks the next best thing: `demo/wasm/inputs.sha256`, written by the build,
  records the hash of every file the compiler read (its own dependency list:
  the sources, every header they include, the glue), of the build script and
  of both outputs. An input changed since the last build -- a new earshot
  snapshot included -- or an output that is not the one the build wrote fails
  here, and so does an earshot source file the build did not compile (only
  the two miniaudio files are left out, EARSHOT_CAPTURE=OFF). The fix is to
  rerun build-wasm.sh. (woofer's check, with the compiler's list in place of a
  hand-kept one.)

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `demo/engine.js` is a hand translation of
source/engine/ (Lattice, Dbm, Tree, Physics, Engine -- the singing coil's
phase-accumulator interrupter, StepSung, included), source/Controls.cpp and
source/Presets.h, and the renderer class in plugin.js is a translation of
source/render/Renderer.cpp. Nor can it tell whether the .wasm really is what
its inputs compile to (only rebuilding can; the build is reproducible). Only a reader can tell whether they still agree.
When you change one of those, change the port too -- and remember that a wrong
port shows up on the page as a discharge that is subtly the wrong shape, which
nobody will notice.
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol. Every stage Renderer::InitGL compiles.
SHADERS = [
    ("QUAD_VERTEX", "source/render/Shaders.cpp", "kQuadVertex"),
    ("SEGMENT_VERTEX", "source/render/Shaders.cpp", "kSegmentVertex"),
    ("SEGMENT_FRAGMENT", "source/render/Shaders.cpp", "kSegmentFragment"),
    ("DECAY_FRAGMENT", "source/render/Shaders.cpp", "kDecayFragment"),
    ("DOWN_FRAGMENT", "source/render/Shaders.cpp", "kDownFragment"),
    ("BLUR_FRAGMENT", "source/render/Shaders.cpp", "kBlurFragment"),
    ("LIGHT_FRAGMENT", "source/render/Shaders.cpp", "kLightFragment"),
    ("DISPLAY_FRAGMENT", "source/render/Shaders.cpp", "kDisplayFragment"),
    ("GROUND_FRAGMENT", "source/render/Shaders.cpp", "kGroundFragment"),
]

# Not a shader, but a number the light pass depends on and the C++ keeps beside
# them: the halo's octave weights. The page hands the same six to the same
# uniform, so they are held to the C++ too.
GLOW_CPP = re.compile(r"const float kGlowWeights\[ kGlowLevels \] = \{ ([^}]*) \};")
GLOW_JS = re.compile(r"^const kGlowWeights = \[([^\]]*)\]", re.M)

# The singing lamp's fragment: JS constant in demo/singing.js, C++ file, symbol.
LAMP = ("LAMP_FRAGMENT", "source/render/Lamp.cpp", "kLampFragment")

# What a page that never touches a device, and never plays a sound, must never
# name. OfflineAudioContext renders to memory and is allowed; a realtime one is
# an output device.
DEVICE_APIS = ("getUserMedia", "enumerateDevices", "mediaDevices")
REALTIME_AUDIO = re.compile(r"(?<!Offline)\bAudioContext\b|webkitAudioContext")

MANIFEST = os.path.join("demo", "wasm", "inputs.sha256")
OUTPUTS = ("demo/flyback-sing.js", "demo/flyback-sing.wasm")
# The pitch input's sources: they must have gone in.
PLUGIN_INPUTS = ("source/Singing.cpp", "source/Diag.cpp")
# earshot's device backend, which build-wasm.sh leaves out (EARSHOT_CAPTURE=OFF).
NOT_COMPILED = ("Miniaudio.cpp", "MiniaudioImpl.cpp")


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    if "${" in body:
        return None, "template substitution"

    return body.replace("\\`", "`"), None


def numbers(text):
    return [float(v.strip().rstrip("f")) for v in text.split(",") if v.strip()]


def compare(name, path, symbol, js, where):
    """One shader body, C++ against JS. Returns the number of problems."""
    cpp_text = from_cpp(path, symbol)
    js_text, complaint = from_js(js, name)
    if cpp_text is None:
        print(f"FAIL  {symbol} not found in {path}")
        return 1
    if complaint is not None:
        print(f"FAIL  {name} in {where} has a {complaint}")
        return 1
    if js_text is None:
        print(f"FAIL  {name} not found in {where}")
        return 1
    if cpp_text == js_text:
        print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
        return 0
    print(f"FAIL  {name} has drifted from {symbol} in {path}")
    cpp_lines = cpp_text.splitlines()
    js_lines = js_text.splitlines()
    for i in range(max(len(cpp_lines), len(js_lines))):
        a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
        b = js_lines[i] if i < len(js_lines) else "<missing>"
        if a != b:
            print(f"        first difference at line {i + 1}")
            print(f"          C++: {a}")
            print(f"          js : {b}")
            break
    return 1


def check_no_devices():
    problems = 0
    demo = os.path.join(REPO, "demo")
    served = []
    for root, dirs, files in os.walk(demo):
        rel = os.path.relpath(root, demo)
        if rel.split(os.sep)[0] in ("tools", "wasm"):
            continue  # not served (.assetsignore), and this file names them
        for name in files:
            if name.endswith((".js", ".html")):
                served.append(os.path.join(root, name))
    for path in sorted(served):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for api in DEVICE_APIS:
            if api in text:
                print(f"FAIL  {os.path.relpath(path, REPO)} names {api}: the page must never ask for or list an audio device")
                problems += 1
        if REALTIME_AUDIO.search(text):
            print(f"FAIL  {os.path.relpath(path, REPO)} makes a realtime AudioContext: the page plays nothing aloud")
            problems += 1
    if problems == 0:
        print(f"ok    {'NO DEVICES':<18} none of the {len(served)} served scripts and pages names {', '.join(DEVICE_APIS)} or a realtime AudioContext")
    return problems


def sha256(path):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, path), "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def check_wasm():
    try:
        lines = read_text(MANIFEST).splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1

    problems = 0
    entries = 0
    recorded = set()
    for line in lines:
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        recorded.add(path)
        entries += 1
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            if path in OUTPUTS:
                print(f"FAIL  {path} is not the file demo/tools/build-wasm.sh wrote -- rerun it; never edit an output by hand")
            else:
                print(f"FAIL  {path} has changed since demo/flyback-sing.wasm was built -- rerun demo/tools/build-wasm.sh")
            problems += 1

    for path in PLUGIN_INPUTS + ("demo/wasm/glue.cpp", "demo/tools/build-wasm.sh") + OUTPUTS:
        if path not in recorded:
            print(f"FAIL  {MANIFEST} does not record {path}")
            problems += 1

    # An earshot source added since the build is one the build never compiled.
    tree = os.path.join("external", "earshot", "src")
    for name in sorted(os.listdir(os.path.join(REPO, tree))):
        rel = f"{tree}/{name}".replace(os.sep, "/")
        if name.endswith(".cpp") and name not in NOT_COMPILED and rel not in recorded:
            print(f"FAIL  {rel} is not in the .wasm's manifest -- add it to build-wasm.sh's EARSHOT_SOURCES and rebuild")
            problems += 1

    for output in OUTPUTS:
        mode = os.stat(os.path.join(REPO, output)).st_mode & 0o777 if os.path.exists(os.path.join(REPO, output)) else 0
        if mode & 0o111:
            print(f"FAIL  {output} is executable ({oct(mode)}): served files are 644")
            problems += 1
    if problems == 0:
        print(f"ok    {'WASM':<18} built from these sources ({entries} files hashed, every earshot source but miniaudio's compiled)")
    return problems


def read_text(path):
    with open(os.path.join(REPO, path)) as handle:
        return handle.read()


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    with open(os.path.join(REPO, "source", "render", "Shaders.cpp")) as handle:
        cpp_glow = GLOW_CPP.search(handle.read())
    js_glow = GLOW_JS.search(js)
    if cpp_glow is None or js_glow is None:
        print("FAIL  kGlowWeights not found on one side")
        problems += 1
    elif numbers(cpp_glow.group(1)) != numbers(js_glow.group(1)):
        print(f"FAIL  kGlowWeights differ: C++ {cpp_glow.group(1)} / js {js_glow.group(1)}")
        problems += 1
    else:
        print(f"ok    {'kGlowWeights':<18} matches ({js_glow.group(1)})")

    try:
        singing_js = read_text(os.path.join("demo", "singing.js"))
    except FileNotFoundError:
        print("FAIL  demo/singing.js is missing")
        singing_js = ""
        problems += 1
    if singing_js:
        problems += compare(LAMP[0], LAMP[1], LAMP[2], singing_js, "demo/singing.js")
    problems += check_no_devices()
    problems += check_wasm()

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across (do not edit plugin.js or singing.js by hand),"
              " and rerun demo/tools/build-wasm.sh for the .wasm")
        return 1

    print(f"all {len(SHADERS)} shaders, the glow weights and the lamp are identical to the plugin's,"
          " no served file names a device API, and the .wasm is built from the current sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
