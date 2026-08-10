#!/usr/bin/env bash
# Sample the dcdiag counters at a fixed cadence from the START of a run, to
# catch the MOMENT a title stops working rather than its steady state.
#
#   scripts/flycast-transition.sh [samples] [interval_s]
#
# Why this exists. On Sonic Adventure the steady state is uninformative: no
# renders complete, no punch-through lists, display lists carry one parameter
# each. But the cumulative counters show it DID render 425 frames and DID submit
# 12 punch-through lists before that. So the mechanism works and then stops --
# and the interesting measurement is the transition, which a steady-state
# snapshot cannot see.
#
# Columns are chosen so one line tells you which subsystem stopped first:
#   render   SB_ISTNRM bit 0 (render complete)  -- did a frame finish?
#   pt       punch-through lists closed         -- the flag SA waits on
#   op/tr    opaque / translucent lists closed
#   params   TA parameters in opaque lists      -- is there any GEOMETRY?
#   reqcmd   dcload's g_gd_idx_counts[0]        -- is the title still reading?
set -uo pipefail

N="${1:-30}"
IV="${2:-10}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

PID=$(tasklist.exe 2>/dev/null | awk '/^flycast\.exe/{print $2; exit}' | tr -d '\r')
[[ -n "$PID" ]] || { echo "flycast is not running" >&2; exit 1; }
CDB=$(ls /mnt/c/Program\ Files*/Windows\ Kits/*/Debuggers/x64/cdb.exe 2>/dev/null | head -1)
[[ -n "$CDB" ]] || { echo "cdb.exe not found" >&2; exit 1; }

vals() {
	"$CDB" -p "$PID" -c 'dd flycast!dcdiag_nrm L20; dd flycast!dcdiag_ta_close L8; dd flycast!dcdiag_ta_param L8; qd' 2>/dev/null \
		| tr -d '\r' | grep -oE '^[0-9a-f`]+ +([0-9a-f]{8} ?)+' | sed 's/^[0-9a-f`]* *//' \
		| tr ' ' '\n' | grep -E '^[0-9a-f]{8}$'
}

printf '%8s %8s %8s %8s %8s %8s %8s\n' "t(s)" "render" "vblank" "op" "tr" "pt" "params"
t=0
for ((i=0; i<N; i++)); do
	mapfile -t V < <(vals)
	if [[ ${#V[@]} -ge 48 ]]; then
		d() { echo $((16#${V[$1]})); }
		printf '%8s %8s %8s %8s %8s %8s %8s\n' \
			"$t" "$(d 0)" "$(d 3)" "$(d 32)" "$(d 34)" "$(d 36)" "$(d 40)"
	else
		printf '%8s  (cdb returned %d values -- is flycast built with dcdiag?)\n' "$t" "${#V[@]}"
	fi
	# dcload's own read counter, best effort (needs the GDB stub free)
	sleep "$IV"; t=$((t+IV))
done
