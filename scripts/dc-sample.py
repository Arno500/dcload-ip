#!/usr/bin/env python3
"""Poll a fixed set of dcload counters at a steady cadence and print a table.

Why this exists rather than a loop around dc-peek.py: attribution of a stall
needs SEVERAL counters read close together (a sample where the read counter is
frozen but the packet counter climbed means something completely different from
one where both froze), and it needs many samples, because a single plateau
cannot distinguish "stuck" from "not started yet".

Symbols resolve once, from the dcload ELF. Raw addresses (0x...) are allowed for
things that are not dcload symbols, e.g. the game's own frame counter.

Usage:
  scripts/dc-sample.py --seconds 180 --period 2
"""
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ELF = REPO / "target-src" / "dcload" / "dcload"
NM = Path("/opt/toolchains/dc/sh-elf/bin/sh-elf-nm")

# (label, symbol-or-address). Order is the column order.
WATCH = [
    ("reads", "cdfs_queue_next"),
    ("slots", "cdfs_slots_in_use"),
    ("gdtick", "g_gd_ticks"),
    ("frame", "0x8c8a2ffc"),
    ("pkt", "g_pkt_seen"),
    ("udp", "g_udp_seen"),
    ("disp", "g_cmd_dispatch"),
    ("pbin_ok", "g_partbin_accepted"),
    ("pbin_drop", "g_partbin_dropped"),
    ("stale", "g_partbin_stale_tag"),
    ("rxrst", "g_rtl_rx_resets"),
    # The two counters that decide whether the 2026-08-05 pair of fixes works:
    # retries should tick over slowly (bounded by CDFS_REQ_RETRY_MIN_GAP_TICKS)
    # and credit_lost should stay at or near zero (it counts PBINs written to
    # memory but credited to no slot -- the black hole).
    ("retries", "g_cdfs_req_retries"),
    ("credlost", "g_cdfs_credit_lost"),
    # Discriminates the two ways a read can go silent for minutes, which look
    # IDENTICAL from the host (nothing arrives, nothing is asked for):
    #   orphans climbing -> the host's CMD_RETVAL was lost. dcload concludes it
    #     itself after CDFS_RETV_GRACE_TICKS (64 ticks, ~0.35s), so this is
    #     never the cause of a long stall -- only of a short hiccup.
    #   retries climbing, orphans flat -> the DC's CMD_CDFSREAD was lost. The
    #     only recovery is CDFS_REQ_RETRY_TICKS (110000 GD syscalls), which at
    #     the rate a title polls at between frames is MINUTES, not the ~2s it
    #     was calibrated to at busy-wait rate.
    ("orphans", "g_cdfs_retv_orphans"),
    # Non-zero means the HOST asked us to write inside dcload's own image and we
    # refused (dcload_owns_range, base 0x0cf00000). Read it together with the
    # sanity of cdfs_slots: if dcload's state is corrupt while this stays 0, the
    # writer was the GAME, not the host -- and nothing guards against that.
    # Titles have been observed allocating right up to 0x0cf00000 with zero
    # bytes of margin, so this is the first thing to check after a wild jump.
    ("selfwr", "g_selfwrite_refused"),
    # Over-announcement watchdog: the window said "complete" while the CDFS slot
    # disagreed. Should stay at 0; anything else means a read was reported
    # finished with bytes missing, which is data corruption delivered as success.
    ("diverge", "g_dbin_divergences"),
]


def symbol_table():
    out = subprocess.run([str(NM), str(ELF)], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
            table.setdefault(parts[2].lstrip("_"), int(parts[0], 16))
    return table


def rsp(payload: str) -> bytes:
    return b"+$" + payload.encode() + b"#%02x" % (sum(payload.encode()) & 0xFF)


def read_reply(sock) -> str:
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")


def sample(addrs):
    """One connect / read-all / detach cycle. Returns list of ints or None."""
    try:
        sock = socket.create_connection(("127.0.0.1", 3263), timeout=4)
    except OSError:
        return None
    vals = []
    with sock:
        sock.settimeout(4)
        try:
            for addr in addrs:
                sock.sendall(rsp(f"m{addr:x},4"))
                hexdata = read_reply(sock)
                if not hexdata or hexdata.startswith("E"):
                    vals.append(None)
                else:
                    vals.append(int.from_bytes(bytes.fromhex(hexdata), "little"))
        except OSError:
            vals = None
        # ALWAYS detach: flycast halts the guest while a debugger is attached,
        # so closing the socket without D leaves the machine looking frozen for
        # reasons that have nothing to do with the bug under investigation.
        try:
            sock.sendall(rsp("D"))
            read_reply(sock)
        except OSError:
            pass
    return vals


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=180)
    ap.add_argument("--period", type=float, default=2.0)
    args = ap.parse_args()

    syms = symbol_table()
    addrs, labels = [], []
    for label, target in WATCH:
        if target.startswith("0x"):
            addrs.append(int(target, 16))
        elif target in syms:
            addrs.append(syms[target])
        else:
            print(f"# missing symbol: {target} ({label}) -- column dropped", file=sys.stderr)
            continue
        labels.append(label)

    print("t    " + "".join(f"{l:>11}" for l in labels), flush=True)
    t0 = time.time()
    prev = None
    while time.time() - t0 < args.seconds:
        vals = sample(addrs)
        t = time.time() - t0
        if vals is None:
            print(f"{t:5.0f}  <stub unreachable>", flush=True)
        else:
            cells = []
            for i, v in enumerate(vals):
                if v is None:
                    cells.append(f"{'?':>11}")
                else:
                    d = "" if prev is None or prev[i] is None else f"+{v - prev[i]}"
                    cells.append(f"{v:>7}{d:>4}" if d else f"{v:>11}")
            print(f"{t:5.0f}  " + "".join(cells), flush=True)
            prev = vals
        time.sleep(args.period)
    return 0


if __name__ == "__main__":
    sys.exit(main())
