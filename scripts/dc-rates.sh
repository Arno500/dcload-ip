#!/usr/bin/env bash
# Sample dcload's counters twice and print RATES -- what is happening per
# second while a title runs, rather than the totals since boot.
#
#   scripts/dc-rates.sh [window_seconds]      # default 10
#
# WHY RATES, AND WHY THESE ONES
#
# The question "is the loader stealing time from the game" has no clock to
# answer it: PMCR reads 0 once a title owns the machine (§11), so dcload cannot
# time itself. Two counters get around that.
#
#   polls per chunk = d(g_rx_polls) / d(g_cdfs_sync_chunks)
#
# g_rx_polls only advances while dcload holds the CPU, so its ratio to work
# done is how hard the loader is spinning per 16 KB served -- a duty figure
# that needs no clock and no calibration. It is the number to quote. Measured
# on Sonic Adventure: 5301 before the host stopped sleeping through every disc
# read, 179 after.
#
#   g_gd_idx_counts[2] (ExecServer) and [4] (GetDrvStat)
#
# Sonic Adventure calls both once per frame, so their rate IS its frame rate --
# 45.0/s while the loader was hogging the machine, 58.9/s after. A title that
# polls differently will not give a frame rate here, so check the two agree
# before believing either.
#
# FREE ALIGNMENT CHECK: g_gd_idx_counts[3] is InitSystem, which a title calls
# exactly once. If the [3] column is not 1, the array is being read at the
# wrong offset and every other number on this page is fiction.
#
# The duty-cycle percentage is DERIVED and machine-specific: it divides the
# observed poll rate by FULL_TILT below, measured on this host during a load in
# which dcload was blocked continuously. Treat it as an order of magnitude;
# polls-per-chunk is the figure that survives being wrong about it.
#
# Each sample is a fresh GDB attach on purpose. flycast halts the guest for as
# long as a debugger stays connected, so holding the connection would freeze
# the very thing being measured.
set -u
cd "$(dirname "$0")/.."

WINDOW="${1:-10}"
FULL_TILT="${DC_RATES_FULL_TILT:-116582}"   # poll iterations/s at 100% duty

snap() {
  timeout 90 python3 scripts/dc-peek.py g_gd_idx_counts --len 72 2>/dev/null \
    | grep -oP 'bytes=\K[0-9a-f]+'
  timeout 90 python3 scripts/dc-peek.py g_rx_polls g_cdfs_sync_chunks g_rx_frames \
      g_rx_missed g_rx_overflow g_cdfs_read_retries 2>/dev/null \
    | grep -oP '= \K[0-9]+'
}

T0=$(date +%s.%N); A=$(snap)
sleep "$WINDOW"
T1=$(date +%s.%N); B=$(snap)

python3 - "$T1" "$T0" "$FULL_TILT" <<EOF
import sys
dt = float(sys.argv[1]) - float(sys.argv[2])
full = float(sys.argv[3])
A = """$A""".split()
B = """$B""".split()
if len(A) < 2 or len(B) < 2:
    sys.exit("could not read the counters -- is flycast up with its GDB stub, "
             "and a title running?")

def parse(v):
    words = [int.from_bytes(bytes.fromhex(v[0][i:i + 8]), 'little')
             for i in range(0, len(v[0]), 8)]
    return words, [int(x) for x in v[1:]]

wa, na = parse(A)
wb, nb = parse(B)
names = {0: "ReqCmd", 1: "GetCmdStat", 2: "ExecServer", 3: "InitSystem",
         4: "GetDrvStat", 5: "G1DmaEnd", 6: "ReqDmaTrans", 7: "CheckDmaTrans",
         8: "ReadAbort", 9: "Reset", 10: "ChangeDataType"}
cn = ["g_rx_polls", "g_cdfs_sync_chunks", "g_rx_frames", "g_rx_missed",
      "g_rx_overflow", "g_cdfs_read_retries"]

print(f"window = {dt:.2f} s")
print("--- GD syscalls (total / delta / rate) ---")
for i, (x, y) in enumerate(zip(wa, wb)):
    if y == 0 and y - x == 0:
        continue
    print(f"  [{i:2d}] {names.get(i,'?'):14s} {y:9d} {y-x:8d} {(y-x)/dt:9.1f}/s")
if len(wb) > 3 and wb[3] != 1:
    print(f"  !! InitSystem reads {wb[3]}, not 1 -- the histogram is misaligned,")
    print( "     so every number above is meaningless. Check dc-peek's offsets.")
print("--- transport ---")
for n, x, y in zip(cn, na, nb):
    print(f"  {n:22s} {y:9d} {y-x:8d} {(y-x)/dt:9.1f}/s")

polls, chunks = nb[0] - na[0], nb[1] - na[1]
print("--- derived ---")
if chunks > 0:
    print(f"  polls per chunk        {polls/chunks:9.0f}   <- the calibration-free one")
    print(f"  throughput             {chunks*16384/dt/1024:9.1f} KB/s")
    print(f"  dcload's duty cycle    {polls/dt/full*100:9.1f} %   (derived, see header)")
else:
    print("  no disc reads in this window -- the title asked for nothing.")
    print(f"  dcload's duty cycle    {polls/dt/full*100:9.1f} %")
EOF
