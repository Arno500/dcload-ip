#!/usr/bin/env bash
# Read the dcdiag counters out of a RUNNING flycast, cold, with no breakpoint.
#
#   scripts/flycast-counters.sh            # one snapshot
#   scripts/flycast-counters.sh 20         # two snapshots 20s apart -> rates
#
# Requires the dcdiag patch in the flycast tree (core/hw/holly/dcdiag.h plus the
# increments in holly_intc.cpp and ta.cpp) and a flycast built from it.
#
# WHY NOT A BREAKPOINT. `bp flycast!asic_RaiseInterrupt` answers the same
# questions but costs a debugger round trip per Holly interrupt: measured, that
# drops VBlank from 60/s to 1.7/s. At 3% speed you can ask "does X ever happen?"
# and nothing more -- rates are meaningless and two runs cannot be compared,
# because the throttling changes what the title does. These counters are plain
# increments on paths that already run, so a snapshot costs nothing and rates
# are real.
set -uo pipefail

INTERVAL="${1:-0}"

PID=$(tasklist.exe 2>/dev/null | awk '/^flycast\.exe/{print $2; exit}' | tr -d '\r')
[[ -n "$PID" ]] || { echo "flycast is not running" >&2; exit 1; }

CDB=$(ls /mnt/c/Program\ Files*/Windows\ Kits/*/Debuggers/x64/cdb.exe 2>/dev/null | head -1)
[[ -n "$CDB" ]] || { echo "cdb.exe not found (install the Windows SDK debuggers)" >&2; exit 1; }

snapshot() {
	"$CDB" -p "$PID" -c \
		'dd flycast!dcdiag_nrm L20; dd flycast!dcdiag_ta_open L8; dd flycast!dcdiag_ta_close L8; dd flycast!dcdiag_ta_param L8; dd flycast!dcdiag_ack_nrm L20; qd' \
		2>/dev/null | tr -d '\r'
}

# cdb prints "<addr>  v1 v2 v3 v4" lines; flatten to one value per line, in order.
flatten() { grep -oE '^[0-9a-f`]+ +([0-9a-f]{8} ?)+' | sed 's/^[0-9a-f`]* *//' | tr ' ' '\n' | grep -E '^[0-9a-f]{8}$'; }

NRM_NAMES=([7]="TA list end: Opaque" [8]="TA list end: Opaque Modifier" \
           [9]="TA list end: Translucent" [10]="TA list end: Trans Modifier" \
           [11]="TA list end: Punch-Through" [12]="Maple DMA done" \
           [13]="Maple V-blank over" [14]="GD-ROM DMA done" [15]="AICA DMA done" \
           [19]="CH2 DMA done" [21]="TA list end (PT alt)" [3]="V-blank in" [4]="V-blank out")
LIST_NAMES=("Opaque" "Opaque Modifier" "Translucent" "Trans Modifier" "Punch-Through" "?5" "?6" "?7")

report() {
	local -n A=$1
	echo "--- SB_ISTNRM interrupts raised (index = bit) ---"
	for b in $(seq 0 31); do
		v=${A[$b]:-0}
		[[ "$v" == "0" ]] && continue
		printf '  bit %-2s  %-12s  %s\n' "$b" "$v" "${NRM_NAMES[$b]:-}"
	done
	echo "--- TA lists: opened / closed / parameters ---"
	printf '  %-18s %10s %10s %12s\n' "list" "open" "close" "params"
	for t in 0 1 2 3 4; do
		printf '  %-18s %10s %10s %12s\n' "${LIST_NAMES[$t]}" \
			"${A[$((32+t))]:-0}" "${A[$((40+t))]:-0}" "${A[$((48+t))]:-0}"
	done
	echo "  (a list opened but never closed raises no interrupt -- that is the"
	echo "   distinction no interrupt counter can make)"
}

mapfile -t V1 < <(snapshot | flatten)
[[ ${#V1[@]} -ge 56 ]] || { echo "unexpected cdb output (${#V1[@]} values); is this flycast built with the dcdiag patch?" >&2; exit 1; }
declare -a S1; for i in "${!V1[@]}"; do S1[$i]=$((16#${V1[$i]})); done

if [[ "$INTERVAL" == "0" ]]; then
	echo "=== flycast dcdiag counters (pid $PID, cumulative) ==="
	report S1
	exit 0
fi

echo "sampling ${INTERVAL}s ..."
sleep "$INTERVAL"
mapfile -t V2 < <(snapshot | flatten)
declare -a D; for i in "${!V2[@]}"; do D[$i]=$(( 16#${V2[$i]} - ${S1[$i]:-0} )); done
echo "=== flycast dcdiag counters (pid $PID, DELTA over ${INTERVAL}s) ==="
report D
