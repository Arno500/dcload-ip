#!/usr/bin/env bash
# THE CONTROL EXPERIMENT: boot Sonic Adventure from its own GDI, with no dcload,
# no host tool, and no debugger breakpoint, then screenshot the result.
#
#   scripts/sa-gdi-control.sh [settle_s] [out.png]     # default 180 s
#
# WHY. Every finding in this investigation compares dcload configurations
# against each other. None of them answers the prior question: does this title,
# on this emulator build, get past its title screen AT ALL? If it freezes here
# too, the freeze is flycast's and no dcload change can fix it -- and months of
# A/B on the loader would be measuring nothing.
#
# This was attempted before and INVALIDATED: it was run under a `cdb` breakpoint
# on asic_RaiseInterrupt, which costs a debugger round trip per interrupt and
# drops VBlank from 60/s to 1.7/s. A title starved that badly proves nothing.
# So: full speed, no breakpoint. The GDB stub is used only to resume the
# emulation (flycast built with ENABLE_GDB_SERVER=ON starts suspended) and to
# read the framebuffer at the end -- both are one-shot and cost no per-frame
# overhead. Each ends with a detach, or the target stays halted and looks frozen.
set -uo pipefail

SETTLE="${1:-180}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${2:-$REPO/sa-gdi-control.png}"
FLYCAST_DIR=/mnt/c/Users/arnod/Code/flycast/build/RelWithDebInfo
FLYCAST_DIR_WIN='C:\Users\arnod\Code\flycast\build\RelWithDebInfo'
FLYCAST_EXE_WIN="$FLYCAST_DIR_WIN"'\flycast.exe'
GDI_WIN='C:\Users\arnod\Downloads\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!].gdi'

ps_() { timeout 60 powershell.exe -NoProfile -NonInteractive -Command "$@"; }

echo "[kill] clearing the field"
timeout 30 taskkill.exe /F /IM flycast.exe      >/dev/null 2>&1
timeout 30 taskkill.exe /F /IM dcload-ip-rs.exe >/dev/null 2>&1
timeout 30 taskkill.exe /F /IM powershell.exe   >/dev/null 2>&1
sleep 3

rm -f "$FLYCAST_DIR/flycast.log" 2>/dev/null || true
grep -q '^LogToFile = yes' "$FLYCAST_DIR/emu.cfg" 2>/dev/null \
  || sed -i 's/^\[log\]$/[log]\nLogToFile = yes/' "$FLYCAST_DIR/emu.cfg"

echo "[flycast] launching the GDI directly (no dcload)"
ps_ "Start-Process -FilePath '$FLYCAST_EXE_WIN' -ArgumentList '\"$GDI_WIN\"' -WorkingDirectory '$FLYCAST_DIR_WIN'" >/dev/null

for ((t = 0; t < 60; t++)); do
  timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/3263' 2>/dev/null && break
  sleep 1
done
echo "[flycast] resuming (the stub starts it halted)"
for ((t = 0; t < 8; t++)); do
  timeout 20 python3 "$REPO/scripts/flycast-resume.py" 127.0.0.1 3263 0.5 >/dev/null 2>&1 && break
  sleep 1
done

echo "[wait] ${SETTLE}s at full speed"
sleep "$SETTLE"

timeout 120 python3 "$REPO/scripts/dc-screen.py" "$OUT" >/dev/null 2>&1
echo "[screen] $OUT"
ls -la "$OUT" 2>/dev/null
grep -cE 'REIOS: Booting up' "$FLYCAST_DIR/flycast.log" 2>/dev/null | sed 's/^/[log] REIOS boots = /'
