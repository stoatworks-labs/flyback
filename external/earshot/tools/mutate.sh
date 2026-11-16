#!/bin/bash
# The mutation test: plant one bug at a time in a scratch copy of the source,
# rebuild, and require estest to fail. A surviving mutant is a claim the
# harness does not actually check.
#
# The Mutations flags in include/earshot/testing.h are the negative controls
# (estest --negative); this is the other half: edits the harness has never
# heard of, written as the bugs they model.
#
#   tools/mutate.sh            every mutant
#   tools/mutate.sh 3 7        just those (1-based)
#
# Arm64 only (it is an iteration build). Never touches the working tree.
#
# Retired: kAgreeCents = 5000 (pitch pass two believed however far it
# jumps) survived every signal estest has, residue tones with every partial
# above pass two's band included; the guard stays as defence and is not
# claimed (AGENTS.md).

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/earshot-mutate.$$"
trap 'rm -rf "$WORK"' EXIT

# file | from | to | groups (estest options) | the bug it models
MUTANTS=(
"src/Pitch.cpp|const double delta = 0.5 * ( a - c ) / d;|const double delta = -0.5 * ( a - c ) / d;|--pitch|NSDF peak interpolated the wrong way"
"src/Partials.cpp|delta = 0.5 * ( a - c ) / d;|delta = 0.25 * ( a - c ) / d;|--partials|QIFFT offset halved"
"src/Onsets.cpp|s.meanFlux + (double)config.onsetDeviations * s.devFlux|s.meanFlux + 0.25 * (double)config.onsetDeviations * s.devFlux|--onset-prime --onset-attack|onset threshold a quarter of what it should be"
"src/Onsets.cpp|constexpr double kLogKneeDb      = -110.0;|constexpr double kLogKneeDb      = -60.0;|--onset-attack --onset-prime|log-flux knee too high: a swell reads as a landslide"
"src/ThirdOctave.cpp|constexpr double kFastSeconds = 0.125;|constexpr double kFastSeconds = 0.0125;|--thirds|time weighting ten times too fast"
"src/ThirdOctave.cpp|315.0f, 400.0f|300.0f, 400.0f|--thirds|an ISO 266 label wrong"
"src/Tempo.cpp|constexpr double kLevelRatio   = 0.70;|constexpr double kLevelRatio   = 0.99;|--tempo|metre level picks the strongest member, not the fastest"
"src/Tempo.cpp|constexpr int    kMinOnsets    = 6;|constexpr int    kMinOnsets    = 0;|--tempo|tempo read with no onsets at all"
"src/Tap.h|static constexpr int kZeroCrossings = 10;|static constexpr int kZeroCrossings = 2;|--tap|anti-alias kernel cut to two zero crossings"
"src/Stereo.cpp|( total - 2.0 * lr ) / ( 2.0 * total )|( total + 2.0 * lr ) / ( 2.0 * total )|--stereo|width sign flipped"
"src/Capture.h|static constexpr double kSilenceTimeout = 3.0;|static constexpr double kSilenceTimeout = 2.0;|--silent|Silent after 2 s instead of spasis's 3"
"src/Ring.h|claim >= start && claim - start <= capacity_;|claim >= start;|--ring|seqlock ignores a lap during the copy"
"src/HostFft.cpp|constexpr double kGapSeconds    = 1.0;|constexpr double kGapSeconds    = 5.0;|--fallback|a 2 s host-clock jump is not re-primed"
"src/Engine.cpp|mono[ (size_t)i ]  = 0.5f * ( left[ (size_t)i ] + right[ (size_t)i ] );|mono[ (size_t)i ]  = left[ (size_t)i ] + right[ (size_t)i ];|--thirds --bands|mono sum 6 dB hot"
"src/Engine.cpp|if( mut.configureClears && following )|if( following )|--params|configure() clears the ring (spasis's forbidden move)"
)

pick=("$@")
selected() {
	[ ${#pick[@]} -eq 0 ] && return 0
	for p in "${pick[@]}"; do [ "$p" = "$1" ] && return 0; done
	return 1
}

mkdir -p "$WORK"
rsync -a --exclude build --exclude 'build-*' --exclude .git "$ROOT/" "$WORK/src-tree/"
cmake -S "$WORK/src-tree" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null || exit 2
cmake --build "$WORK/build" -j4 >/dev/null || { echo "mutate: the unmutated copy does not build"; exit 2; }

killed=0; survived=0; total=0
for i in "${!MUTANTS[@]}"; do
	n=$((i + 1))
	selected "$n" || continue
	IFS='|' read -r file from to groups what <<<"${MUTANTS[$i]}"
	path="$WORK/src-tree/$file"
	cp "$path" "$path.orig"
	# make (macOS's 3.81) compares mtimes to the second: a file rewritten in
	# the second its object was built looks up to date, and the previous
	# mutant (or none) gets tested instead. Wait the second out, both ways.
	sleep 1.1
	if ! FROM="$from" TO="$to" python3 -c '
import os, sys
p = sys.argv[1]; s = open(p).read(); f = os.environ["FROM"]
if s.count(f) < 1: sys.exit(3)
open(p, "w").write(s.replace(f, os.environ["TO"], 1))' "$path"; then
		echo "  STALE     $n  $file: the text to mutate is gone (update tools/mutate.sh)"
		survived=$((survived + 1)); total=$((total + 1))
		continue
	fi
	total=$((total + 1))
	if ! cmake --build "$WORK/build" -j4 >/dev/null 2>&1; then
		echo "  killed    $n  $what (does not compile)"
		killed=$((killed + 1))
	else
		# shellcheck disable=SC2086
		out="$("$WORK/build/tools/estest/estest" $groups 2>&1)"
		status=$?
		[ -n "${MUTATE_VERBOSE:-}" ] && printf '%s\n' "$out" | grep -E '^  FAIL' | cut -c1-200
		if [ $status -ne 0 ]; then
			first="$(printf '%s\n' "$out" | grep -m1 '^  FAIL' | awk '{print $2}')"
			echo "  killed    $n  $what  (first failure: $first)"
			killed=$((killed + 1))
		else
			echo "  SURVIVED  $n  $what  ($file)"
			survived=$((survived + 1))
		fi
	fi
	sleep 1.1
	cat "$path.orig" >"$path"   # new mtime, so the next build recompiles it
	rm -f "$path.orig"
done
echo
echo "mutate: $killed of $total mutants killed"
[ "$survived" -eq 0 ]
