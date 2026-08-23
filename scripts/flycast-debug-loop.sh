#!/usr/bin/env bash
# Reproducible dcload-ip <-> flycast debug loop.
# See docs/flycast-debug-loop.md for the full recipe (incl. the GDB MCP call
# sequence, which can't be scripted here since GDB is driven via MCP tools).
#
# Usage:
#   scripts/flycast-debug-loop.sh [--skip-build] [--no-rust] [--keep-running]
#                                 [--game <preset>] [--host <ip>]
#
# Presets (mirror /mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/.zed/debug.json):
#   sa-pal    Sonic Adventure v1.003 PAL       (default, host 192.168.1.130)
#   sa2-pal   Sonic Adventure 2 v1.008 PAL     (host 192.168.1.130)
#   sa-intl   Sonic Adventure International    (host 192.168.1.64)
#   sa-cdi    Sonic Adventure CDI              (host 192.168.1.64)
#   crazy-taxi
#   dreamshell (no -d, uses -m test)
#
# Exit code 0 = flycast + (optionally) the Rust server are both up.
# Prints "FLYCAST_PID=", "RUST_PID=", "DC_IP=" lines a caller/LLM can grep.

set -euo pipefail

REPO="/opt/toolchains/dc/dcload-ip"
# Which MSVC configuration of flycast to run. Debug is ~10x slower at emulating
# the SH4, which matters a lot here: the user keeps the dynarec OFF for console
# fidelity, so everything runs through the interpreter. RelWithDebInfo changes
# only how the EMULATOR is compiled -- it does not touch dynarec or any other
# emulation setting -- so prefer it whenever you need a title to actually boot
# in reasonable time. ENABLE_GDB_SERVER is a global cache option, so :3263
# works in both. Override with FLYCAST_CONFIG=Debug.
FLYCAST_CONFIG="${FLYCAST_CONFIG:-RelWithDebInfo}"
FLYCAST_DIR="/mnt/c/Users/arnod/Code/flycast/build/$FLYCAST_CONFIG"
# Windows-form of FLYCAST_DIR: powershell Start-Process -WorkingDirectory needs
# a native Windows path, NOT the /mnt/c WSL path (which fails with
# DirectoryNotFoundException).
FLYCAST_DIR_WIN='C:\Users\arnod\Code\flycast\build\'"$FLYCAST_CONFIG"
FLYCAST_EXE_WIN="$FLYCAST_DIR_WIN"'\flycast.exe'
CDI_DEST_DIR="/mnt/c/Users/arnod/Documents/Dreamcast/dc-load"
CDI_DEST_WIN='C:\Users\arnod\Documents\Dreamcast\dc-load\dcload.cdi'
# Release by default: the host tool is on the critical path of every CDFS
# sector read, and a debug build is an unnecessary variable when the thing
# under measurement is throughput. Set RUST_PROFILE=debug to get symbols back.
RUST_PROFILE="${RUST_PROFILE:-release}"
RUST_EXE_WIN='E:\Nextcloud\Projets\Dreamcast\dcload-ip-rs\target\'"$RUST_PROFILE"'\dcload-ip-rs.exe'
RUST_DIR_WIN='E:\Nextcloud\Projets\Dreamcast\dcload-ip-rs'
LOG_OUT_WIN="${CDI_DEST_WIN%\\*}"'\rustserver_stdout.log'
LOG_ERR_WIN="${CDI_DEST_WIN%\\*}"'\rustserver_stderr.log'
LOG_OUT_WSL="$CDI_DEST_DIR/rustserver_stdout.log"
LOG_ERR_WSL="$CDI_DEST_DIR/rustserver_stderr.log"

SKIP_BUILD=0
NO_RUST=0
KEEP_RUNNING=0
GAME="sa-pal"
HOST_OVERRIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    --no-rust) NO_RUST=1; shift ;;
    --keep-running) KEEP_RUNNING=1; shift ;;
    --game) GAME="$2"; shift 2 ;;
    --host) HOST_OVERRIDE="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

ps_() { powershell.exe -NoProfile -Command "$1"; }

# Poll a predicate until it succeeds: probe FIRST, then sleep. Every wait loop
# here used to sleep before its first probe, which charged dead time to
# conditions that were frequently already true on entry.
# $1 = max attempts, $2 = seconds between attempts, rest = command to run.
# The locals are prefixed because the predicate runs in THIS shell (that is what
# lets probes like flycast_pid_probe assign to FLYCAST_PID). A plain `local i`
# would shadow an `i` used by the predicate, and since the predicate would then
# be writing this loop's counter, the attempt budget would silently go wrong.
poll_until() {
  local _pu_tries=$1 _pu_delay=$2 _pu_i
  shift 2
  for ((_pu_i = 0; _pu_i < _pu_tries; _pu_i++)); do
    "$@" && return 0
    sleep "$_pu_delay"
  done
  return 1
}

# Fast TCP reachability probe, run from WSL rather than via PowerShell (saves a
# ~0.33s interpreter spawn per attempt). The short default timeout is load-
# bearing, not a guess: mirrored-mode WSL + the Hyper-V firewall black-hole SYNs
# to closed Windows ports, so a closed port costs the whole timeout instead of
# returning ECONNREFUSED. See the header of scripts/flycast-resume.py.
port_open() {
  python3 -c 'import socket, sys
s = socket.socket()
s.settimeout(float(sys.argv[3]))
try:
    s.connect((sys.argv[1], int(sys.argv[2])))
except OSError:
    sys.exit(1)
finally:
    s.close()' "${1:-127.0.0.1}" "${2:-3263}" "${3:-0.25}"
}

# ---- 1. Build + package (skip with --skip-build once dcload.cdi is already current) ----
#
# DCLOAD_IP forces DREAMCAST_IP for this build. It defaults to a STATIC address
# because DHCP does not complete under flycast's BBA bridge on this machine:
# measured, the guest sends its DISCOVER (the bridge opens its capture device at
# ~2.7 s, which only happens once the guest has transmitted) and never gets a
# lease, so the console stays unreachable and every host->DC ARP goes
# unanswered. A static IP also means dcload's gratuitous ARP is the first frame
# out, which is exactly what the neighbour-warming step below needs.
#
# Makefile.cfg ships 0.0.0.0 (DHCP) on purpose for real hardware -- that setting
# is not touched here; this is a command-line override, which GNU make gives
# precedence over the assignment in Makefile.cfg and propagates to sub-makes.
# Set DCLOAD_IP=dhcp to build the tree's own setting instead.
#
# The override needs a `make clean` when it CHANGES: DREAMCAST_IP reaches the
# compiler as a -D with no dependency on any file, so an incremental make keeps
# the old objects and silently produces a loader with the previous address
# (AGENTS.md 14.6). The stamp file below is what makes that decidable.
DCLOAD_IP="${DCLOAD_IP:-192.168.1.130}"
if [[ $SKIP_BUILD -eq 0 ]]; then
  echo "[build] source environ.sh && make"
  # shellcheck disable=SC1091
  # KOS environ.sh references unbound vars (KOS_SUBARCH etc.); disable -u
  # around the source or the whole script aborts before building.
  set +u
  source /opt/toolchains/dc/kos/environ.sh
  set -u

  MAKE_IP_ARG=()
  if [[ "$DCLOAD_IP" != "dhcp" ]]; then
    MAKE_IP_ARG=("DREAMCAST_IP=$DCLOAD_IP")
    echo "[build] forcing DREAMCAST_IP=$DCLOAD_IP"
  fi
  # Extra make variables for this build, e.g. to bisect a size knob:
  #   DCLOAD_MAKE_ARGS="PKT_BUFS_IN_HIRAM=0" scripts/flycast-debug-loop.sh
  # Folded into the stamp below, so switching them forces the same clean the
  # IP does. Most of these knobs DO have a file dependency (the %.o rules
  # depend on the Makefile), but relying on that per-knob is exactly the kind
  # of thing that is wrong once and then costs a session of testing an image
  # that was never rebuilt.
  # shellcheck disable=SC2206
  MAKE_EXTRA=(${DCLOAD_MAKE_ARGS:-})
  if [[ ${#MAKE_EXTRA[@]} -gt 0 ]]; then
    echo "[build] extra make args: ${MAKE_EXTRA[*]}"
  fi
  STAMP="$REPO/target-src/dcload/.built-ip"
  STAMP_WANT="$DCLOAD_IP ${DCLOAD_MAKE_ARGS:-}"
  if [[ "$(cat "$STAMP" 2>/dev/null || echo none)" != "$STAMP_WANT" ]]; then
    echo "[build] build settings changed since last build -- make clean"
    make -C "$REPO" clean >/dev/null 2>&1 || true
  fi
  # The per-base loader set the host chainloads into, BEFORE the main build:
  # `make loaders` links each base in turn in the same directory and restores
  # the default build on the way out, so running it first leaves a consistent
  # tree for mkdcdisc to package.
  #
  # It must be built with the SAME DREAMCAST_IP as the CD image. A chainloaded
  # loader is a fresh image with no memory of the previous instance's address
  # except what the BBA's SRAM carries (and that path is only taken on real
  # hardware), so a loader compiled for a different address simply disappears
  # off the network mid-session. The override propagates to the sub-make
  # through MAKEFLAGS.
  if [[ "${LOADERS:-1}" == "1" ]]; then
    echo "[build] make loaders (per-base dcload set)"
    make -C "$REPO/target-src/dcload" "${MAKE_IP_ARG[@]}" "${MAKE_EXTRA[@]}" loaders \
      >/tmp/dcload-loaders.log 2>&1 \
      || { echo "LOADERS BUILD FAILED, see /tmp/dcload-loaders.log" >&2; exit 1; }
  fi

  make -C "$REPO" "${MAKE_IP_ARG[@]}" "${MAKE_EXTRA[@]}" >/tmp/dcload-build.log 2>&1 || { echo "BUILD FAILED, see /tmp/dcload-build.log" >&2; exit 1; }
  echo "$STAMP_WANT" > "$STAMP"

  # Deploy the loader set and the game database where the host tool looks.
  # Not target/debug/loaders: `cargo clean` would take it out and the next run
  # would silently lose every relocation.
  echo "[package] deploying loaders + game database to $CDI_DEST_DIR/loaders"
  # Mirror, do not merge: an ELF for a base that has been dropped from
  # LOADER_BASES must not survive here and go on being chainloaded.
  rm -rf "$CDI_DEST_DIR/loaders"
  mkdir -p "$CDI_DEST_DIR/loaders"
  cp "$REPO"/target-src/dcload/loaders/*.elf "$CDI_DEST_DIR/loaders/" 2>/dev/null || true
  python3 "$REPO/scripts/make-preset-db.py" --out "$CDI_DEST_DIR/loaders/game-presets.tsv" \
    >/tmp/dcload-presets.log 2>&1 \
    || { echo "PRESET DB GENERATION FAILED, see /tmp/dcload-presets.log" >&2; exit 1; }

  echo "[package] mkdcdisc"
  /opt/toolchains/dc/mkdcdisc/build/mkdcdisc -B "$REPO/target-src/1st_read/1st_read.bin" -N \
    -o "$REPO/target-src/1st_read/dcload.cdi" >/tmp/mkdcdisc.log 2>&1 || { echo "mkdcdisc FAILED, see /tmp/mkdcdisc.log" >&2; exit 1; }

  cp "$REPO/target-src/1st_read/dcload.cdi" "$CDI_DEST_DIR/dcload.cdi"
fi

# ---- 1b. Refuse to run a CDI that does not contain the current binary ----
#
# This runs on EVERY path, --skip-build included, because --skip-build is exactly
# where the trap springs: it re-uses the packaged CDI without re-running
# mkdcdisc, so a rebuilt 1st_read.bin never reaches the Dreamcast. Two full test
# sessions were spent debugging code that had never executed, and nothing looked
# wrong: the stale CDI even had the same byte size as a fresh one, so no size or
# "looks recent" check would have caught it.
#
# The only trustworthy test is containment: mkdcdisc stores 1st_read.bin
# verbatim, so its first bytes must appear somewhere in the image.
cdi_contains_current() {
  python3 - "$1" "$REPO/target-src/1st_read/1st_read.bin" <<'PY'
import sys
try:
    sig = open(sys.argv[2], "rb").read()[:64]
    data = open(sys.argv[1], "rb").read()
except OSError as exc:
    print(exc, file=sys.stderr)
    sys.exit(2)
sys.exit(0 if sig and sig in data else 1)
PY
}

for cdi in "$REPO/target-src/1st_read/dcload.cdi" "$CDI_DEST_DIR/dcload.cdi"; do
  if ! cdi_contains_current "$cdi"; then
    echo "[package] STALE CDI: $cdi does not contain the current 1st_read.bin." >&2
    echo "[package] Re-run without --skip-build (mkdcdisc must repackage)." >&2
    exit 1
  fi
done
echo "[package] CDI verified: contains the current 1st_read.bin"

# ---- 2. Kill stale processes from a previous iteration ----
if [[ $KEEP_RUNNING -eq 0 ]]; then
  echo "[cleanup] stopping any existing flycast.exe / dcload-ip-rs.exe"
  # Stop-Process -Force sometimes fails with "Access denied" on flycast and
  # leaves a zombie holding :3263 and flycast.log. The next launch then adds a
  # SECOND instance: both answer the host tool on the same emulated MAC and the
  # run is silently unreproducible. Escalate to WMI Terminate and verify.
  # Kill and count in ONE PowerShell call. This used to be two calls with a
  # blind `sleep 2` between them, which was paid in full even when there was
  # nothing to kill -- the per-process escalation below already waits.
  # `|| true` inside the substitution, not after it: this runs under
  # `set -euo pipefail`, and unlike the probe helpers below this assignment is
  # not in a condition context, so a non-zero exit from PowerShell would abort
  # the whole script instead of just failing the check.
  survivors=$({ ps_ "Get-Process flycast,dcload-ip-rs -ErrorAction SilentlyContinue | ForEach-Object { \$_ | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300; if (Get-Process -Id \$_.Id -ErrorAction SilentlyContinue) { (Get-WmiObject Win32_Process -Filter \"ProcessId=\$(\$_.Id)\").Terminate() | Out-Null } }; @(Get-Process flycast -ErrorAction SilentlyContinue).Count" 2>/dev/null || true; } | tr -d '\r' | tail -1)
  if [[ "${survivors:-0}" != "0" ]]; then
    echo "ERROR: $survivors flycast instance(s) survived the kill; refusing to launch a second one" >&2
    exit 1
  fi
fi

# ---- 3. Launch flycast with the CDI ----
# With --keep-running, REUSE the flycast that is already up instead of starting
# a second one. Two instances share the same emulated BBA MAC, so they both get
# the same DHCP lease and both answer the host tool: the run silently becomes
# unreproducible (and they interleave into the same flycast.log with different
# time bases, which is very confusing to read afterwards).
FLYCAST_PID=$(ps_ "(Get-Process flycast -ErrorAction SilentlyContinue).Id" | tr -d '\r' | head -1)
if [[ $KEEP_RUNNING -eq 1 && -n "$FLYCAST_PID" ]]; then
  echo "[flycast] reusing already-running instance"
else
  # flycast APPENDS to flycast.log across runs and never truncates it, so a
  # stale tail reads like fresh output. Purge before every launch.
  # `|| true`: Windows keeps the handle for a moment after Stop-Process, so the
  # delete can fail with EPERM. Under `set -e` that would abort the whole run.
  rm -f "$FLYCAST_DIR/flycast.log" 2>/dev/null || true
  # LogToFile is read with config::loadBool but is not a registered Option, so
  # flycast drops the key when it rewrites emu.cfg on a clean exit. Re-assert it
  # or the next run produces no log at all.
  if ! grep -q '^LogToFile = yes' "$FLYCAST_DIR/emu.cfg" 2>/dev/null; then
    echo "[flycast] re-adding LogToFile=yes to emu.cfg (flycast drops it on exit)"
    sed -i 's/^\[log\]$/[log]\nLogToFile = yes/' "$FLYCAST_DIR/emu.cfg"
  fi
  # ENABLE_GDB_SERVER=ON in the CMake cache is necessary but NOT sufficient:
  # Debug.GDBEnabled in emu.cfg is the runtime half, and flycast rewrites
  # emu.cfg on a clean exit -- it was found set back to `no`, which looks
  # exactly like a build without the feature (:3263 never opens, every
  # dc-peek/dc-screen instrument is unavailable, and the script's own resume
  # step warns instead of failing). Re-assert it on every launch.
  if ! grep -q '^Debug.GDBEnabled = yes' "$FLYCAST_DIR/emu.cfg" 2>/dev/null; then
    echo "[flycast] re-enabling Debug.GDBEnabled in emu.cfg"
    if grep -q '^Debug.GDBEnabled' "$FLYCAST_DIR/emu.cfg" 2>/dev/null; then
      sed -i 's/^Debug.GDBEnabled = .*/Debug.GDBEnabled = yes/' "$FLYCAST_DIR/emu.cfg"
    else
      sed -i '0,/^Debug\./s//Debug.GDBEnabled = yes\nDebug./' "$FLYCAST_DIR/emu.cfg"
    fi
  fi
  echo "[flycast] launching"
  ps_ "Start-Process -FilePath '$FLYCAST_EXE_WIN' -ArgumentList '$CDI_DEST_WIN' -WorkingDirectory '$FLYCAST_DIR_WIN'" >/dev/null

  FLYCAST_PID=""
  flycast_pid_probe() {
    FLYCAST_PID=$(ps_ "(Get-Process flycast -ErrorAction SilentlyContinue).Id" | tr -d '\r' | head -1)
    [[ -n "$FLYCAST_PID" ]]
  }
  poll_until 20 0.25 flycast_pid_probe || { echo "flycast did not start" >&2; exit 1; }
fi
echo "FLYCAST_PID=$FLYCAST_PID"

# ---- 3b/4. Wait for the GDB stub (:3263), then resume the emulation.
#      flycast built with ENABLE_GDB_SERVER=ON starts SUSPENDED and waits for a
#      debugger; `Debug.GDBWaitForConnection = no` does not prevent it. Without
#      the resume, nothing appears in flycast.log past "REIOS: Booting up" and it
#      looks exactly like a hang.
#
#      ORDER MATTERS, and getting it wrong was this script's single biggest
#      source of startup latency. These were two separate steps: a resume
#      retried up to 20x with a 1s sleep, and only THEN a wait for :3263. So
#      every resume attempt made before the stub had bound its socket connected
#      to a closed Windows port -- which under mirrored-mode WSL is black-holed,
#      not refused, and so cost the resume script's full 5s connect timeout.
#      Worst case that was 20 * (1 + 5) = 120s of pure dead time, followed by up
#      to another 30s in the old step 4. Wait for the port first with cheap
#      short-timeout probes, then resume as soon as there is something to talk
#      to.
#
#      Note the old guard here tested `-z "${FLYCAST_ALREADY_RUNNING:-}"`, a
#      variable nothing in this script ever set, so the block always ran even
#      with --keep-running. Kept that behaviour: a detach against an already
#      running emulator is a harmless no-op resume.
#      ----
echo "[gdb] waiting for :3263"
if poll_until 60 0.25 port_open 127.0.0.1 3263 0.25; then
  echo "[flycast] resuming emulation (GDB stub starts it halted)"
  poll_until 8 0.25 "$REPO/scripts/flycast-resume.py" 127.0.0.1 3263 0.5 >/dev/null 2>&1 \
    || echo "[flycast] WARNING: :3263 is listening but did not acknowledge the resume" >&2
else
  echo "WARNING: :3263 never opened. Either flycast lacks ENABLE_GDB_SERVER=ON" \
       "(reconfigure with 'cmake -B build -DENABLE_GDB_SERVER=ON' and rebuild)," \
       "or flycast hasn't finished initialising." >&2
fi

# ---- 5. Discover the DC's DHCP-leased IP without a screenshot: diff arp -a
#         before/after. Skip entirely if --host was given or dcload-ip-rs
#         doesn't need one (rare). ----
DC_IP="$HOST_OVERRIDE"
if [[ -z "$DC_IP" ]]; then
  if [[ "$DCLOAD_IP" != "dhcp" ]]; then
    # The loader was compiled with this address, so there is nothing to guess.
    DC_IP="$DCLOAD_IP"
    echo "[net] DC_IP=$DC_IP (compiled into this build, not a DHCP lease)"
  else
    case "$GAME" in
      sa-pal|sa2-pal) DC_IP="192.168.1.130" ;;
      *) DC_IP="192.168.1.64" ;;
    esac
    echo "[net] assuming DC_IP=$DC_IP (known-stable DHCP lease for this MAC.\
 Override with --host if the router gave a different one; use 'arp -a' diff otherwise)"
  fi
fi
echo "DC_IP=$DC_IP"

# ---- 5b. Warm the host's neighbour entry for the DC.
#      The host tool's first packet is unicast, so Windows must resolve the DC's
#      MAC first, and while it resolves it SILENTLY DISCARDS the datagrams --
#      `send()` still returns success. Measured with tshark: a tool run that was
#      the first sender after an idle period put exactly two frames on the wire,
#      an ARP request and dcload's reply, and not one of its five VERS
#      datagrams. Prime the entry with another sender first and the very same
#      tool run goes out fine.
#
#      This burst is UDP to the real dcload port, not ICMP. ICMP was worse than
#      useless here: dcload only ever answered 2 of the 10 pings (counted on the
#      DC side), and a run of unanswered probes pushes the Windows neighbour
#      entry to Unreachable, which black-holes the tool's traffic for far longer
#      than a cold cache would have. A datagram to :53535 exercises exactly the
#      path the tool needs and dcload ignores an unrecognised 12-byte command.
#
#      Note dcload also emits a gratuitous ARP at boot, but Windows does not
#      create a cache entry from an unsolicited ARP -- it only refreshes an
#      existing one -- so this is still needed. ----
# First wait for dcload to actually be up. :3263 opening proves nothing -- the
# GDB stub listens from the moment flycast starts. The real signal is the bridge
# announcing its capture device, which only happens once the GUEST transmits its
# first frame (gratuitous ARP for a static IP, DHCP DISCOVER otherwise).
echo "[net] waiting for dcload to come up (first guest frame)"
guest_frame_seen() { grep -aq "using capture device" "$FLYCAST_DIR/flycast.log" 2>/dev/null; }
# 0.25s granularity, not 2s: this is a local grep, so polling it finely is
# nearly free, and the emulator reaches this point at an unpredictable moment.
# The old loop could sit on a condition that had been true for almost 2s.
poll_until 240 0.25 guest_frame_seen \
  || echo "[net] WARNING: no guest frame in flycast.log; dcload may not have booted" >&2

# Warm, then VERIFY. Firing a burst and hoping is what made this step look
# reliable while it was not: an entry that has gone Unreachable (which is what a
# run of unanswered probes leaves behind) black-holes every subsequent send, and
# nothing in the burst tells you that happened. Poll the entry until Windows
# reports a state that actually forwards traffic. Reachable/Stale/Permanent all
# do; Unreachable and Incomplete do not.
NEIGH_STATE=""
neigh_ok() {
  NEIGH_STATE=$(ps_ "(Get-NetNeighbor -IPAddress $DC_IP -ErrorAction SilentlyContinue | Select-Object -First 1).State" | tr -d '\r' | head -1)
  case "$NEIGH_STATE" in
    Reachable|Stale|Permanent) return 0 ;;
  esac
  return 1
}
neigh_warm() {
  ps_ "\$u=New-Object System.Net.Sockets.UdpClient; \$u.Connect('$DC_IP',53535); \$b=New-Object byte[] 12; 1..4 | ForEach-Object { \$u.Send(\$b,12) | Out-Null; Start-Sleep -Milliseconds 200 }; \$u.Close()" >/dev/null 2>&1 || true
  neigh_ok
}
# CHECK before warming. The burst costs ~1.2s (4 sends, 200ms apart) and the
# entry is usually already usable from the previous iteration of the loop, so
# warming unconditionally spent that time to learn nothing. The burst itself
# paces the retries, so no extra sleep between attempts either.
if neigh_ok; then
  echo "[net] neighbour entry for $DC_IP already usable"
else
  echo "[net] warming neighbour entry for $DC_IP (UDP :53535)"
  poll_until 15 0 neigh_warm || true
fi
echo "[net] neighbour state = ${NEIGH_STATE:-<none>}"
case "$NEIGH_STATE" in
  Reachable|Stale|Permanent) ;;
  *) echo "[net] WARNING: Windows will drop the host tool's datagrams in this state." >&2
     echo "[net]          Fix from an ADMIN shell, then re-run:" >&2
     echo "[net]          New-NetNeighbor -InterfaceAlias 'Ethernet 4' -IPAddress $DC_IP -LinkLayerAddress 0C0A0F0E0031 -State Permanent" >&2 ;;
esac

# ---- 6. Launch the Rust host tool (dcload-ip-rs) for the chosen game ----
RUST_PID=""
if [[ $NO_RUST -eq 0 ]]; then
  DL='C:\Users\arnod\Downloads'
  case "$GAME" in
    sa-pal)
      BIN="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\1ST_READ.BIN"
      IMG="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!].gdi"
      # No --gdb flag: the restored server starts its GDB stub unconditionally.
      EXTRA="-d \"$IMG\"" ;;
    sa2-pal)
      BIN="$DL\\Sonic Adventure 2 v1.008 (2001)(Sega)(PAL)(M5)[!]\\1ST_READ.BIN"
      IMG="$DL\\Sonic Adventure 2 v1.008 (2001)(Sega)(PAL)(M5)[!]\\Sonic Adventure 2 v1.008 (2001)(Sega)(PAL)(M5)[!].gdi"
      EXTRA="-d \"$IMG\"" ;;
    sa-intl)
      BIN="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\USA\\1ST_READ.BIN"
      IMG="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\USA\\Sonic Adventure International v1.003 (1999)(Sega)(NTSC)(JP)(M5)[!].gdi"
      EXTRA="-d \"$IMG\"" ;;
    sa-cdi)
      BIN="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\1ST_READ_CDI.BIN"
      IMG="$DL\\Sonic Adventure v1.003 (1999)(Sega)(PAL)(M5)[!]\\Sonic Adventure (USA)(Limited Edition).cdi"
      EXTRA="-d \"$IMG\"" ;;
    crazy-taxi)
      BIN="$DL\\Crazy Taxi v1.000 (2000)(Sega)(PAL)[!]\\1ST_READ.BIN"
      IMG="$DL\\Crazy Taxi v1.000 (2000)(Sega)(PAL)[!]\\Crazy Taxi v1.000 (2000)(Sega)(PAL)[!].gdi"
      EXTRA="-d \"$IMG\"" ;;
    dreamshell)
      BIN='C:\Users\arnod\Documents\Dreamcast\DS.elf'
      EXTRA="-m test" ;;
    *) echo "Unknown --game preset: $GAME" >&2; exit 1 ;;
  esac

  # Build the host tool too unless told not to. Forgetting this is the same trap
  # as a stale CDI (see step 1b): the run looks normal and exercises the
  # PREVIOUS binary. cargo.exe is reachable from WSL and produces the Windows
  # .exe the launch below needs; a WSL-native `cargo` would build an ELF that
  # Start-Process cannot run. Set RUST_BUILD=0 to skip.
  RUST_EXE_WSL="/mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/target/$RUST_PROFILE/dcload-ip-rs.exe"
  if [[ "${RUST_BUILD:-1}" == "1" ]]; then
    echo "[rust] cargo build ($RUST_PROFILE)"
    CARGO_ARGS=(build)
    [[ "$RUST_PROFILE" == "release" ]] && CARGO_ARGS+=(--release)
    ( cd /mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs && cargo.exe "${CARGO_ARGS[@]}" ) \
      >/tmp/dcload-rs-build.log 2>&1 \
      || { echo "RUST BUILD FAILED, see /tmp/dcload-rs-build.log" >&2; tail -30 /tmp/dcload-rs-build.log >&2; exit 1; }
  fi
  if [[ ! -f "$RUST_EXE_WSL" ]]; then
    echo "ERROR: $RUST_EXE_WSL does not exist (RUST_PROFILE=$RUST_PROFILE)." >&2
    exit 1
  fi

  echo "[rust] launching dcload-ip-rs ($GAME, host $DC_IP)"
  # RUST_EXTRA_ARGS is appended to the subcommand, which is where dcload-ip-rs
  # wants its flags (`u-exec <bin> -d <image> --verify-reads`, not before
  # u-exec). Use it for one-off instrumentation without editing this script:
  #   RUST_EXTRA_ARGS=--verify-reads scripts/flycast-debug-loop.sh --skip-build
  # RUST_GLOBAL_ARGS goes BEFORE the subcommand (that is where clap puts the
  # global flags), RUST_EXTRA_ARGS after it. `-v` is global and turns on the
  # debug! lines, including one per ReadSector with its LBA and first 8 bytes --
  # the only way to see which sectors the host actually served:
  #   RUST_GLOBAL_ARGS=-v scripts/flycast-debug-loop.sh --skip-build
  # RUST_ENV is a space-separated list of KEY=VALUE set in the launched
  # process's environment. Several of dcload-ip-rs' knobs are env vars rather
  # than flags, and Start-Process inherits the PowerShell session's environment,
  # so this is the only way to reach them without editing the Rust source:
  #   RUST_ENV=DCLOAD_VERIFY_READS=1 scripts/flycast-debug-loop.sh --skip-build
  #   RUST_ENV="DCLOAD_RT_BURST=1 DCLOAD_RT_DELAY_US=500" ...
  # Where the host finds the per-base loaders and the game database. Both were
  # deployed in step 1; the host also accepts --loader-dir / --game-db.
  RUST_ENV="DCLOAD_LOADER_DIR=${CDI_DEST_WIN%\\*}\\loaders ${RUST_ENV:-}"
  RUST_ENV_PS=""
  for kv in ${RUST_ENV:-}; do
    RUST_ENV_PS="$RUST_ENV_PS\$env:${kv%%=*} = '${kv#*=}'; "
  done
  ps_ "$RUST_ENV_PS\$argLine = '--host $DC_IP ${RUST_GLOBAL_ARGS:-} u-exec \"$BIN\" $EXTRA ${RUST_EXTRA_ARGS:-}'; Start-Process -FilePath '$RUST_EXE_WIN' -ArgumentList \$argLine -WorkingDirectory '$RUST_DIR_WIN' -RedirectStandardOutput '$LOG_OUT_WIN' -RedirectStandardError '$LOG_ERR_WIN'" >/dev/null

  rust_pid_probe() {
    RUST_PID=$(ps_ "(Get-Process dcload-ip-rs -ErrorAction SilentlyContinue).Id" | tr -d '\r' | head -1)
    [[ -n "$RUST_PID" ]]
  }
  poll_until 20 0.25 rust_pid_probe || true
  echo "RUST_PID=$RUST_PID"
  echo "[rust] logs: $LOG_OUT_WSL / $LOG_ERR_WSL (tail these instead of re-running the tool)"
fi

echo "[done] flycast=$FLYCAST_PID rust=${RUST_PID:-none} dc_ip=$DC_IP"
