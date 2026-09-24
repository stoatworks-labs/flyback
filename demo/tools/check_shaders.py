"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

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

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `demo/engine.js` is a hand translation of
source/engine/ (Lattice, Dbm, Tree, Physics, Engine), source/Controls.cpp and
source/Presets.h, and the renderer class in plugin.js is a translation of
source/render/Renderer.cpp. Only a reader can tell whether they still agree.
When you change one of those, change the port too -- and remember that a wrong
port shows up on the page as a discharge that is subtly the wrong shape, which
nobody will notice.
"""
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

    print()
    if problems:
        print(f"{problems} item(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders and the glow weights are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
