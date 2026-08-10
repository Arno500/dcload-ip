#!/usr/bin/env bash
# Report every reference from RESIDENT dcload code to a symbol that lives in
# the TRANSIENT region (>= _resident_end).
#
# Why this matters: after the resident/transient split, everything at or above
# _resident_end is memory a running game owns and overwrites (Sonic Adventure
# fills 0x8ce07340..0x8ce0b000). A resident function that calls a transient one
# while running==1 does not fail gracefully -- the jump lands in game data and
# raises an illegal instruction (historically PC=0xac000010). The `!running`
# guard *inside* the callee cannot help: the crash happens on the jump.
#
# So every hit below must fall into one of these buckets:
#   - guarded at the call site by `!running` (or `is_main_loop`, which only
#     holds before EXEC)
#   - boot-only (crt0 -> main)
#   - a linker boundary literal that objdump merely annotated with whichever
#     symbol happens to sit at that address (_resident_end / __transient_bss_start)
# Anything else is a latent crash. Follow the call graph one level deeper than
# feels necessary: PMCR_Restart was correctly kept resident, but its callees
# PMCR_Enable/PMCR_Stop were not, which is exactly how a non-deterministic
# crash got in.
#
# Usage: scripts/check-resident-invariant.sh [path/to/dcload.elf]

set -uo pipefail

ELF="${1:-$(dirname "$0")/../target-src/dcload/dcload}"
SH="${KOS_CC_BASE:-/opt/toolchains/dc/sh-elf}/bin"

[[ -f "$ELF" ]] || { echo "no ELF at $ELF (source environ.sh && make first)" >&2; exit 1; }

RE=$("$SH/sh-elf-nm" "$ELF" | awk '$3=="_resident_end"{print "0x"$1}')
[[ -n "$RE" ]] || { echo "_resident_end not found in $ELF" >&2; exit 1; }

# The bound is DERIVED from the linker script, for the same reason PREFIX is
# derived below: it was written here as a literal and went stale the moment the
# base moved, so the headroom figure was nonsense (negative, in fact) while the
# check itself still printed happily. dcload.x owns this number; read it there.
LDS="$(dirname "$0")/../target-src/dcload/dcload.x"
BOUND=$(sed -n 's/.*_resident_end <= \(0x[0-9a-fA-F]*\).*/\1/p' "$LDS" | head -1)
[[ -n "$BOUND" ]] || { echo "cannot read the _resident_end bound from $LDS" >&2; exit 1; }

printf '_resident_end = %s   headroom to %s = %d bytes\n\n' \
	"$RE" "$BOUND" "$(( BOUND - RE ))"

# The address prefix has to be DERIVED, never written down. It was hardcoded to
# "8ce0" and stayed that way after dcload was relocated to 0x8cf00000, so the
# pattern matched nothing and the check reported a clean bill of health for every
# build since -- the worst possible failure mode for a guard.
PREFIX="${RE:2:4}"

"$SH/sh-elf-objdump" -d --stop-address=$((RE)) "$ELF" 2>/dev/null | awk -v e=$((RE)) -v p="$PREFIX" '
	/^[0-9a-f]+ <_/ { fn=$2; next }
	$0 ~ ("! " p "[0-9a-f]{4} <_") {
		for (i = 1; i <= NF; i++) if ($i == "!") { a = strtonum("0x" $(i+1)); sym = $(i+2); break }
		if (a >= e) print (fn == "" ? "<crt0/literal>" : fn) "  ->  " sym
	}' | sort -u | grep -vE "_end>|_stack>"
