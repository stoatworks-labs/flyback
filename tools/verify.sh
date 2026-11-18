#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is FRESH and universal on purpose. `cmake -B build` on an existing
# tree re-uses its cache, and the cache is where the architecture list lives: a
# tree once configured arm64-only for a fast iteration loop rebuilds happily
# into a single-architecture bundle, and the build log calls that a success.
# So the verify tree is deleted first, and the architecture is checked with
# lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]        (default build-verify)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"
cd "$REPO"

# oxbow, the FFGL test host. It sits beside this repo's checkout -- and from
# a git worktree `..` is the worktrees folder, not Projects/resolume, so the
# main checkout is found through git's common dir as well. OXBOW names the
# binary outright.
OXBOW_REPO=""
for candidate in "$REPO/../oxbow" \
                 "$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../oxbow"; do
	if [ -d "$candidate/build" ]; then
		OXBOW_REPO="$candidate"
		break
	fi
done
OXBOW_REPO="${OXBOW_REPO:-$REPO/../oxbow}"

PASSED=0
step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }
pass() { printf 'ok   %s\n' "$1"; PASSED=$(( PASSED + 1 )); }

#---------------------------------------------------------------------------
# The GLSL reserved words, as identifiers. glslc catches these too, but glslc
# is optional and a machine without it skips the whole shader step -- so the
# one trap most likely to be walked into gets its own grep that always runs.
# (`half` was walked into during this build, as a local in the display pass.)
#---------------------------------------------------------------------------
reserved_words() {
	local words="patch sample input output filter common active half layout flat smooth noperspective"
	local bad=0 word
	for word in $words; do
		if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)]" \
		            source/render/Shaders.cpp source/render/Lamp.cpp >/dev/null 2>&1; then
			printf '   "%s" is declared as an identifier and is a GLSL reserved word\n' "$word"
			bad=$(( bad + 1 ))
		fi
	done
	return "$bad"
}

step "GLSL reserved words"
reserved_words || fail "a GLSL reserved word is used as an identifier"
pass "none of the reserved words is an identifier"

step "Shaders"
# The same script CI runs (tools/glslc.sh): one copy of the extraction.
tools/glslc.sh || fail "a shader does not compile"
pass "every shader compiles"

step "Browser demo's shaders"
# demo/plugin.js carries copies of source/render/Shaders.cpp for the page at
# flyback-demo.stoatworks-labs.com. A copy that drifts still renders a plausible
# picture, so the drift has to fail here instead. It checks the shaders only:
# demo/engine.js is a hand port of the engine that nothing but a reader checks.
if [ -f demo/tools/check_shaders.py ]; then
	log="$( mktemp )"
	if python3 demo/tools/check_shaders.py >"$log" 2>&1; then
		pass "$( tail -1 "$log" )"
	else
		tail -14 "$log"
		fail "the demo's shaders have drifted from source/render/Shaders.cpp"
	fi
	rm -f "$log"
else
	printf '   skipped: no demo/\n'
fi

step "Submodule"
[[ -f external/ffgl/CMakeLists.txt ]] || fail "FFGL SDK missing -- run: git submodule update --init --recursive"
pin="$(git -C external/ffgl rev-parse --short=7 HEAD)"
[[ "$pin" == "b1afaf9" ]] || fail "FFGL SDK at $pin, not the fleet's b1afaf9"
pass "FFGL SDK pinned at $pin"

step "earshot snapshot"
# external/earshot is `git archive` of one commit, unmodified (SNAPSHOT says
# which, and whether anything was patched: nothing). Where the earshot checkout
# is on this machine, the snapshot is compared with that commit file for file
# (eidophone's step).
snap="$( sed -n 's/^Commit:[[:space:]]*\([0-9a-f]\{7,\}\).*/\1/p' external/earshot/SNAPSHOT )"
[[ -n "$snap" ]] || fail "external/earshot/SNAPSHOT names no commit"
grep -q "$snap" AGENTS.md || fail "AGENTS.md does not record the earshot commit $snap"
EARSHOT="${EARSHOT:-$HOME/Projects/resolume/earshot}"
if git -C "$EARSHOT" cat-file -e "$snap^{commit}" 2>/dev/null; then
	tmp="$( mktemp -d )"
	git -C "$EARSHOT" archive "$snap" | tar -x -C "$tmp"
	diff -r -x SNAPSHOT "$tmp" external/earshot >/dev/null || { diff -rq -x SNAPSHOT "$tmp" external/earshot | head; rm -rf "$tmp"; fail "external/earshot differs from earshot $snap"; }
	rm -rf "$tmp"
	pass "external/earshot is earshot $snap, file for file"
else
	pass "SNAPSHOT and AGENTS.md agree on $snap (no earshot checkout at $EARSHOT to compare the files with)"
fi

step "Build (fresh, universal, Release)"
rm -rf "$BUILD"
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >/dev/null
pass "configured from nothing and built"

step "Bundles"
OXBOW="${OXBOW:-$OXBOW_REPO/build/oxbow}"
declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h \
	|| fail "StoatworksAbout.h's versionFallback is not v$declared"

check_bundle() {
	local name="$1" identifier="$2" id="$3" display="$4" type="$5"
	local bundle="$BUILD/$name.bundle"
	local binary="$bundle/Contents/MacOS/$name"
	[[ -f "$binary" ]] || fail "no binary at $binary"

	# lipo, never the build log.
	local arches
	arches="$( lipo -archs "$binary" )"
	[[ "$arches" == *arm64* ]]  || fail "$name: no arm64 slice (got: $arches)"
	[[ "$arches" == *x86_64* ]] || fail "$name: no x86_64 slice (got: $arches)"

	# Captured, then matched from a herestring -- never `nm | grep -q`, which
	# under pipefail fails BECAUSE the symbol was found (grep exits, nm takes
	# SIGPIPE).
	local symbols
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	grep -q '_plugMain' <<<"$symbols" || fail "$name: plugMain not exported"
	pass "$name: $arches, plugMain exported"

	local plist="$bundle/Contents/Info.plist"
	read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || true; }
	[[ "$( read_plist CFBundleIdentifier )" == "$identifier" ]] || fail "$name: bundle id is '$( read_plist CFBundleIdentifier )'"
	[[ "$( read_plist CFBundleExecutable )" == "$name" ]] || fail "$name: CFBundleExecutable is '$( read_plist CFBundleExecutable )'"
	[[ "$( read_plist CFBundlePackageType )" == "BNDL" ]] || fail "$name: CFBundlePackageType is not BNDL"
	[[ "$( read_plist CFBundleVersion )" == "$declared" ]] || fail "$name: plist version != CMakeLists '$declared'"
	pass "$name: $identifier, BNDL, v$declared -- plist, CMakeLists and About agree"

	# Ad hoc, on a copy: proves the bundle is well enough formed to sign.
	local signdir
	signdir="$( mktemp -d )"
	cp -R "$bundle" "$signdir/"
	codesign --force --sign - --timestamp=none "$signdir/$name.bundle" >/dev/null 2>&1 || fail "$name: could not be ad-hoc signed"
	codesign --verify --deep --strict "$signdir/$name.bundle" >/dev/null 2>&1 || fail "$name: ad-hoc signature does not verify"
	rm -rf "$signdir"
	pass "$name: ad-hoc signed and verified"

	# What a host reads. The name field is 16 bytes, NOT null-terminated: a
	# long name is truncated silently, and only something that reads the
	# bundle the way a host does would notice.
	if [[ -x "$OXBOW" ]]; then
		local probe
		probe="$( "$OXBOW" probe "$bundle" 2>&1 || true )"
		printf '%s\n' "$probe" | sed -n '1,6p' | sed 's/^/   /'
		grep -qE "^name:[[:space:]]+$display\$" <<<"$probe" || fail "$name: oxbow did not read the name '$display'"
		grep -qE "^id:[[:space:]]+$id\$" <<<"$probe" || fail "$name: oxbow did not read the id $id"
		grep -qE "^type:[[:space:]]+$type\$" <<<"$probe" || fail "$name: oxbow did not read the type $type"
		pass "$name: a host reads $id / $display / $type"
		local self
		self="$( "$OXBOW" selftest "$bundle" 2>&1 || true )"
		grep -q 'selftest:[[:space:]]*PASS' <<<"$self" || { printf '%s\n' "$self" | tail -6; fail "$name: oxbow selftest did not pass"; }
		pass "$name: oxbow selftest instantiates it through the host path and renders ($( grep -E '^lit pixels' <<<"$self" | sed 's/^lit pixels: *//' ))"
	else
		echo "   skipped: no oxbow at $OXBOW (set OXBOW=...)"
	fi
}
check_bundle "Flyback" "com.stoatworks.ffgl.flyback" "HV01" "SW Flyback" "source"
check_bundle "Flyback Over" "com.stoatworks.ffgl.flyback.over" "HV02" "SW Flyback Over" "effect"

step "CoreAudio, never, with the singing mode off"
# Pitch Source is Off by default, and Off must not reach CoreAudio at all: no
# earshot engine, no device list, no select (listDevices and selectDevice both
# start CoreAudio). libcatrap (tools/catrap) logs and refuses every CoreAudio
# dlopen and every direct HAL or component call; the mode-off path runs under
# it -- a host loading both bundles and rendering them (oxbow probe and
# selftest), and hvtest's --off, --defaults, --list and a film of each machine
# -- and the log must show the trap loaded in each process and no HIT. The
# negative control is `hvtest --trap-probe`, which reaches CoreAudio three
# ways under the same trap and must leave HITs. HVTEST_REAL_DEVICES=1 keeps
# hvtest's scripted backend out of it: anything that started would go to
# miniaudio, and so into the trap. No FLYBACK_NO_DEVICES here: the gate would
# hide what this looks for. (Run from bash, never through perl or env: a SIP
# binary strips DYLD_*.)
TRAP="$BUILD/libcatrap.dylib"
[[ -f "$TRAP" ]] || fail "no $TRAP"
traplog="$( mktemp )"
trapped() { CATRAP_LOG="$traplog" HVTEST_REAL_DEVICES=1 DYLD_INSERT_LIBRARIES="$TRAP" "$@"; }
runs=0
if [[ -x "$OXBOW" ]]; then
	for name in "Flyback" "Flyback Over"; do
		trapped "$OXBOW" probe "$BUILD/$name.bundle" >/dev/null 2>&1 || fail "oxbow probe under the trap: $name"
		self="$( trapped "$OXBOW" selftest "$BUILD/$name.bundle" 2>&1 || true )"
		grep -q 'selftest:[[:space:]]*PASS' <<<"$self" || fail "oxbow selftest under the trap: $name"
		runs=$(( runs + 2 ))
	done
else
	fail "no oxbow at $OXBOW: the trap needs a host (set OXBOW=...)"
fi
for check in off defaults; do
	trapped "$BUILD/hvtest" "--$check" >/dev/null 2>&1 || fail "hvtest --$check under the trap"
	runs=$(( runs + 1 ))
done
trapped "$BUILD/hvtest" --list >/dev/null 2>&1 || fail "hvtest --list under the trap"
runs=$(( runs + 1 ))
for machine in 0 1 2 3 4; do
	trapped "$BUILD/hvtest" --film 30 --size 160x90 --machine "$machine" >/dev/null 2>&1 || fail "hvtest --film (machine $machine) under the trap"
	runs=$(( runs + 1 ))
done
head -c $(( 160 * 90 * 4 * 30 )) /dev/zero | trapped "$BUILD/hvtest" --pipe --effect --size 160x90 >/dev/null 2>&1 || fail "hvtest --pipe --effect under the trap"
runs=$(( runs + 1 ))
loaded=$( grep -c '^loaded ' "$traplog" || true )
hits=$( grep -c '^HIT ' "$traplog" || true )
[[ "$loaded" -eq "$runs" ]] || { cat "$traplog"; fail "the trap loaded in $loaded of $runs processes"; }
[[ "$hits" -eq 0 ]] || { grep '^HIT ' "$traplog" | sort | uniq -c; fail "the mode-off path reached CoreAudio $hits time(s)"; }
pass "mode off: $runs processes (oxbow probe and selftest on both bundles, hvtest --off --defaults --list, a film of each machine, the effect's pipe), the trap loaded in all, 0 CoreAudio calls"
: >"$traplog"
probe="$( trapped "$BUILD/hvtest" --trap-probe 2>&1 )" || { printf '%s\n' "$probe"; fail "hvtest --trap-probe did not run under the trap"; }
for what in "dlopen /System/Library/Frameworks/CoreAudio.framework" "AudioObjectGetPropertyDataSize"; do
	grep -q "^HIT .*$what" "$traplog" || { cat "$traplog"; fail "the negative control reached CoreAudio and the trap did not log '$what'"; }
done
pass "negative control: hvtest --trap-probe reaches CoreAudio and the trap logs $( grep -c '^HIT ' "$traplog" ) HITs (listDevices' dlopen, a direct HAL call, an eager pitch input's dlopen)"
set +e
"$BUILD/hvtest" --trap-probe >/dev/null 2>&1
status=$?
set -e
[[ "$status" -eq 2 ]] || fail "hvtest --trap-probe without the trap gave exit $status, not 2 (it must refuse)"
pass "without the trap, --trap-probe refuses (exit 2) rather than ask the OS for devices"
rm -f "$traplog"

step "Checks"
# Every claim the README makes, in the order it makes them. Physics first
# (engine only, no GPU), then the pixel checks at their development rasters.
run_check() {
	local log
	log="$( mktemp )"
	if "$BUILD/hvtest" "$@" >"$log" 2>&1; then
		grep -E '^  (ok|FAIL) ' "$log" | sed 's/^/ /'
		pass "hvtest $*"
	else
		cat "$log"
		fail "hvtest $*"
	fi
	rm -f "$log"
}
for check in laplace dimension ladder tesla vdg kirchhoff light exposure determinism over onset defaults names state \
             sing sing-clock sing-memory device sing-picture off lamp resize-sing; do
	run_check "--$check"
done

step "Pixel checks at 320x180 (CI's raster)"
# Every check that touches a pixel or the GPU, again at 320x180, with its
# negative controls there too. The physics checks have no raster to change.
for check in light exposure determinism over onset state sing-picture off lamp resize-sing; do
	run_check "--$check" --size 320x180
done

step "The singing coil's pixel checks on Apple's software renderer"
# A second rasteriser (BRIEF-ADDENDUM), at both rasters. On it --off compares
# the engine's frames bit for bit and reports the pixels without claiming them:
# that renderer does not reproduce its own output run to run.
for size in "" "320x180"; do
	for check in sing-picture off lamp resize-sing; do
		if [ -n "$size" ]; then
			HVTEST_RENDERER=software run_check "--$check" --size "$size"
		else
			HVTEST_RENDERER=software run_check "--$check"
		fi
	done
done

step "Negative controls"
# Every check, against a deliberately wrong model, must FAIL. A check that
# cannot fail is not a check. Once at the development rasters, once at 320x180.
for size in "" "320x180"; do
	log="$( mktemp )"
	if [ -n "$size" ]; then set -- --size "$size"; else set --; fi
	"$BUILD/hvtest" --negative "$@" >"$log" 2>&1 || { grep -E 'negative control|PASSED against|undetected' "$log"; fail "a negative control went undetected${size:+ at $size}"; }
	grep -E 'wrong models' "$log" | sed 's/^/ /'
	rm -f "$log"
	pass "every wrong model is caught${size:+ at $size}"
done
set --

step "Offline (what CI runs without a GL context)"
# CI's own selector, run here too so it cannot rot: every check the one table
# in hvtest's main() marks as needing no GL, and their negative controls.
log="$( mktemp )"
"$BUILD/hvtest" --offline >"$log" 2>&1 || { cat "$log"; fail "hvtest --offline"; }
grep -E '^  ran:|NOT run|^  ##   --' "$log" | sed 's/^/ /'
rm -f "$log"
pass "hvtest --offline"

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to a take. Through the effect (--effect), the only
# path that reads input frames.
#---------------------------------------------------------------------------
step "pipe"
HVTEST="$BUILD/hvtest"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
set +e
got=$( "$HVTEST" --pipe --effect --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' '; exit "${PIPESTATUS[0]}" )
status=$?
set -e
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever hvtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
set +e
"$HVTEST" --pipe --effect --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
set -e
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141.
head -c $(( frame * 20 )) /dev/zero > "$raw"
set +e
"$HVTEST" --pipe --effect --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the run with exit 1, not SIGPIPE"
else
	fail "a closed stdout gave exit $status, not 1"
fi
# And the source's --film, which reads nothing and writes until told to stop.
set +e
"$HVTEST" --film 20 --size 64x36 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
if [ "$status" -eq 1 ]; then
	pass "--film into a closed stdout ends with exit 1, not SIGPIPE"
else
	fail "--film into a closed stdout gave exit $status, not 1"
fi
# The same, singing: a tone through earshot, the Tesla's interrupter at its
# pitch, the lamp drawn. 2.5 frames in, 2 out; a hung-up reader is exit 1.
sing=( --tone 220 --set "Machine=1" --set "Pitch Source=System default" )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
set +e
got=$( "$HVTEST" --pipe --effect --size 64x36 "${sing[@]}" < "$raw" 2>/dev/null | wc -c | tr -d ' '; exit "${PIPESTATUS[0]}" )
status=$?
set -e
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "singing: 2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "singing: 2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
head -c $(( frame * 20 )) /dev/zero > "$raw"
set +e
"$HVTEST" --pipe --effect --size 64x36 "${sing[@]}" < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
[ "$status" -eq 1 ] || fail "singing: a closed stdout gave exit $status, not 1"
set +e
"$HVTEST" --film 20 --size 64x36 "${sing[@]}" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
[ "$status" -eq 1 ] || fail "singing: --film into a closed stdout gave exit $status, not 1"
pass "singing: a closed stdout ends --pipe and --film with exit 1, not SIGPIPE"
set +e
"$HVTEST" --film 2 --size 64x36 --set "Pitch Source=Nowhere" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail "--set naming no option of Pitch Source gave exit $status, not 2"
pass "--set refuses an option name that is not one (exit 2)"
rm -f "$raw" "$cues"

step "Dead controls"
python3 tools/sweep.py --build "$BUILD" || fail "a control is dead or barely alive"
pass "every control changes the picture where it applies"

step "Cost"
# 720p and 1080p (a shared machine: no 4K loops). The machines on their
# defaults, the singing mode off; then the Tesla singing a 220 Hz tone.
"$BUILD/hvtest" --bench --sizes 720,1080
"$BUILD/hvtest" --bench --sizes 720,1080 --machine 1 --tone 220 --set "Pitch Source=System default"

printf '\n\033[32mall green: %d steps passed\033[0m\n' "$PASSED"
