#!/usr/bin/env bash
# Catch whoever writes the BIOS GD syscall vector at guest 0x8c0000bc.
#
# WHY THIS EXISTS, AND WHY IT IS NOT A GDB SCRIPT
# -----------------------------------------------
# flycast's GDB stub implements NO watchpoints: Z1/Z2/Z3/Z4 all answer with an
# empty packet (`insertMatchpoint` in core/debug/gdb_server.cpp literally does
# `return "";` for those four). Only Z0 software breakpoints work, and only with
# len == 2. The SH4's own UBC is not a way out either -- flycast's
# core/hw/sh4/modules/ubc.cpp is 41 lines of register storage with no compare
# logic anywhere in the memory path, so programming BARA from dcload would
# compile, run, and never fire.
#
# What DOES work is a host-side x64 hardware watchpoint, set with cdb.exe on the
# host address that guest RAM is mapped to. That is what this script sets up.
#
# WHAT IT ANSWERS
# ---------------
# On Sonic Adventure the machine dies because ~121 KB of TA vertex parameters
# land at guest 0x8c000000, wiping the syscall vector table at +0xbc; the
# title's trampoline at 0x8c10d8a0 then jumps into on-chip register space. Every
# destination-selection register (QACR0/1, SB_C2DSTAT, LMMODE, CHCR2, the TA
# output pointers) reads correct, so nothing was mis-aimed at the hardware
# level -- which leaves plain CPU stores through a pointer that was zero. This
# script names the code that does it: guest PC and PR at the moment of the
# write, plus the flycast host callstack, which also settles CPU-store versus
# DMA at the emulator level.
#
# USAGE
# -----
#   scripts/dc-catch-vector-write.sh --dry-run   # show what would be armed
#   scripts/dc-catch-vector-write.sh             # attach and arm, then block
#
# Arm it AFTER the title is running (see "WHEN TO ARM" below). It then blocks
# until the write happens; Ctrl-C to give up. Requires an Administrator shell
# on the Windows side, same as the rest of the cdb tooling.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$REPO/target-src/dcload/dcload"
NM=/opt/toolchains/dc/sh-elf/bin/sh-elf-nm
OBJDUMP=/opt/toolchains/dc/sh-elf/bin/sh-elf-objdump

FLYCAST_CONFIG="${FLYCAST_CONFIG:-RelWithDebInfo}"
FLYCAST_DIR="/mnt/c/Users/arnod/Code/flycast/build/$FLYCAST_CONFIG"
WORKDIR="/mnt/c/Users/arnod/Documents/Dreamcast/dc-load"
WORKDIR_WIN='C:\Users\arnod\Documents\Dreamcast\dc-load'
CDB_WIN='C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'

CMDS="$WORKDIR/cdb_vector_cmds.txt"
CMDS_WIN="$WORKDIR_WIN"'\cdb_vector_cmds.txt'
CLOG="$WORKDIR/cdb_vector.log"
CLOG_WIN="$WORKDIR_WIN"'\cdb_vector.log'

GUEST_VECTOR=0x8c0000bc
DRY_RUN=0
VERIFY=0
PID_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1 ;;
        --verify) VERIFY=1 ;;
        --pid) PID_OVERRIDE="$2"; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

# --- 1. The value a LEGITIMATE write puts there -----------------------------
#
# Derived, never hard-coded. cdfs_redir_enable writes the literal word
# `cfs_redir_k`, and cfs_redir has already moved once in this project's history
# (0x8cf00444 -> 0x8cf00410) when four bytes left the jump table in
# dcload-crt0.s. A stale literal here would make the watchpoint fire on
# dcload's own legitimate install and, worse, look like a catch. See AGENTS.md
# §14 and the memory note never-hardcode-a-linker-owned-address.
[ -f "$ELF" ] || die "no dcload ELF at $ELF -- build target-src/dcload first"
K_ADDR=$("$NM" "$ELF" 2>/dev/null | awk '$3 == "cfs_redir_k" { print $1; exit }')
[ -n "$K_ADDR" ] || die "symbol cfs_redir_k not found in $ELF"
EXPECTED=$("$OBJDUMP" -s --start-address="0x$K_ADDR" --stop-address=$((0x$K_ADDR + 4)) "$ELF" \
    | awk '/^ [0-9a-f]+ / { print $2; exit }' \
    | sed -E 's/(..)(..)(..)(..)/\4\3\2\1/')      # little-endian -> value
[ -n "$EXPECTED" ] || die "could not read the contents of cfs_redir_k"
echo "[elf]  cfs_redir_k @ 0x$K_ADDR contains 0x$EXPECTED  (the legitimate vector value)"

# --- 2. Where guest RAM lives in the host process --------------------------
#
# flycast logs "BASE <p> RAM(16 MB) <p> ..." and the RAM pointer is
# &ram_base[0x0C000000] (addrspace.cpp) -- a pointer INTO the nvmem region, not
# a second mapping, so guest 0x8c0000bc (physical 0x0c0000bc) is exactly
# RAM + 0xbc and there is only one host virtual address to watch. The line is
# emitted TWICE per run (~0.85 s apart); take the LAST, or the watchpoint sits
# on a stale mapping and never fires.
FLOG="$FLYCAST_DIR/flycast.log"
[ -f "$FLOG" ] || die "no flycast.log at $FLOG (is LogToFile = yes?)"
RAMBASE=$(grep -ao 'RAM(16 MB) [0-9A-Fa-f]\+' "$FLOG" | tail -1 | awk '{print $3}')
[ -n "$RAMBASE" ] || die "no 'RAM(16 MB)' line in $FLOG -- flycast may not have booted"
HOST_ADDR=$(printf '%X' $(( 0x$RAMBASE + (GUEST_VECTOR & 0xFFFFFF) )))
echo "[mem]  RAM base 0x$RAMBASE -> guest $GUEST_VECTOR is host 0x$HOST_ADDR"

# --- 3. The target process -------------------------------------------------
if [ -n "$PID_OVERRIDE" ]; then
    FLYPID="$PID_OVERRIDE"
else
    FLYPID=$(tasklist.exe /FI "IMAGENAME eq flycast.exe" /FO CSV /NH 2>/dev/null \
        | tr -d '\r' | awk -F'","' 'NR==1 { gsub(/"/,"",$2); print $2 }')
fi
[ -n "$FLYPID" ] || die "flycast.exe is not running"
echo "[proc] flycast.exe pid $FLYPID"

# --- 3b. Prove the address translation before trusting it ------------------
#
# A watchpoint on the wrong host address never fires, and "never fired" is
# indistinguishable from "nobody wrote there" -- the exact failure mode
# AGENTS.md §14.16 warns about. So verify the chain (log parse -> & 0xFFFFFF ->
# cdb attach -> symbol access) by reading the word and comparing it with what
# the GDB stub reports for the same guest address. Cheap, and safe on a frozen
# instance. Run it after every flycast restart: the RAM base changes per run.
if [ "$VERIFY" = 1 ]; then
    {
        printf '.echo ===watched word===\r\n'
        printf 'dd %s L1\r\n' "$HOST_ADDR"
        printf '.echo ===guest context===\r\n'
        printf 'dx flycast!p_sh4rcb->cntx.pc\r\n'
        printf 'dx flycast!p_sh4rcb->cntx.vbr\r\n'
        printf 'qd\r\n'
    } > "$CMDS"
    rm -f "$CLOG"
    powershell.exe -NoProfile -Command \
        "& '$CDB_WIN' -p $FLYPID -logo '$CLOG_WIN' -cf '$CMDS_WIN'" >/dev/null 2>&1
    [ -f "$CLOG" ] || die "cdb produced no log -- is flycast running elevated? \
run this shell as Administrator, or start flycast un-elevated."
    echo
    echo "--- cdb sees ---"
    tr -d '\r' < "$CLOG" | grep -E '===|^[0-9a-f]{8}`?[0-9a-f]*  |cntx\.' || true
    echo
    echo "--- GDB stub sees, for the same guest address ---"
    python3 "$REPO/scripts/dc-regs.py" --raw "$(printf '%x' $GUEST_VECTOR)" --len 4 2>/dev/null \
        || echo "(stub unreachable -- compare by hand)"
    echo
    echo "The two must agree (cdb prints host byte order, the stub prints raw"
    echo "bytes). If they differ, the RAM base or the pid is stale -- re-run."
    exit 0
fi

# --- 4. The cdb command file ----------------------------------------------
#
# `ba w4` is legal here only because 0xbc is 4-byte aligned; x64 watchpoints
# must be aligned to their own size, and `ba w4` on a misaligned address fails
# silently enough to look like "nothing ever wrote there".
#
# THE BUILT-IN POSITIVE CONTROL. An x64 data breakpoint lives in the per-thread
# debug registers, and flycast runs the SH4 on its own emulation thread, so a
# `ba` that does not reach that thread would simply never fire -- and "never
# fired" reads exactly like "nobody wrote there". Rather than assume cdb
# propagates it, we let dcload prove it: cdfs_redir_enable writes this very word
# from GUEST code, on the emulation thread, when the title is launched. So arm
# BEFORE launching, and treat that write as a self-test.
#
#   $t0 = 0  -> we have not yet seen dcload install the vector.
#   a write of the legitimate value      -> SELFTEST-OK, set $t0 = 1, continue.
#   a write of anything else while $t0=0 -> boot-time churn (BIOS/reios), ignore.
#   a write of anything else while $t0=1 -> the catch. Stop and dump.
#
# If the run reaches the freeze and SELFTEST-OK never appeared, the instrument
# was blind and the result means nothing -- re-arm on the emulation thread
# explicitly (find it with `~*k` and use `~N ba ...`).
#
# `kp` gives the flycast HOST callstack, which is what distinguishes an
# interpreter store opcode from a DMA/memcpy path. `qd` quits and DETACHES,
# leaving flycast alive for follow-up reads.
BA="ba w4 $HOST_ADDR \".echo ===GD-VECTOR-WRITE===; dd $HOST_ADDR L1; dx flycast!p_sh4rcb->cntx.pc; dx flycast!p_sh4rcb->cntx.pr; .if (dwo($HOST_ADDR) == 0x$EXPECTED) { .echo ===SELFTEST-OK: dcload installed cfs_redir, watchpoint proven live===; r \$t0 = 1; gc } .else { .if (@\$t0 == 1) { .echo ===FATAL WRITE - STOPPING===; dx flycast!p_sh4rcb->cntx.r[0]; dx flycast!p_sh4rcb->cntx.r[3]; dx flycast!p_sh4rcb->cntx.r[15]; dx flycast!p_sh4rcb->cntx.vbr; kp; qd } .else { .echo ===pre-install write, ignoring===; gc } }\""

{
    printf '.echo === armed on host %s (guest %s), legit value 0x%s ===\r\n' \
        "$HOST_ADDR" "$GUEST_VECTOR" "$EXPECTED"
    printf 'r $t0 = 0\r\n'
    printf '%s\r\n' "$BA"
    printf 'g\r\n'
} > "$CMDS"

echo "[cdb]  command file: $CMDS"
if [ "$DRY_RUN" = 1 ]; then
    echo
    echo "--- would run ---"
    echo "cdb.exe -p $FLYPID -logo $CLOG_WIN -cf $CMDS_WIN"
    echo
    echo "--- command file ---"
    tr -d '\r' < "$CMDS"
    exit 0
fi

rm -f "$CLOG"
echo "[cdb]  attaching and arming; this BLOCKS until the vector is written."
echo "       Do not run dc-peek/dc-regs against :3263 while this is attached."
powershell.exe -NoProfile -Command \
    "& '$CDB_WIN' -p $FLYPID -logo '$CLOG_WIN' -cf '$CMDS_WIN'" >/dev/null 2>&1

# --- 5. Read the verdict back ---------------------------------------------
[ -f "$CLOG" ] || die "cdb produced no log at $CLOG"
echo
echo "=== catch ==="
tr -d '\r' < "$CLOG" | grep -E '===|cntx\.|^[0-9a-f]{8}|flycast!' || {
    echo "(nothing matched; full log at $CLOG)"; exit 1; }

# Translate the guest PC into something actionable. A PC inside 0x8c004000..
# 0x8cf0c000 would mean dcload itself wrote the vector, which would be a very
# different -- and much more serious -- finding than the title doing it.
GUEST_PC=$(tr -d '\r' < "$CLOG" | grep -A1 'cntx\.pc' | grep -oiE '0x[0-9a-f]{8}' | head -1)
if [ -n "$GUEST_PC" ]; then
    echo
    echo "guest PC $GUEST_PC"
    pc=$((GUEST_PC))
    if [ $pc -ge $((0x8c004000)) ] && [ $pc -lt $((0x8c010000)) ]; then
        echo "  ** inside dcload ** -- resolve with:"
        echo "     $NM $ELF | sort | awk '\$1 <= \"${GUEST_PC#0x}\"' | tail -3"
    else
        echo "  in the title's own code (dcload is 0x8c004000..0x8c010000)."
        echo "  host address to disassemble in cdb:"
        printf "     u %X L20\n" $(( 0x$RAMBASE + (pc & 0xFFFFFF) ))
    fi
fi
