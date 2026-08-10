#!/usr/bin/env python3
"""One-shot snapshot of everything that matters during a dcload freeze.

WHY THIS EXISTS. The interesting freeze lasts tens of seconds -- measured at
38.589 s between two host-visible syscalls -- which is ample time to read the
Dreamcast's state while it is still stuck. Every earlier attempt did that by hand
with several dc-peek invocations plus manual hex decoding, and that is exactly
where the mistakes came from: a slot table parsed at the wrong stride once looked
like memory corruption that did not exist, and hand-decoded counters were read
from a stale build's addresses twice.

So: one connection, one pass, everything named, and the struct stride DERIVED from
the ELF rather than written down.

  scripts/dc-freeze.py              # snapshot now
  scripts/dc-freeze.py --watch 3    # re-snapshot every 3s until Ctrl-C

THE SIGNATURE TO LOOK FOR. A gap in the host log with no ReadSector at all does
NOT mean the game stopped asking us for things: gdGdcGetCmdStat generates no host
traffic, so a title busy-waiting on a read's status is completely invisible from
the host side. g_gd_stat_handle / g_gd_stat_ret say which handle it is polling and
what we keep answering, and the slot table says whether that handle is still in
flight. Those two together separate "the game is blocked on us" from "the game is
busy doing its own work".
"""
import argparse
import importlib.util
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Reuse dc-peek's symbol table, RSP framing and image-identity check rather than
# duplicating them -- the verification in particular must not drift out of sync.
_spec = importlib.util.spec_from_file_location("dcpeek", HERE / "dc-peek.py")
peek = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(peek)

COUNTERS = [
    "g_gd_stat_handle", "g_gd_stat_ret",
    "cdfs_queue_next", "cdfs_slots_in_use",
    "g_gd_ticks", "g_partbin_accepted", "g_partbin_dropped", "g_partbin_stale_tag",
    "g_selfwrite_refused", "g_cdfs_credit_lost", "g_cdfs_req_retries",
    "g_cdfs_window_credited", "g_cdfs_retv_orphans", "g_cdfs_dmaend_incomplete",
    "g_cdfs_slots_reclaimed", "g_dbin_divergences", "g_dbin_complete_count",
    "g_loadbin_entries", "g_rtl_rx_resets", "g_rtl_rx_resyncs",
    "g_rtl_tx_timeouts", "g_pkt_seen", "g_udp_seen",
]

SLOT_UINTS = ["handle", "dest", "byte_count", "bytes_received",
              "queue_pos", "lba", "req_tick", "prog_tick", "done_lo"]
SLOT_CHARS = ["win_mask", "retry_count", "data_done", "retval_seen", "acknowledged"]


def read_mem(sock, addr, length):
    sock.sendall(peek.rsp(f"m{addr:x},{length}"))
    reply = peek.read_reply(sock)
    try:
        raw = bytes.fromhex(reply)
    except ValueError:
        return None
    # The stub has been seen answering more than asked; trust only the prefix.
    return raw[:length] if len(raw) >= length else None


def u32(raw, i):
    return struct.unpack_from("<I", raw, i * 4)[0]


def show_slots(sock, syms):
    """Dump cdfs_slots, with the stride taken from the ELF, never assumed.

    A previous session hard-coded 40 bytes after the struct had grown to 44, and
    a perfectly healthy table then read as garbage (handle=1023,
    byte_count=216727552) -- reported as memory corruption that was not there.
    """
    addr = syms.get("_cdfs_slots") or syms.get("cdfs_slots")
    total = peek.symbol_sizes().get("_cdfs_slots")
    if addr is None:
        print("  (cdfs_slots not in ELF)")
        return
    if not total:
        print("  (cdfs_slots has no size in the ELF; refusing to guess a stride)")
        return
    nslots = 4
    stride = total // nslots
    expect = len(SLOT_UINTS) * 4 + len(SLOT_CHARS)
    if stride < expect:
        print(f"  REFUSING to parse: stride {stride} < {expect} bytes of known fields.")
        print("  The struct changed; update SLOT_UINTS/SLOT_CHARS from cdfs_syscalls.c.")
        return

    raw = read_mem(sock, addr, total)
    if raw is None:
        print("  (slot table read failed)")
        return
    print(f"  cdfs_slots @ {addr:08x}  stride {stride} B (derived)")
    hdr = f"  {'#':>1} {'handle':>7} {'dest':>10} {'byte_cnt':>9} {'recvd':>9} " \
          f"{'queue':>6} {'req_tick':>9} {'prog_tick':>9} {'win':>4} {'rty':>3} {'done':>4} {'retv':>4} {'ack':>3}"
    print(hdr)
    for i in range(nslots):
        b = raw[i * stride:(i + 1) * stride]
        v = struct.unpack_from("<9I", b, 0)
        c = b[36:41]
        print(f"  {i} {v[0]:>7} 0x{v[1]:08x} {v[2]:>9} {v[3]:>9} {v[4]:>6} "
              f"{v[6]:>9} {v[7]:>9} 0x{c[0]:02x} {c[1]:>3} {c[2]:>4} {c[3]:>4} {c[4]:>3}")
        if v[0] and v[2]:
            pct = 100.0 * v[3] / v[2]
            print(f"      -> {pct:.1f}% received"
                  + ("  IN FLIGHT, NOT DONE" if not c[2] else ""))


def snapshot(host, port, no_verify):
    import socket
    syms = peek.symbol_table()
    try:
        sock = socket.create_connection((host, port), timeout=5)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
        return 1
    with sock:
        sock.settimeout(5)
        if not no_verify and not peek.verify_image(sock, syms):
            try:
                sock.sendall(peek.rsp("D"))
                peek.read_reply(sock)
            except OSError:
                pass
            return 2

        print("--- counters ---")
        for name in COUNTERS:
            addr = syms.get("_" + name) or syms.get(name)
            if addr is None:
                continue
            raw = read_mem(sock, addr, 4)
            if raw is None:
                print(f"  {name:26} <read failed>")
                continue
            val = struct.unpack("<i" if name.endswith("_ret") else "<I", raw)[0]
            print(f"  {name:26} = {val}")

        print("--- last GetCmdStat answer ---")
        addr = syms.get("_g_gd_stat_out")
        if addr:
            raw = read_mem(sock, addr, 16)
            if raw:
                out = struct.unpack("<4I", raw)
                print(f"  status[] = {out}   (status[2] = bytes transferred so far)")

        print("--- slots ---")
        show_slots(sock, syms)

        # ALWAYS detach: closing the socket leaves flycast halted, which then
        # looks exactly like the freeze under investigation.
        try:
            sock.sendall(peek.rsp("D"))
            peek.read_reply(sock)
        except OSError:
            pass
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--watch", type=float, default=0.0,
                    help="re-snapshot every N seconds (Ctrl-C to stop)")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    if args.watch <= 0:
        return snapshot(args.host, args.port, args.no_verify)
    try:
        while True:
            print("=" * 60)
            snapshot(args.host, args.port, args.no_verify)
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
