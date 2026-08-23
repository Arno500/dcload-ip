#!/usr/bin/env python3
"""Read the GUEST_TICK sampling block off a running Dreamcast.

WHY THIS EXISTS

  dcload only executes when a title calls a GD syscall. A title that hangs
  before its first disc read therefore takes the machine with it: nothing is
  answered, no counter is readable, and every diagnosis is inference. Build the
  loader with GUEST_TICK=1 and a TMU0 interrupt handler in the vector table we
  hand the title samples SPC -- the PC the title was executing -- into a ring.

  A hung title samples to the same handful of addresses. That names the loop.

WHY IT NEEDS NO ELF

  The block sits at 0x8cff8000, above every loader placement for every base and
  outside every region anything zeroes -- crt0's BSS, .hiram, zero_game_ram()'s
  range, and 1st_read's 0x8c004000..0x8c010000 bootstrap fill. It does NOT move
  with the loader's base. So unlike dc-counters.py this cannot be pointed at the
  wrong image (AGENTS.md 14.19): there is no symbol to resolve. The magic word
  is the only check, and it is enough.

HOW TO GET AN ANSWER OUT OF A HUNG MACHINE

  Nobody services the network while the title is stuck, so this cannot be read
  live. There are two ways back, and the block survives both:

    1. THE WATCHDOG. After GUEST_TICK_WATCHDOG ticks (300 = about 3 s) the
       handler jumps back into the loader, which re-inits, adopts the adapter
       warm and answers again. You will SEE this: dcload repaints its own
       screen. Then run this.

    2. THE RESET, when the watchdog never fires -- which is itself the finding
       that the title masked our timer or took the machine somewhere the SH4
       cannot be interrupted out of. Reset the console, let it boot dcload off
       the CD, run this. The samples are still at 0x8cff8000; nothing on the
       boot path clears that address.

  watchdog=1 in the output means exactly that happened, i.e. the title really
  was not running. watchdog=0 with a rising tick count means the title is alive
  and simply never asked for a disc.

USAGE

  scripts/dc-ticks.py 192.168.1.64
  scripts/dc-ticks.py 192.168.1.64 --watch      # re-read until interrupted
"""

import argparse
import socket
import struct
import sys
import time

DEFAULT_PORT = 53535
CMD_LEN = 12
RECV_TIMEOUT = 0.5
TRIES = 5

BLOCK_ADDR = 0x8CFF8000		# GUEST_TICK_BLOCK -- survives a reset, see below
BLOCK_LEN = 160
MAGIC = 0x4443544B		# 'DCTK'
RING = 32

# INTEVT codes worth naming; anything else is printed raw. A title that is
# running normally shows a spread here, a hung one usually shows only TUNI0.
INTEVT = {
    0x400: "TUNI0 (our timer)",
    0x420: "TUNI1",
    0x440: "TUNI2",
    0x4A0: "RTC",
    0x320: "IRL",
    0x360: "IRL",
    0x3A0: "IRL",
    0x3E0: "IRL",
}


class Dc:
    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(RECV_TIMEOUT)

    def send(self, cid, address=0, size=0, data=b""):
        self.sock.sendto(cid + struct.pack(">II", address, size) + data, self.addr)

    def alive(self):
        for _ in range(TRIES):
            self.send(b"VERS", 0x00020004, 0)
            try:
                pkt, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if pkt[:4] == b"VERS":
                payload = pkt[CMD_LEN:CMD_LEN + struct.unpack(">I", pkt[8:12])[0]]
                nul = payload.find(b"\0")
                return payload[:nul if nul >= 0 else None].decode("latin-1")
        return None

    def read(self, address, size):
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
        return None


def show(raw):
    w = struct.unpack("<40I", raw)
    magic, ticks, idx, spc, ssr, intevt, wd_limit, wd_fired = w[:8]
    ring = w[8:8 + RING]

    if magic != MAGIC:
        print(f"no sampling block at 0x{BLOCK_ADDR:08x} "
              f"(magic 0x{magic:08x}, want 0x{MAGIC:08x})")
        print("  the loader was not built with GUEST_TICK=1, or never launched a title")
        return False

    print(f"ticks    : {ticks}")
    print(f"watchdog : limit {wd_limit}, fired {wd_fired}"
          + ("   <- the title was not running" if wd_fired else ""))
    print(f"last SPC : 0x{spc:08x}    SSR 0x{ssr:08x}"
          f"   (RB={(ssr >> 29) & 1} BL={(ssr >> 28) & 1} IMASK={(ssr >> 4) & 15})")
    print(f"INTEVT   : 0x{intevt:03x}  {INTEVT.get(intevt, '?')}")

    if ticks == 0:
        print("\nno samples: the timer never fired. Either the title masked it "
              "(SR.BL/IMASK) or it replaced our vector table.")
        return True

    seen = {}
    n = min(ticks, RING)
    order = [(idx - 1 - i) % RING for i in range(n)]
    for slot in order:
        seen[ring[slot]] = seen.get(ring[slot], 0) + 1

    print(f"\nlast {n} sampled PCs, most frequent first:")
    for pc, count in sorted(seen.items(), key=lambda kv: -kv[1]):
        print(f"  0x{pc:08x}  x{count}{'':4}{where(pc)}")
    if len(seen) <= 3:
        print("\n  a handful of addresses over the whole ring is a SPIN LOOP, "
              "and those are its instructions.")
    return True


def where(pc):
    """Whose code is this? Coarse, but it answers the first question asked."""
    a = pc & 0x1FFFFFFF
    if a < 0x0C000000:
        return "<- not in RAM (BIOS/ROM or a wild jump)"
    if a < 0x0C004000:
        return "<- BIOS syscall area"
    if a < 0x0C010000:
        return "<- the loader, or its vector table"
    return "<- the title"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="Dreamcast IP, optionally IP:PORT")
    ap.add_argument("--watch", action="store_true",
                    help="re-read once a second until interrupted")
    args = ap.parse_args()

    host, _, port = args.target.partition(":")
    dc = Dc(host, int(port) if port else DEFAULT_PORT)

    version = dc.alive()
    if version is None:
        sys.exit("no answer to VERS -- the loader is not servicing the network.\n"
                 "If a title is running, wait for the watchdog to fire and retry.")
    print(f"loader   : {version}\n")

    while True:
        raw = dc.read(BLOCK_ADDR, BLOCK_LEN)
        if raw is None:
            sys.exit(f"incomplete read at 0x{BLOCK_ADDR:08x}")
        if not show(raw) or not args.watch:
            break
        print("-" * 60)
        time.sleep(1.0)


if __name__ == "__main__":
    main()
