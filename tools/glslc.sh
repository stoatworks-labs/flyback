#!/usr/bin/env bash
#
# Every shader in source/render/Shaders.cpp through a real GLSL compiler.
# A shader that will not compile is "the plugin does nothing" in a host, with
# the message in a log file.
#
# Called by tools/verify.sh AND by CI (.github/workflows/ci.yml), which has no
# GL context to compile them through the driver: one copy of the extraction,
# so neither can drift from the other.
#
# --target-env=opengl4.5 -fauto-map-locations: glslc targets SPIR-V and would
# otherwise demand Vulkan's explicit locations.
#
#     tools/glslc.sh              skips (exit 0) when glslc is not installed
#     tools/glslc.sh --require    fails (exit 1) when glslc is not installed (CI)
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if ! command -v glslc >/dev/null 2>&1; then
	if [ "${1:-}" = "--require" ]; then
		printf '   glslc not installed (brew install shaderc), and --require was given\n'
		exit 1
	fi
	printf '   skipped: glslc not installed (brew install shaderc)\n'
	exit 0
fi

dir="$( mktemp -d )"
trap 'rm -rf "$dir"' EXIT
python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )
FILES = [ "source/render/Shaders.cpp" ]
for f in FILES:
	text = pathlib.Path( f ).read_text()
	named = {}
	for m in re.finditer( r'(\w+)\s*=\s*R"\((.*?)\)"', text, re.S ):
		named[ m.group( 1 ) ] = m.group( 2 )
	# Adjacent raw literals, joined: MSVC C2026 caps one literal at ~16 KB, so a
	# shader that outgrows it is split and has to be rejoined here.
	for m in re.finditer( r'(\w+)\s*=\s*((?:R"\(.*?\)"\s*){2,});', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )
	for name, body in named.items():
		if body.lstrip().startswith( "#version" ) and "void main" in body:
			ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
			( out / ( name + ext ) ).write_text( body )
SHADERS_PY

bad=0
n=0
for shader in "$dir"/*.vert "$dir"/*.frag; do
	[ -e "$shader" ] || continue
	n=$(( n + 1 ))
	if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
		printf '   %s does not compile\n' "$( basename "$shader" )"
		sed "s|$dir/||; s|^|      |" "$dir/err"
		bad=$(( bad + 1 ))
	fi
done
if [ "$n" -eq 0 ]; then
	# Nothing extracted is a failure: the extraction has lost the shaders,
	# and a check that looks at nothing is worse than none.
	printf '   no shaders were extracted -- the extraction has gone stale\n'
	exit 1
fi
[ "$bad" -eq 0 ] && printf '   %d shaders, all compile\n' "$n"
[ "$bad" -eq 0 ]
