#!/usr/bin/env bash
# Run the SAME image N times and classify the outcome, robustly.
#
#   scripts/sa-repeat.sh [runs] [settle_s]        # default 8 runs, 150 s
#
# WHY THIS EXISTS. The outcome is not reproducible from a single run: the same
# dcload build has reached Sonic Adventure's full title screen on some runs and
# stayed black on others. Every single-run A/B in this investigation is
# therefore evidence about transport counters (which ARE stable) and not about
# the picture. Counting outcomes is the only way to test a race.
#
# WHY THE FIRST VERSION FAILED, and what is different here. It called
# flycast-debug-loop.sh in a loop with no time bound. That script ends by
# launching the Rust server through `powershell.exe Start-Process`, and that
# PowerShell can hang indefinitely -- observed stuck for 20 minutes, producing
# zero runs. Three rules follow, and they are the whole point of this rewrite:
#
#   1. EVERY external step gets a `timeout`. Nothing may block forever.
#   2. Kill stray powershell.exe between runs, not just flycast/dcload-ip-rs.
#      A hung one from the previous run poisons the next.
#   3. A run that does not reach "executing at" within LAUNCH_TIMEOUT is
#      recorded as LAUNCHFAIL and abandoned, not waited on. A harness that
#      stalls on a bad run measures nothing.
#
# Each line is appended as soon as it is known, so a caller that times out
# still keeps every completed run.
set -uo pipefail

RUNS="${1:-8}"
SETTLE="${2:-150}"
LAUNCH_TIMEOUT="${LAUNCH_TIMEOUT:-240}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SCRATCH="${SCRATCH:-/tmp/claude-1000/-opt-toolchains-dc-dcload-ip/c02773a6-3183-4b5a-b58b-3cfca99d4741/scratchpad}"
OUT="${OUT:-$SCRATCH/sa-repeat.txt}"
FLYLOG=/mnt/c/Users/arnod/Code/flycast/build/RelWithDebInfo/flycast.log
SRVLOG=/mnt/c/Users/arnod/Documents/Dreamcast/dc-load/rustserver_stderr.log
mkdir -p "$SCRATCH"

kill_all() {
	timeout 30 taskkill.exe /F /IM flycast.exe      >/dev/null 2>&1
	timeout 30 taskkill.exe /F /IM dcload-ip-rs.exe >/dev/null 2>&1
	# The hung-PowerShell rule above. Harmless otherwise: the launcher's
	# powershell invocations are short-lived by design.
	timeout 30 taskkill.exe /F /IM powershell.exe   >/dev/null 2>&1
	sleep 3
}

# Percentage of lit pixels. Decode the PNG properly -- counting non-zero bytes
# in the file counts DEFLATE output, which barely differs between a black frame
# and a dark one.
classify() {
	timeout 60 python3 - "$1" <<'PY' 2>/dev/null || echo "?"
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
idat, pos, w, h = b'', 8, 0, 0
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos+4])[0]
    typ, body = d[pos+4:pos+8], d[pos+8:pos+8+ln]
    if typ == b'IHDR': w, h = struct.unpack('>II', body[:8])
    elif typ == b'IDAT': idat += body
    pos += 12 + ln
raw = zlib.decompress(idat)
stride, nz, rows = w*3 + 1, 0, 0
for y in range(0, h, 4):
    row = raw[y*stride+1:(y+1)*stride]; rows += 1
    nz += sum(1 for i in range(0, len(row), 3) if row[i] or row[i+1] or row[i+2])
print(f"{100.0*nz/(rows*w):.1f}")
PY
}

# One 32-bit dcload counter by symbol name.
peek32() {
	timeout 60 python3 "$REPO/scripts/dc-peek.py" "$1" --len 4 2>/dev/null \
		| tail -1 | sed 's/.*bytes=//' | cut -c1-8 \
		| timeout 20 python3 -c "import sys;h=sys.stdin.read().strip();print(int.from_bytes(bytes.fromhex(h),'little') if len(h)==8 else '?')" 2>/dev/null || echo "?"
}

printf '# %-3s %-10s %6s %7s %7s %7s %6s %6s %s\n' \
	run outcome lit% ReqCmd GetCStat chunks reent boots note >> "$OUT"

for ((i = 1; i <= RUNS; i++)); do
	kill_all
	timeout "$LAUNCH_TIMEOUT" bash "$REPO/scripts/flycast-debug-loop.sh" \
		--skip-build --keep-running >/dev/null 2>&1

	# Did the game actually start? Bounded, then give up on this run.
	ok=0
	for ((t = 0; t < 60; t++)); do
		grep -q "executing at" "$SRVLOG" 2>/dev/null && { ok=1; break; }
		sleep 2
	done
	if [[ $ok -eq 0 ]]; then
		printf '%5d %-10s %6s %7s %7s %7s %6s %6s %s\n' \
			"$i" LAUNCHFAIL - - - - - - "no EXEC within 120s" >> "$OUT"
		continue
	fi

	sleep "$SETTLE"

	png="$SCRATCH/rep_$i.png"
	timeout 120 python3 "$REPO/scripts/dc-screen.py" "$png" >/dev/null 2>&1
	lit=$(classify "$png")
	req=$(peek32 g_gd_idx_counts)
	chunks=$(peek32 g_cdfs_sync_chunks)
	reent=$(peek32 g_cdfs_sync_reentered)
	gcs=$(timeout 60 python3 "$REPO/scripts/dc-peek.py" g_gd_idx_counts --len 8 2>/dev/null \
		| tail -1 | sed 's/.*bytes=//' | cut -c9-16 \
		| timeout 20 python3 -c "import sys;h=sys.stdin.read().strip();print(int.from_bytes(bytes.fromhex(h),'little') if len(h)==8 else '?')" 2>/dev/null || echo "?")
	boots=$(grep -c 'REIOS: Booting up' "$FLYLOG" 2>/dev/null || echo '?')
	uerr=$(grep -c 'error while uploading' "$SRVLOG" 2>/dev/null || echo 0)
	stale=$(grep -oE 'stale \+[0-9]+ \(=([0-9]+)\)' "$SRVLOG" 2>/dev/null | tail -1 | grep -oE '=[0-9]+' | tr -d '=' || echo 0)

	case "$lit" in
		\?)  out=NO-READ ;;
		0.0) out=BLACK   ;;
		*)   out=PICTURE ;;
	esac
	printf '%5d %-10s %6s %7s %7s %7s %6s %6s upload_err=%s stale=%s\n' \
		"$i" "$out" "$lit" "$req" "$gcs" "$chunks" "$reent" "$boots" "$uerr" "${stale:-0}" >> "$OUT"
done
echo "# done $(date +%H:%M:%S)" >> "$OUT"
