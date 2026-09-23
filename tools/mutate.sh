#!/usr/bin/env bash
#
# Mutation-test the harness: change ONE character in the shipped engine or
# GLSL, rebuild, and require the check that should notice to fail. A harness
# that passes a mutated plugin is measuring something other than the plugin.
#
# Not part of verify.sh (each mutant is a rebuild, ~5 minutes in all); run it
# when a check or the code it guards changes. Each mutant is built in its own
# copy of the tree, so the working tree is never touched.
#
#     tools/mutate.sh
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$( mktemp -d )"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it means
MUTANTS=(
	"source/render/Shaders.cpp|return 0.5 * ( 1.0 + erfAS( x * 0.70710678118654752 ) );|return 0.6 * ( 1.0 + erfAS( x * 0.70710678118654752 ) );|light|GLSL: the normal CDF 20% high"
	"source/render/Shaders.cpp|light = ( 1.0 - Glow ) * e + Glow * g;|light = ( 1.0 + Glow ) * e + Glow * g;|light|GLSL: the core adds the glow's share instead of giving it up"
	"source/engine/Physics.cpp|const double beta  = 2.0 * arc.B * v + 4.0 * supply.sourceOhms * arc.D;|const double beta  = 2.0 * arc.B * v + 5.0 * supply.sourceOhms * arc.D;|ladder|engine: L*'s linear coefficient"
	"source/engine/Physics.cpp|const Charge gb = { -last.q * b / r, s - b * b / r };|const Charge gb = { -last.q * b / r, s + b * b / r };|vdg|engine: an image charge on the wrong side of the grounded sphere"
	"source/engine/Lattice.cpp|yr[ i ] = -mr[ i ] * ( xr[ i - 1 ] + xr[ i + 1 ] + xr[ i - nx ] + xr[ i + nx ] - 4.0f * xr[ i ] );|yr[ i ] = -mr[ i ] * ( xr[ i - 1 ] + xr[ i + 1 ] + xr[ i - nx ] + xr[ i + nx ] - 5.0f * xr[ i ] );|laplace|engine: the Laplacian's centre weight"
	"source/engine/Dbm.cpp|const double field = ( 1.0 - phi ) / kBond[ bestK ];|const double field = ( 1.0 + phi ) / kBond[ bestK ];|dimension|engine: growth toward the potential instead of down it"
	"source/engine/Engine.cpp|next  = anchor + static_cast< double >( bangIndex + 1 ) / s.bps;|next  = anchor + static_cast< double >( bangIndex + 2 ) / s.bps;|tesla|engine: the interrupter skips a beat"
)

caught=0
missed=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check meaning <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree"
	# Everything but the builds; the SDK by link.
	( cd "$REPO" && tar cf - --exclude='./build*' --exclude='./external' --exclude='./.git' . ) | ( cd "$tree" && tar xf - )
	mkdir -p "$tree/external"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY' || { echo "  MUTANT NOT APPLIED: $meaning"; missed=$(( missed + 1 )); continue; }
import sys
path, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if text.count(a) != 1:
    sys.exit(1)
diff = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
if diff != 1:
    print(f"not a one-character mutant: {diff} differ", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(text.replace(a, b))
PY

	if ! cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null 2>&1 \
	   || ! cmake --build "$tree/build" --target hvtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1; then
		echo "  (did not build: $meaning)"
		missed=$(( missed + 1 ))
		continue
	fi
	if "$tree/build/hvtest" "--$check" >"$WORK/log" 2>&1; then
		echo "  MISSED  --$check passed a mutant: $meaning"
		missed=$(( missed + 1 ))
	else
		failed=$( grep -c '^  FAIL' "$WORK/log" )
		echo "  caught  --$check failed $failed check(s): $meaning"
		caught=$(( caught + 1 ))
	fi
done

echo "mutants: ${#MUTANTS[@]}, caught $caught, missed $missed"
[ "$missed" -eq 0 ]
