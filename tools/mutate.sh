#!/usr/bin/env bash
#
# Mutation-test the harness: change ONE character in the shipped engine or
# GLSL, rebuild, and require the check that should notice to fail. A harness
# that passes a mutated plugin is measuring something other than the plugin.
#
# Not part of verify.sh (each mutant is an arm64 rebuild of hvtest, a few minutes in all); run it
# when a check or the code it guards changes. Each mutant is built in its own
# copy of the tree, so the working tree is never touched, and every build is
# from nothing (make's one-second mtimes cannot keep a stale object).
#
# Each mutant's check runs under a time limit (MUTANT_SECONDS, default 300):
# a mutant can hang a check (an inverted loop test never exits), and a hang is
# the check noticing. A timeout is counted as caught, and said so.
#
#     tools/mutate.sh            every mutant
#     tools/mutate.sh sing       only those whose check starts with "sing"
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$( mktemp -d )"
trap 'rm -rf "$WORK"' EXIT
LIMIT="${MUTANT_SECONDS:-300}"
ONLY="${1:-}"

# file ^ the exact text ^ its one-character mutant ^ the check (and its flags) that must fail ^ what it means
# The separator is ^, not |: C++ has || and a | table splits on it.
MUTANTS=(
	"source/render/Shaders.cpp^return 0.5 * ( 1.0 + erfAS( x * 0.70710678118654752 ) );^return 0.6 * ( 1.0 + erfAS( x * 0.70710678118654752 ) );^light^GLSL: the normal CDF 20% high"
	"source/render/Shaders.cpp^light = ( 1.0 - Glow ) * e + Glow * g;^light = ( 1.0 + Glow ) * e + Glow * g;^light^GLSL: the core adds the glow's share instead of giving it up"
	"source/engine/Physics.cpp^const double beta  = 2.0 * arc.B * v + 4.0 * supply.sourceOhms * arc.D;^const double beta  = 2.0 * arc.B * v + 5.0 * supply.sourceOhms * arc.D;^ladder^engine: L*'s linear coefficient"
	"source/engine/Physics.cpp^const Charge gb = { -last.q * b / r, s - b * b / r };^const Charge gb = { -last.q * b / r, s + b * b / r };^vdg^engine: an image charge on the wrong side of the grounded sphere"
	"source/engine/Lattice.cpp^yr[ i ] = -mr[ i ] * ( xr[ i - 1 ] + xr[ i + 1 ] + xr[ i - nx ] + xr[ i + nx ] - 4.0f * xr[ i ] );^yr[ i ] = -mr[ i ] * ( xr[ i - 1 ] + xr[ i + 1 ] + xr[ i - nx ] + xr[ i + nx ] - 5.0f * xr[ i ] );^laplace^engine: the Laplacian's centre weight"
	"source/engine/Dbm.cpp^const double field = ( 1.0 - phi ) / kBond[ bestK ];^const double field = ( 1.0 + phi ) / kBond[ bestK ];^dimension^engine: growth toward the potential instead of down it"
	"source/engine/Engine.cpp^next  = anchor + static_cast< double >( bangIndex + 1 ) / s.bps;^next  = anchor + static_cast< double >( bangIndex + 2 ) / s.bps;^tesla^engine: the interrupter skips a beat"
	"source/engine/Physics.cpp^return 24.0 * kPi * kEpsilon0 * kPositiveIonMobility * a * onsetVolts / ( b * b );^return 25.0 * kPi * kEpsilon0 * kPositiveIonMobility * a * onsetVolts / ( b * b );^vdg^engine: the corona conductance 4% high"
	"source/engine/Engine.cpp^v = ( volts + dt / C * ( I + G * Vc ) ) / ( 1.0 + dt * G / C );^v = ( volts + dt / C * ( I - G * Vc ) ) / ( 1.0 + dt * G / C );^vdg^engine: the corona drains from zero volts, not from its onset"
	# The singing coil.
	"source/engine/Engine.cpp^const double end = phase + r * ( b - a );^const double end = phase - r * ( b - a );^sing^engine: the sung phase runs backwards"
	"source/engine/Engine.cpp^sung.push_back( std::min( b, a + ( k - phase ) / r ) );^sung.push_back( std::min( b, a + ( k + phase ) / r ) );^sing-clock^engine: a sung bang placed by the phase's wrong sign"
	"source/engine/Engine.cpp^phase = 0.0;// a rest^phase = 1.0;// a rest^sing^engine: a rest leaves the next note's first bang due at once"
	"source/Singing.cpp^r *= 0.5;^r *= 0.6;^sing^pitch: folding down by 0.6, not an octave"
	"source/Singing.cpp^static_cast< double >( f.pitch.levelDb ) >= gateDb;^static_cast< double >( f.pitch.levelDb ) <= gateDb;^sing^pitch: the Gate the wrong way round"
	"source/Singing.cpp^else if( ( !above && !( hooks & kHookKeepNote ) ) || ( hooks & kHookVoicedOnly ) )^else if( ( above && !( hooks & kHookKeepNote ) ) || ( hooks & kHookVoicedOnly ) )^sing^pitch: sound above the Gate forgets the note (no sustain)"
	"source/Singing.cpp^h *= 16777619u;^h *= 16777618u;^device^pitch: the menu's key is not earshot's deviceKey"
	"source/Singing.h^return applied > 0.0f;^return applied >= 0.0f;^off^pitch: Off counts as on"
	"source/render/Lamp.cpp^float lit   = clamp( Radius - d + 0.5, 0.0, 1.0 );^float lit   = clamp( Radius - d + 0.5, 0.0, 0.0 );^lamp --size 320x180^GLSL: the lamp's disc never lit"
)

caught=0
missed=0
ran=0
for entry in "${MUTANTS[@]}"; do
	IFS='^' read -r file original mutant check meaning <<<"$entry"
	if [ -n "$ONLY" ] && [[ "$check" != "$ONLY"* ]]; then
		continue
	fi
	ran=$(( ran + 1 ))
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree"
	# Everything but the builds; the SDK and the earshot snapshot by link.
	( cd "$REPO" && tar cf - --exclude='./build*' --exclude='./external' --exclude='./.git' --exclude='./.claude' . ) | ( cd "$tree" && tar xf - )
	mkdir -p "$tree/external"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
	ln -s "$REPO/external/earshot" "$tree/external/earshot"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY' || { echo "  MUTANT NOT APPLIED: $meaning"; missed=$(( missed + 1 )); continue; }
import sys
path, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if text.count(a) != 1:
    print(f"the original occurs {text.count(a)} times in {path}", file=sys.stderr)
    sys.exit(1)
def one_edit(a, b):
    # One character substituted, inserted or deleted (an edit distance of 1).
    if len(a) == len(b):
        return sum(1 for x, y in zip(a, b) if x != y) == 1
    if abs(len(a) - len(b)) != 1:
        return False
    short, long_ = (a, b) if len(a) < len(b) else (b, a)
    return any(long_[:i] + long_[i + 1:] == short for i in range(len(long_)))
if not one_edit(a, b):
    print("not a one-character mutant", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(text.replace(a, b))
PY

	if ! cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null 2>&1 \
	   || ! cmake --build "$tree/build" --target hvtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1; then
		echo "  (did not build: $meaning)"
		missed=$(( missed + 1 ))
		continue
	fi
	# perl's alarm survives exec, and SIGALRM's default action ends the check:
	# exit 142 is the time limit. (No DYLD_* needed here, so perl being a SIP
	# binary that strips them does not matter.)
	# shellcheck disable=SC2086
	perl -e 'alarm shift; exec @ARGV' "$LIMIT" "$tree/build/hvtest" --$check >"$WORK/log" 2>&1
	status=$?
	if [ "$status" -eq 0 ]; then
		echo "  MISSED  --$check passed a mutant: $meaning"
		missed=$(( missed + 1 ))
	elif [ "$status" -eq 142 ]; then
		echo "  caught  --$check did not finish in ${LIMIT} s (counted as caught): $meaning"
		caught=$(( caught + 1 ))
	else
		failed=$( grep -c '^  FAIL' "$WORK/log" )
		echo "  caught  --$check failed $failed check(s): $meaning"
		caught=$(( caught + 1 ))
	fi
done

echo "mutants: $ran, caught $caught, missed $missed"
[ "$missed" -eq 0 ]
