#!/usr/bin/env bash
#
# Datamosh diagnostics collector — macOS.
#
# Answers, without you needing to know what matters: are the plugin files where
# Resolume looks, are they loadable, did Resolume look, did Resolume load them,
# and did the parameters register.
#
#   bash collect-datamosh-diagnostics.sh | tee ~/Desktop/datamosh-diag.txt
#
# Then paste the file. Add --redact-user to replace your username with <user>.
#
# STRICTLY READ-ONLY. It never clears quarantine, never signs anything, never
# writes inside Resolume or its plugin folders, and only ever issues HTTP GETs.
# It cannot fix anything — that is deliberate, so it is safe to run at any point
# and its output describes the machine as it actually is.
#
# Every section is independent: a missing tool or folder prints a SKIPPED line
# and the run continues. The script always exits 0.

SCRIPT_VERSION="1.0.0"

REDACT=0
for arg in "$@"; do
	case "$arg" in
		--redact-user) REDACT=1 ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 0 ;;
	esac
done

# ---------------------------------------------------------------- plumbing --

section()     { printf '\n===== [%s] %s =====\n' "$1" "$2"; }
section_end() { printf '===== [%s] END =====\n' "$1"; }
skip()        { printf '[SECTION %s: SKIPPED — %s]\n' "$1" "$2"; }
note()        { printf '  NOTE: %s\n' "$1"; }
finding()     { printf '  >>> %s\n' "$1"; }

# timeout(1) is not present on stock macOS, so bound commands by hand.
run_bounded() {
	local limit="$1"; shift
	"$@" &
	local pid=$!
	local waited=0
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$waited" -ge "$limit" ]; then
			kill -9 "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null
			echo "  (timed out after ${limit}s: $*)"
			return 1
		fi
		sleep 1
		waited=$(( waited + 1 ))
	done
	wait "$pid" 2>/dev/null
	return 0
}

have() { command -v "$1" >/dev/null 2>&1; }

# Guard the platform up front. Almost every probe below is BSD/macOS syntax —
# stat -f, xattr, lipo, codesign, nm -arch — and on Linux they either fail or,
# worse, mean something entirely different (stat -f prints filesystem stats).
# Better to say so than to emit a page of confident nonsense.
if [ "$(uname -s 2>/dev/null)" != "Darwin" ]; then
	echo "This collector is for macOS. On Windows use collect-datamosh-diagnostics.ps1."
	echo "Detected: $(uname -s 2>/dev/null || echo unknown)"
	exit 0
fi

# Redaction is applied to the whole stream at the end rather than per-line, so
# no section has to remember to do it.
main() {

# --------------------------------------------------------------- section 0 --
section 0 "HEADER"
echo "  collector version:  $SCRIPT_VERSION"
echo "  plugin under test:  ffgl-datamosh (Datamosh / Mosh Transplant)"
echo "  UTC:                $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "  local:              $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "  invoked as:         $0 $*"
echo "  redact-user:        $REDACT"
if have sw_vers; then
	echo "  macOS:              $(sw_vers -productVersion) (build $(sw_vers -buildVersion))"
else
	echo "  macOS:              UNKNOWN (sw_vers absent — is this actually macOS?)"
fi
echo "  arch:               $(uname -m)"
have sysctl && echo "  cpu:                $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
section_end 0

# --------------------------------------------------------------- section 1 --
section 1 "RESOLUME INSTALLATION"
found_app=0
for app in /Applications/Resolume*.app /Applications/Resolume*/*.app; do
	[ -d "$app" ] || continue
	found_app=1
	echo "  APP: $app"
	if have plutil && [ -f "$app/Contents/Info.plist" ]; then
		# plutil -p, not `defaults read`: defaults wants the path WITHOUT the
		# .plist extension and fails silently otherwise.
		plutil -p "$app/Contents/Info.plist" 2>/dev/null \
			| grep -E 'CFBundleShortVersionString|CFBundleExecutable|CFBundleVersion' \
			| sed 's/^/    /'
	fi
	exe="$app/Contents/MacOS/$(basename "${app%.app}")"
	if [ -f "$exe" ] && have lipo; then
		echo "    host architectures: $(lipo -archs "$exe" 2>/dev/null)"
	fi
	# The bundled MCP server appears in 7.26+. Its presence dates the install.
	for mcpb in "$(dirname "$app")"/mcp/*.mcpb "$app/../mcp/"*.mcpb; do
		[ -f "$mcpb" ] && echo "    MCP server present: $mcpb  (Resolume 7.26+)"
	done
done
[ "$found_app" -eq 0 ] && skip 1 "no Resolume installation found under /Applications"

if have pgrep && pgrep -x "Resolume Arena" >/dev/null 2>&1; then
	echo "  RUNNING: Resolume Arena is running now"
elif have pgrep && pgrep -x "Resolume Avenue" >/dev/null 2>&1; then
	echo "  RUNNING: Resolume Avenue is running now"
else
	echo "  RUNNING: no Resolume process found"
fi
note "Whether Resolume runs native or under Rosetta cannot be determined from a"
note "script — sysctl sysctl.proc_translated reports on the calling process, not"
note "on Resolume. Read Activity Monitor's Kind column (Apple / Intel) instead."
section_end 1

# --------------------------------------------------------------- section 2 --
section 2 "PLUGIN FOLDER DISCOVERY"
note "Two official Resolume sources disagree on this path, so nothing is assumed."
candidates=""
for base in "$HOME/Documents/"Resolume*; do
	[ -d "$base" ] || continue
	dir="$base/Extra Effects"
	if [ -d "$dir" ]; then
		count=$(ls -1 "$dir" 2>/dev/null | wc -l | tr -d ' ')
		printf '  CANDIDATE: %-58s EXISTS   (%s entries)\n' "$dir" "$count"
		candidates="$candidates
$dir"
	else
		printf '  CANDIDATE: %-58s ABSENT\n' "$dir"
	fi
done
if [ -z "$candidates" ]; then
	skip 2 "no ~/Documents/Resolume*/Extra Effects folder exists"
else
	echo
	# A here-string rather than a pipe: a `while read` on the right of a pipe
	# runs in a subshell, so anything it sets is lost when the loop ends.
	while read -r dir; do
		[ -n "$dir" ] || continue
		echo "  ls -la $dir"
		ls -la "$dir" 2>/dev/null | sed 's/^/    /'
	done <<< "$candidates"
fi
note "Which of these Resolume actually scans is answered by Preferences > Video"
note "(which lists the FFGL directories) and by section 6's 'Scanning directory'"
note "lines — not by this script."
note "If more than one candidate holds a Datamosh binary you cannot tell which"
note "one Resolume loaded. Empty all but one before testing."
note "Custom FFGL directories are set in Preferences > Video; this script cannot"
note "read them."
section_end 2

# ------------------------------------------------------------ sections 3/4 --
section 3 "PLUGIN FILES AND LOADABILITY"
echo "  Reference values for v0.1.2+:"
echo "    CFBundlePackageType     BNDL"
echo "    Contents/Resources and Contents/_CodeSignature are ABSENT — that is normal."
echo "    arm64 slice: ad-hoc, linker-signed (flags=0x20002). Sufficient for dlopen."
echo "    x86_64 slice: unsigned. Also normal. Do NOT run codesign to 'fix' either."
echo

any_bundle=0
while read -r dir; do
	[ -n "$dir" ] || continue
	for name in Datamosh DatamoshTransplant; do
		bundle="$dir/$name.bundle"
		[ -d "$bundle" ] || continue
		any_bundle=1
		echo "  ---- $bundle"
		echo "    modified: $(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$bundle" 2>/dev/null)"

		plist="$bundle/Contents/Info.plist"
		exe_name="$name"
		if [ -f "$plist" ] && have plutil; then
			plutil -p "$plist" 2>/dev/null \
				| grep -E 'CFBundleExecutable|CFBundleIdentifier|CFBundlePackageType|CFBundleShortVersionString' \
				| sed 's/^/    /'
			parsed=$(plutil -extract CFBundleExecutable raw "$plist" 2>/dev/null)
			[ -n "$parsed" ] && exe_name="$parsed"
		else
			finding "Contents/Info.plist missing — this is not a loadable bundle"
		fi

		exe="$bundle/Contents/MacOS/$exe_name"
		if [ ! -f "$exe" ]; then
			finding "Contents/MacOS/$exe_name MISSING — CFBundleExecutable does not match what is on disk"
			ls -la "$bundle/Contents/MacOS/" 2>/dev/null | sed 's/^/      /'
			continue
		fi

		echo "    size:     $(stat -f '%z' "$exe" 2>/dev/null) bytes"
		have shasum && echo "    sha256:   $(shasum -a 256 "$exe" 2>/dev/null | awk '{print $1}')"
		have file   && echo "    file:     $(file -b "$exe" 2>/dev/null | tr '\n' ' ')"

		if have lipo; then
			archs=$(lipo -archs "$exe" 2>/dev/null)
			echo "    archs:    $archs"
			case "$archs" in
				*arm64*) ;;
				*) finding "NO arm64 SLICE. Resolume 7.11+ runs native ARM and cannot load this." ;;
			esac
		fi

		# The decisive check. Quarantine anywhere inside the bundle blocks the
		# dlopen, and Resolume shows and logs nothing at all when it does.
		if have xattr; then
			echo "    xattr (bundle root):"
			xattr -l "$bundle" 2>/dev/null | sed 's/^/      /'
			echo "    xattr (executable):"
			xattr -l "$exe" 2>/dev/null | sed 's/^/      /'
			qcount=$(find "$bundle" -exec xattr -p com.apple.quarantine {} \; 2>/dev/null | wc -l | tr -d ' ')
			echo "    QUARANTINED FILES INSIDE BUNDLE: $qcount"
			[ "$qcount" != "0" ] && finding "QUARANTINED. Gatekeeper will refuse the load, silently. Fix: xattr -dr com.apple.quarantine '$bundle'"
		fi
		note "com.apple.provenance and com.apple.macl are harmless. Only com.apple.quarantine blocks loading."

		# Without --arch, codesign inspects the host-native slice only, and will
		# report "not signed at all" on an Intel Mac — which contradicts the
		# expectation and sends people signing things they should not.
		if have codesign; then
			for a in arm64 x86_64; do
				echo "    codesign --arch $a:"
				codesign -dvvv --arch "$a" "$exe" 2>&1 | grep -E 'Identifier|CodeDirectory|flags|Signature' | sed 's/^/      /'
			done
		fi

		# Catches the historical build that shipped with no entry point at all.
		if have nm; then
			for a in x86_64 arm64; do
				if nm -arch "$a" -gU "$exe" 2>/dev/null | grep -qi plugmain; then
					echo "    plugMain EXPORTED ($a): yes"
				else
					echo "    plugMain EXPORTED ($a): NO"
					finding "plugMain not exported for $a — the host has nothing to call. This binary cannot load anywhere."
				fi
			done
		elif have strings; then
			c=$(strings "$exe" 2>/dev/null | grep -c plugMain)
			echo "    plugMain presence probe: $c  (WEAK — nm unavailable, this is not an export check)"
		fi
	done
done <<< "$candidates"
[ "$any_bundle" -eq 0 ] && echo "  (no Datamosh bundles found in any candidate folder)"
section_end 3

# --------------------------------------------------------------- section 5 --
section 5 "GPU AND DRIVER"
note "The shader-compile risk is entirely a property of this machine's GLSL compiler."
if have system_profiler; then
	run_bounded 20 system_profiler SPDisplaysDataType 2>/dev/null \
		| grep -E 'Chipset|Vendor|VRAM|Metal|Device ID|Displays|Resolution' | sed 's/^/  /'
else
	skip 5 "system_profiler not available"
fi
section_end 5

# --------------------------------------------------------------- section 6 --
section 6 "RESOLUME LOG"
note "Resolume's v7 directory list names it 'Resolume Arena log.txt' (substitute"
note "Avenue); the 7.11 revision gives only the folder. So: every file is listed."
logdirs=""
for d in "$HOME/Library/Logs/"Resolume*; do
	[ -d "$d" ] || continue
	logdirs="$logdirs
$d"
	echo "  LOG DIR: $d"
	ls -lat "$d" 2>/dev/null | sed 's/^/    /'
done

if [ -z "$logdirs" ]; then
	skip 6 "no ~/Library/Logs/Resolume* directory found"
else
	newest=$(echo "$logdirs" | while read -r d; do
		[ -n "$d" ] || continue
		ls -t "$d"/* 2>/dev/null | head -2
	done)
	echo
	echo "  Matches (case-insensitive substrings), newest file first, capped at 200 lines:"
	echo "$newest" | while read -r f; do
		[ -f "$f" ] || continue
		grep -inE 'datamosh|DMSH|DMMX|transplant|scanning directory|ffgl|plugin main function|library could not be loaded|failed loading plugin|shader|ERROR' "$f" 2>/dev/null \
			| tail -200 | sed "s|^|    $(basename "$f"):|"
	done

	echo
	echo "  LOG SUMMARY"
	scan=$(echo "$newest" | while read -r f; do [ -f "$f" ] && grep -ic 'scanning directory' "$f" 2>/dev/null; done | awk '{s+=$1} END {print s+0}')
	echo "    'Scanning directory' lines:              $scan"
	if echo "$newest" | while read -r f; do [ -f "$f" ] && grep -i 'scanning directory' "$f" 2>/dev/null; done | grep -qi 'extra effects'; then
		echo "    ...one naming an Extra Effects folder:   yes"
	else
		echo "    ...one naming an Extra Effects folder:   no"
		finding "Resolume never scanned an Extra Effects folder. The plugin was NEVER FOUND — this is a path problem, not a loading problem. Check Preferences > Video."
	fi
	dm=$(echo "$newest" | while read -r f; do [ -f "$f" ] && grep -ic 'datamosh' "$f" 2>/dev/null; done | awk '{s+=$1} END {print s+0}')
	echo "    'datamosh' lines:                        $dm"
	if echo "$newest" | while read -r f; do [ -f "$f" ] && grep -i 'library could not be loaded\|failed loading plugin' "$f" 2>/dev/null; done | grep -q .; then
		finding "The file was FOUND but the OS refused to load it — quarantine, wrong architecture, or a missing runtime. See section 3."
	fi
	if echo "$newest" | while read -r f; do [ -f "$f" ] && grep -i 'plugin main function' "$f" 2>/dev/null; done | grep -q .; then
		finding "The library loaded and the plugin's own init returned FAILURE. That is a bug in the plugin, and is the signature of the phantom-parameter class of failure."
	fi
fi
section_end 6

# --------------------------------------------------------------- section 7 --
section 7 "RESOLUME REST API (GET only)"
note "Needs Preferences > Webserver enabled. Introduced in Resolume 7.8."
if ! have curl; then
	skip 7 "curl not available"
else
	base="http://localhost:8080/api/v1"
	product=$(curl -sS --max-time 3 "$base/product" 2>/dev/null)
	if [ -z "$product" ]; then
		skip 7 "nothing answering on localhost:8080 — webserver disabled, or a different port"
	else
		echo "  GET $base/product"
		echo "$product" | head -c 1000 | sed 's/^/    /'
		echo
		echo "  GET $base/effects  (filtered to Datamosh; this is the machine-readable"
		echo "  answer to 'did Resolume actually register the effect')"
		effects=$(curl -sS --max-time 3 "$base/effects" 2>/dev/null)
		if [ -z "$effects" ]; then
			echo "    (no response)"
		else
			echo "$effects" | tr ',' '\n' | grep -i 'datamosh\|DMSH' | head -20 | sed 's/^/    /'
			if echo "$effects" | grep -qi datamosh; then
				echo "    >>> Datamosh IS registered with the host."
			else
				finding "Datamosh is NOT in the host's effect list. Match on name as well as idstring — it is not documented that third-party FFGL IDs are surfaced verbatim."
			fi
		fi
		note "The mixer will NOT appear here. FFGL mixers are blend modes: GET"
		note "$base/composition/layers/1 and read video.mixer instead."
	fi
fi
section_end 7

printf '\n===== END OF REPORT =====\n'

}

if [ "$REDACT" -eq 1 ]; then
	me=$(id -un)
	main "$@" 2>&1 | sed "s|$HOME|<home>|g; s|\\b$me\\b|<user>|g"
else
	main "$@" 2>&1
fi

exit 0
