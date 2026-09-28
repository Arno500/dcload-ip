#!/usr/bin/env python3
"""Read dcload's own counters off a running Dreamcast, over the network.

WHY THIS EXISTS

  dc-peek.py talks to flycast's GDB stub. On real hardware there is no stub,
  and until now the always-compiled counters listed in AGENTS.md 11 -- the
  ones that say whether the RX ring wedged, whether the NIC was re-initialised,
  how many parts actually landed -- were unreadable on the machine where the
  interesting failures happen.

  dcload answers SBIQ (SendBinQ, "send a binary, quiet") from inside bb->loop(),
  which is where it spends its life, so one UDP round trip reads any range of
  its memory without touching the screen and without a debugger.

WHAT IT REFUSES TO DO

  Resolve a symbol against the wrong image. The loader's base is per-title now
  (AGENTS.md 4.11): the host relinks and chainloads it, so the same counter
  lives at a different address depending on which loader is running. Asking a
  loader at 0x8cfe8000 for an address computed from the 0x8c004000 ELF returns
  plausible nonsense, not an error. So this asks the loader where it lives
  (VERS carries the base since 2.0.4) and refuses to decode anything if the ELF
  disagrees.

USAGE

  scripts/dc-counters.py 192.168.1.64
  scripts/dc-counters.py 192.168.1.64 --repeat 5 --interval 1   # deltas
  scripts/dc-counters.py 192.168.1.64 --elf /path/to/dcload.elf
  scripts/dc-counters.py 192.168.1.64 --raw 0x8cfee0c0 64       # hex dump

CAVEAT

  cmd_sendbinq() sets our_ip from the packet's destination address, exactly
  like every other command. Addressing the Dreamcast by the IP it already
  answers on -- which is the only way to reach it -- therefore changes nothing.
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

DEFAULT_PORT = 53535
CMD_LEN = 12
RECV_TIMEOUT = 0.5
TRIES = 5

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOADER_DIR = os.path.join(REPO, "target-src", "dcload", "loaders")
DEFAULT_ELF = os.path.join(REPO, "target-src", "dcload", "dcload")

# Grouped for reading, not for the wire -- everything is fetched in one range.
# A name that is not in the ELF is skipped in silence: the set of counters
# depends on the build flags (WITH_MAPLE, GD_TRACE...) and an absent one is a
# configuration, not an error.
GROUPS = [
    ("RX ring (rtl8139.c)", [
        "g_rx_polls", "g_rx_frames", "g_rx_missed",
        "g_rx_overflow", "g_rx_reinit", "g_rx_wraps",
        "g_rx_hdr_defer", "g_rx_copying", "g_rx_status_drop",
        "g_rx_last_bad_status",
        "g_rx_linkchange", "g_rx_underrun_ack", "g_rx_link_giveup",
        "g_rx_last_capr", "g_rx_last_cbr",
    ]),
    ("UDP", [
        "g_udp_ok", "g_udp_unmatched", "g_udp_cksum_bad",
    ]),
    ("Upload path (commands.c)", [
        "g_lbin_count", "g_dbin_count", "g_dbin_incomplete",
        "g_pbin_ok", "g_pbin_rejected", "g_pbin_clamped",
        "g_last_load_addr", "g_last_load_size", "g_last_pbin_addr",
        "g_last_reject_addr", "g_last_reject_load",
        "g_last_reject_end", "g_last_reject_size",
    ]),
    ("Warm start / DHCP", [
        "g_warm_start", "g_warm_ip", "g_dhcp_replies", "g_dhcp_not_ours",
    ]),
    ("GD emulation (cdfs_syscalls.c)", [
        "g_gd_park_longs", "g_gd_idx_counts", "g_gd_cmd_counts",
        "g_cdfs_read_retries", "g_cdfs_read_fails", "g_cdfs_read_holes", "g_cdfs_read_stale",
        "g_cdfs_sync_chunks", "g_cdfs_sync_reentered",
        "g_gd_spindown",
    ]),
    ("CDDA (cdda.c) -- AGENTS.md 4.13 says what to read first", [
        "g_cdda_plays", "g_cdda_fetches", "g_cdda_fetch_fails",
        "g_cdda_wrong_lba", "g_cdda_retv_nodata", "g_cdda_stale_lbin",
        "g_cdda_mutes", "g_cdda_ch_stolen",
        "g_cdda_room_min", "g_cdda_svc_gap_max",
        "g_cdda_toc_fails", "g_cdda_last_lba",
        "g_cdda_end_tm", "g_cdda_scale_ppm",
    ]),
    ("Interrupt hook (irq.c) -- AGENTS.md 4.15", [
        "g_irq_hooked", "g_irq_nop_entry", "g_irq_vbr", "g_irq_rehooks",
        "g_irq_refused", "g_irq_refused_vbr",
        "g_irq_entries", "g_irq_ticks", "g_irq_tick_max", "g_irq_tick_sum", "g_irq_evt_last", "g_irq_rx", "g_irq_rx_evt",
        "g_ga_posts", "g_ga_irq_done", "g_ga_xlat_miss", "g_ga_sync", "g_ga_wakes",
    ]),
    ("State", [
        "booted", "running", "our_ip", "tool_ip",
        "g_pmcr_backwards", "g_idle_polls_max",
    ]),
]

IP_NAMES = {"our_ip", "tool_ip", "g_warm_ip"}

# The GD command codes, so g_gd_cmd_counts[] reads as commands rather than as a
# row of 48 numbers to count along by hand. Same numbering as cdfs_syscalls.c
# and as isoldr; a code with no name here still prints, as its number.
GD_CMD_NAMES = {
    16: "PIOREAD", 17: "DMAREAD", 18: "GETTOC", 19: "GETTOC2",
    20: "PLAY_TRACKS", 21: "PLAY_SECTORS", 22: "PAUSE", 23: "RELEASE",
    24: "INIT", 27: "SEEK", 29: "NOP", 30: "REQ_MODE", 31: "SET_MODE",
    33: "STOP", 34: "GETSCD", 35: "GETSES", 36: "REQ_STAT", 40: "GET_VERS",
}
HEX_NAMES = {
    "g_last_load_addr", "g_last_pbin_addr", "g_last_reject_addr",
    "g_last_reject_load", "g_last_reject_end",
    "g_rx_last_capr", "g_rx_last_cbr", "g_rx_last_bad_status",
    "g_cdda_last_lba",
}


def find_nm():
    for cand in ("sh-elf-nm", "sh4-elf-nm"):
        found = shutil.which(cand)
        if found:
            return found
    base = os.environ.get("KOS_CC_BASE")
    if base:
        found = os.path.join(base, "bin", "sh-elf-nm")
        if os.path.exists(found):
            return found
    for guess in ("/opt/toolchains/dc/sh-elf/bin/sh-elf-nm",):
        if os.path.exists(guess):
            return guess
    return None


def read_text(elf):
    """(addr, bytes) of the first loadable code chunk, straight out of the ELF.

    A minimal ELF32 little-endian program-header walk -- no toolchain needed,
    and no dependence on section names, which differ between the LOW and HIGH
    link scripts.
    """
    with open(elf, "rb") as fh:
        blob = fh.read()
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        return None
    phoff, phentsize, phnum = (struct.unpack_from("<I", blob, 0x1c)[0],
                               struct.unpack_from("<H", blob, 0x2a)[0],
                               struct.unpack_from("<H", blob, 0x2c)[0])
    best = None
    for i in range(phnum):
        p = phoff + i * phentsize
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", blob, p)
        if p_type != 1 or p_filesz == 0:          # PT_LOAD with contents
            continue
        if best is None or p_vaddr < best[0]:
            best = (p_vaddr, blob[p_offset:p_offset + min(p_filesz, 256)])
    return best


def read_symbols(elf):
    """{name: (addr, size)} for data/bss symbols, without the ELF underscore."""
    nm = find_nm()
    if nm is None:
        sys.exit("sh-elf-nm not found. source KOS environ.sh, or pass --raw.")
    out = subprocess.run([nm, "-S", "--defined-only", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        # "addr size type name" for sized symbols, "addr type name" otherwise.
        if len(f) == 4:
            addr, size, kind, name = f[0], int(f[1], 16), f[2], f[3]
        elif len(f) == 3:
            addr, size, kind, name = f[0], 0, f[1], f[2]
        else:
            continue
        if kind not in "BbDd":
            continue
        syms[name[1:] if name.startswith("_") else name] = (int(addr, 16), size)
    return syms


class Dc:
    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(RECV_TIMEOUT)

    def send(self, cid, address=0, size=0, data=b""):
        self.sock.sendto(cid + struct.pack(">II", address, size) + data,
                         self.addr)

    def version(self):
        """(version string, base) -- base is None on a loader older than 2.0.4."""
        for _ in range(TRIES):
            self.send(b"VERS", 0x00020004, 0)
            try:
                pkt, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if pkt[:4] != b"VERS":
                continue
            payload = pkt[CMD_LEN:CMD_LEN + struct.unpack(">I", pkt[8:12])[0]]
            nul = payload.find(b"\0")
            if nul < 0:
                return payload.decode("latin-1"), None
            text = payload[:nul].decode("latin-1")
            tail = payload[nul + 1:]
            base = struct.unpack(">I", tail[:4])[0] if len(tail) >= 4 else None
            return text, base
        sys.exit("no answer to VERS -- is the loader still alive?")

    def read(self, address, size):
        """SBIQ: a run of SBIN replies, terminated by DBIN. Retried as a whole."""
        for _ in range(TRIES):
            self.send(b"SBIQ", address, size)
            buf = bytearray(size)
            have = bytearray(size)
            try:
                while True:
                    pkt, _ = self.sock.recvfrom(2048)
                    if pkt[:4] == b"DBIN":
                        break
                    if pkt[:4] != b"SBIN":
                        continue
                    at, n = struct.unpack(">II", pkt[4:12])
                    off = at - address
                    if off < 0 or off + n > size:
                        continue
                    buf[off:off + n] = pkt[CMD_LEN:CMD_LEN + n]
                    have[off:off + n] = b"\1" * n
            except socket.timeout:
                continue
            if all(have):
                return bytes(buf)
        sys.exit(f"incomplete read of {size} bytes at 0x{address:08x}")


def fmt(name, size, raw):
    if size == 1:
        return str(raw[0])
    if size == 4:
        v = struct.unpack("<I", raw)[0]
        if name in IP_NAMES:
            return "%d.%d.%d.%d" % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)
        if name in HEX_NAMES:
            return f"0x{v:08x}"
        return str(v)
    if size % 4 == 0:
        vals = struct.unpack("<%dI" % (size // 4), raw)
        # ONLY WHAT WAS ASKED FOR. 48 slots of which four are used reads as a
        # haystack; the commands a title issues are the whole point.
        if name == "g_gd_cmd_counts":
            hit = [(i, v) for i, v in enumerate(vals) if v]
            if not hit:
                return "(none)"
            return " ".join(f"{GD_CMD_NAMES.get(i, str(i))}={v}" for i, v in hit)
        return "[" + " ".join(str(v) for v in vals) + "]"
    return raw.hex()


def value_of(name, size, raw):
    """Numeric form for delta reporting, or None when a delta is meaningless."""
    if size == 1:
        return raw[0]
    if size == 4 and name not in IP_NAMES and name not in HEX_NAMES:
        return struct.unpack("<I", raw)[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="Dreamcast IP, optionally IP:PORT")
    ap.add_argument("--elf", help="loader ELF (default: chosen from the base "
                                  "the loader reports)")
    ap.add_argument("--raw", nargs=2, metavar=("ADDR", "LEN"),
                    help="hex-dump a range instead of decoding counters")
    ap.add_argument("--repeat", type=int, default=1, help="samples to take")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="seconds between samples")
    args = ap.parse_args()

    host, _, port = args.target.partition(":")
    dc = Dc(host, int(port) if port else DEFAULT_PORT)

    text, base = dc.version()
    print(text + (f" (loaded at 0x{base:08x})" if base is not None else ""))

    if args.raw:
        addr = int(args.raw[0], 0)
        length = int(args.raw[1], 0)
        data = dc.read(addr, length)
        for off in range(0, length, 16):
            row = data[off:off + 16]
            print(f"0x{addr + off:08x}  {row.hex(' ')}")
        return

    elf = args.elf
    if elf is None:
        cand = os.path.join(LOADER_DIR, f"dcload-0x{base:08x}.elf") if base else None
        elf = cand if cand and os.path.exists(cand) else DEFAULT_ELF
    if not os.path.exists(elf):
        sys.exit(f"no ELF at {elf} -- pass --elf, or run `make loaders`")

    syms = read_symbols(elf)

    # THE IDENTITY CHECK. Everything below is an address computed from this
    # ELF; if the machine is running a different image they are addresses in
    # something else, and every value printed would be believable and wrong.
    elf_base = syms.get("dcload_base", (None, 0))[0]
    if base is not None and elf_base is not None and base != elf_base:
        sys.exit(f"{os.path.basename(elf)} is linked at 0x{elf_base:08x} but the "
                 f"loader answers from 0x{base:08x} -- wrong ELF, refusing to decode")

    # THE BASE IS NOT ENOUGH, AND THAT COST A WHOLE READ.
    #
    # Two builds of the same base put the same counter at different addresses:
    # add one global to commands.c and everything after it in .data shifts.
    # Measured on 2026-08-16 -- the loaders deployed next to the host were eight
    # bytes off the tree's, and every value came back believable and wrong
    # (g_dbin_count read 0x0c010000, which is a LoadBinary address). Nothing in
    # the base check could see that. So compare the CODE.
    ref = read_text(elf)
    if ref is not None:
        addr, want = ref
        got = dc.read(addr, len(want))
        if got != want:
            for i, (a, b) in enumerate(zip(got, want)):
                if a != b:
                    break
            sys.exit(
                f"{os.path.basename(elf)} is NOT the image running on the "
                f"Dreamcast: they differ at 0x{addr + i:08x} "
                f"(console 0x{got[i]:02x}, ELF 0x{want[i]:02x}).\n"
                f"Every counter address below would come from the wrong build. "
                f"Point --elf at the ELF that was actually deployed, or "
                f"redeploy this one.")
    print(f"symbols from {os.path.relpath(elf, REPO) if elf.startswith(REPO) else elf}"
          f"{' (image verified)' if ref is not None else ''}")

    wanted = [(g, [n for n in names if n in syms]) for g, names in GROUPS]
    present = [n for _, names in wanted for n in names]
    if not present:
        sys.exit("none of the counters are in this ELF -- is it really dcload?")

    lo = min(syms[n][0] for n in present)
    hi = max(syms[n][0] + max(syms[n][1], 4) for n in present)
    span = hi - lo

    prev = {}
    for sample in range(args.repeat):
        if sample:
            time.sleep(args.interval)
        blob = dc.read(lo, span)
        print()
        for group, names in wanted:
            if not names:
                continue
            print(f"-- {group}")
            for n in names:
                addr, size = syms[n]
                size = max(size, 4) if size == 0 else size
                raw = blob[addr - lo:addr - lo + size]
                line = f"   {n:<24} {fmt(n, size, raw)}"
                cur = value_of(n, size, raw)
                if cur is not None and n in prev and cur != prev[n]:
                    line += f"   (+{cur - prev[n]})" if cur > prev[n] \
                        else f"   ({cur - prev[n]})"
                if cur is not None:
                    prev[n] = cur
                print(line)


if __name__ == "__main__":
    main()
