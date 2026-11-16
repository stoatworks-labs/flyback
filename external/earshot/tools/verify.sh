#!/bin/bash
# Everything earshot claims, in one run. Exit status 0 only if every step
# that ran passed; a step that cannot run here says SKIP, never PASS.
#
#   tools/verify.sh             the lot (about ten minutes)
#   tools/verify.sh --quick     no negative controls, no mutation test
#
# Steps: a universal build and a lipo check; estest natively (arm64) and
# under Rosetta (x86_64); the negative controls; an arm64 build with
# -ffp-contract=off (no fused multiply-add: closer to x86_64 and MSVC
# arithmetic) running estest; a build without capture (EARSHOT_CAPTURE=OFF);
# the consumer (earshot by add_subdirectory into a MODULE, loaded by a host,
# universal, exporting nothing of earshot's); a WAV round trip; the tempo on
# pattern's release-video soundtrack if that file is on this Mac; and the
# mutation test. No step opens an audio device (no microphone prompt):
# live capture is `estest --live`, by hand.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
OUT="$ROOT/build-verify"
LOG="$OUT/logs"
mkdir -p "$LOG"
JOBS=4

results=()
failed=0
record() {   # step status detail
	results+=("$(printf '%-34s %-5s %s' "$1" "$2" "$3")")
	[ "$2" = "FAIL" ] && failed=1
	printf '%-34s %-5s %s\n' "$1" "$2" "$3"
}
last() { grep -E "$2" "$1" | tail -1; }

echo "earshot verify: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo ' (uncommitted changes)')"
echo

#-- 1. Universal build, lipo ------------------------------------------------
if cmake -S "$ROOT" -B "$OUT/universal" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >"$LOG/universal-configure.log" 2>&1 &&
   cmake --build "$OUT/universal" -j$JOBS >"$LOG/universal-build.log" 2>&1; then
	warnings=$(grep -c "warning:" "$LOG/universal-build.log")
	record "universal build" PASS "arm64 + x86_64, Release, $warnings compiler warnings"
else
	record "universal build" FAIL "see $LOG/universal-build.log"
fi
ESTEST="$OUT/universal/tools/estest/estest"
LIB="$OUT/universal/libearshot.a"
lipoOk=1
for f in "$LIB" "$ESTEST"; do
	archs=$(lipo -archs "$f" 2>/dev/null)
	case "$archs" in *arm64*x86_64*|*x86_64*arm64*) ;; *) lipoOk=0 ;; esac
done
[ $lipoOk -eq 1 ] && record "lipo" PASS "libearshot.a and estest: $(lipo -archs "$LIB")" || record "lipo" FAIL "$(lipo -archs "$LIB" 2>&1)"

#-- 2. estest, arm64 and x86_64 -----------------------------------------------
if [ -x "$ESTEST" ]; then
	"$ESTEST" >"$LOG/estest-arm64.log" 2>&1 && s=PASS || s=FAIL
	record "estest (arm64)" $s "$(last "$LOG/estest-arm64.log" '^estest:')"
	if arch -x86_64 /usr/bin/true 2>/dev/null; then
		arch -x86_64 "$ESTEST" >"$LOG/estest-x86_64.log" 2>&1 && s=PASS || s=FAIL
		record "estest (x86_64, Rosetta)" $s "$(last "$LOG/estest-x86_64.log" '^estest:')"
	else
		record "estest (x86_64, Rosetta)" SKIP "Rosetta is not installed"
	fi
	if [ $QUICK -eq 0 ]; then
		"$ESTEST" --negative >"$LOG/negative.log" 2>&1 && s=PASS || s=FAIL
		record "negative controls" $s "$(last "$LOG/negative.log" '^negative:'); $(last "$LOG/negative.log" '^coverage:')"
	else
		record "negative controls" SKIP "--quick"
	fi
	"$ESTEST" --write-wav "$OUT/roundtrip.wav" >"$LOG/wav.log" 2>&1 && s=PASS || s=FAIL
	record "WAV round trip" $s "$(last "$LOG/wav.log" 'wav.roundtrip' | sed 's/^[a-zA-Z]* *//')"
	SOUNDTRACK="${EARSHOT_SOUNDTRACK:-$HOME/Projects/infrastructure/stoatworks-backend/video/projects/pattern/footage/soundtrack.wav}"
	if [ -f "$SOUNDTRACK" ]; then
		for fps in 30 60; do
			"$ESTEST" --file "$SOUNDTRACK" --expect-bpm 125 --from 6 --fps $fps >"$LOG/soundtrack-$fps.log" 2>&1 && s=PASS || s=FAIL
			record "soundtrack tempo, $fps fps" $s "$(last "$LOG/soundtrack-$fps.log" 'file.tempo' | sed 's/^[a-zA-Z]* *file.tempo: //')"
		done
	else
		record "soundtrack tempo" SKIP "pattern's soundtrack.wav is not on this Mac (EARSHOT_SOUNDTRACK=path)"
	fi
else
	record "estest" FAIL "not built"
fi

#-- 3. No fused multiply-add ------------------------------------------------
if cmake -S "$ROOT" -B "$OUT/nofma" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_CXX_FLAGS=-ffp-contract=off \
		>"$LOG/nofma-configure.log" 2>&1 && cmake --build "$OUT/nofma" -j$JOBS >"$LOG/nofma-build.log" 2>&1; then
	"$OUT/nofma/tools/estest/estest" >"$LOG/estest-nofma.log" 2>&1 && s=PASS || s=FAIL
	record "estest (-ffp-contract=off)" $s "$(last "$LOG/estest-nofma.log" '^estest:')"
else
	record "estest (-ffp-contract=off)" FAIL "build failed: $LOG/nofma-build.log"
fi

#-- 4. Without capture --------------------------------------------------------
if cmake -S "$ROOT" -B "$OUT/nocapture" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DEARSHOT_CAPTURE=OFF \
		>"$LOG/nocapture-configure.log" 2>&1 && cmake --build "$OUT/nocapture" -j$JOBS >"$LOG/nocapture-build.log" 2>&1; then
	"$OUT/nocapture/tools/estest/estest" --fft --silent --fallback --params >"$LOG/estest-nocapture.log" 2>&1 && s=PASS || s=FAIL
	record "EARSHOT_CAPTURE=OFF" $s "builds; $(last "$LOG/estest-nocapture.log" '^estest:')"
else
	record "EARSHOT_CAPTURE=OFF" FAIL "build failed: $LOG/nocapture-build.log"
fi

#-- 5. The consumer: add_subdirectory into a MODULE ------------------------
if cmake -S "$ROOT/tools/consumer" -B "$OUT/consumer" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
		>"$LOG/consumer-configure.log" 2>&1 && cmake --build "$OUT/consumer" -j$JOBS >"$LOG/consumer-build.log" 2>&1; then
	MOD="$(ls "$OUT/consumer"/libFakePlugin.* 2>/dev/null | head -1)"
	exports="$(nm -gU "$MOD" 2>/dev/null | awk '{print $3}' | sort -u | tr '\n' ' ')"
	archs="$(lipo -archs "$MOD" 2>/dev/null)"
	"$OUT/consumer/consumer-host" >"$LOG/consumer-arm64.log" 2>&1 && a=ok || a=FAIL
	if arch -x86_64 /usr/bin/true 2>/dev/null; then
		arch -x86_64 "$OUT/consumer/consumer-host" >"$LOG/consumer-x86_64.log" 2>&1 && x=ok || x=FAIL
	else
		x=skip
	fi
	if [ "$exports" = "_fakePluginPitch " ] && [ $a = ok ] && [ $x != FAIL ]; then s=PASS; else s=FAIL; fi
	record "consumer (MODULE)" $s "[$archs] exports: ${exports}; loaded and pitched 440 Hz: arm64 $a, x86_64 $x"
else
	record "consumer (MODULE)" FAIL "build failed: $LOG/consumer-build.log"
fi

#-- 6. Mutation test ----------------------------------------------------------
if [ $QUICK -eq 0 ]; then
	"$ROOT/tools/mutate.sh" >"$LOG/mutate.log" 2>&1 && s=PASS || s=FAIL
	record "mutation test" $s "$(last "$LOG/mutate.log" '^mutate:')"
else
	record "mutation test" SKIP "--quick"
fi

echo
echo "== earshot verify summary =="
printf '%s\n' "${results[@]}"
echo "logs: $LOG"
[ $failed -eq 0 ] && echo "verify: PASS" || echo "verify: FAIL"
exit $failed
