#!/usr/bin/env python3
"""Grab what the Dreamcast is actually displaying, by reading the framebuffer
out of guest VRAM over flycast's GDB stub.

WHY: taking a screenshot of the flycast window is a dead end here (Vulkan
surface -- PrintWindow fails, CopyFromScreen catches the always-on-top
terminal, and there is a 125% DPI offset on top). But the framebuffer the PVR
scans out lives in guest VRAM, and the GDB stub can read guest memory. So
instead of photographing the window, read the pixels the console is scanning
out. That answers "where is the title stuck?" -- a question no counter can.

The PVR tells us where and how:
  FB_R_SOF1  0x005f8050  framebuffer start, byte offset into VRAM
  FB_R_CTRL  0x005f8044  bit0 enable, bits 2-3 pixel format
  FB_R_SIZE  0x005f805c  x size (32-bit words-1) | y size-1 | modulus

Guest VRAM is read through the 64-bit area at 0xa5000000.

Output is a PNG written with zlib only -- no PIL dependency.

Usage:
  scripts/dc-screen.py out.png
  scripts/dc-screen.py out.png --width 640 --height 480   # force geometry
  scripts/dc-screen.py out.png --scale 4                  # quarter-size, ~4x less to read
  scripts/dc-screen.py out.png --repeat 8 --interval 0.4  # out-000.png .. out-007.png
  scripts/dc-screen.py --probe --repeat 20 --scale 4      # watch for a screen change
  scripts/dc-screen.py --probe                            # signature only, no file

WHERE THE TIME GOES (measured 2026-08-14, 640x480, one attach)

  connect + read the PVR registers      0.02 s
  read the framebuffer                  0.42 s   <- everything is here
  convert pixels                        0.05 s
  deflate the PNG                       0.03 s
  ------------------------------------------
  end to end                            0.66 s

So this tool was never the slow part of a capture loop: an orchestration that
wraps it in sleeps and background-task round trips costs far more than the
capture. If a screenshot loop feels slow, time THE LOOP before optimising here.

Two things nonetheless made the read four times heavier than it needed to be,
and both are fixed below.

FLYCAST'S STUB RETURNS MORE THAN YOU ASK FOR, AND THE EXTRA IS REAL.
`m addr,n` answers with roughly 4n bytes once n is over a kilobyte (measured:
16 -> 22, 512 -> 1298, 1024 -> 4132, 4096 -> 16534, saturating near 4.04x).
The surplus is not padding -- it is verified to be the genuine continuation of
guest memory, byte for byte, against reads issued at the corresponding
offsets. The old code asked for 1024 and threw away three quarters of what
came back, so it paid for the transfer four times over. The reader below
consumes whatever arrives and advances by that much, which exploits the quirk
where it exists and still terminates correctly against a stub that answers
exactly n.

ROWS ARE USUALLY CONTIGUOUS. When FB_R_SIZE's modulus field is 1 there is no
gap between scanlines, so the whole framebuffer is one flat range and there is
no reason to restart a request per row. Only the modulus != 1 case needs the
row-by-row walk.
"""
import argparse
import socket
import struct
import sys
import time
import zlib

VRAM64 = 0xA5000000
FB_R_SOF1 = 0xA05F8050
FB_R_CTRL = 0xA05F8044
FB_R_SIZE = 0xA05F805C

FORMATS = {0: "RGB555", 1: "RGB565", 2: "RGB888", 3: "RGB0888"}
BYTES_PER_PIXEL = {0: 2, 1: 2, 2: 3, 3: 4}

# The stub advertises PacketSize=4096. Asking for that much per request is the
# fewest round trips it will honour; the over-fetch then carries us further
# still.
REQUEST_BYTES = 4096


class Rsp:
    """Just enough GDB remote protocol to read memory.

    Packet framing is done properly here rather than by splitting on the first
    '$' and '#'. Hex payloads cannot contain either character, so the sloppy
    version worked -- but it silently glued packets together whenever more than
    one reply was in flight, which is exactly the shape a batched reader takes.
    """

    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def send(self, payload):
        b = payload.encode()
        self.sock.sendall(b"$" + b + b"#%02x" % (sum(b) & 0xFF))

    def recv(self):
        while True:
            while self.buf[:1] in (b"+", b"-"):
                self.buf = self.buf[1:]
            start = self.buf.find(b"$")
            if start >= 0:
                end = self.buf.find(b"#", start)
                if end >= 0 and len(self.buf) >= end + 3:
                    pkt = self.buf[start + 1:end]
                    self.buf = self.buf[end + 3:]
                    self.sock.sendall(b"+")
                    return pkt
            more = self.sock.recv(1 << 20)
            if not more:
                return b""
            self.buf += more

    def cmd(self, payload):
        self.send(payload)
        return self.recv()

    def read_block(self, addr, want):
        """One request. Returns what the stub gave, which may exceed `want`."""
        reply = self.cmd(f"m{addr:x},{want}")
        if not reply or reply[:1] == b"E":
            return b""
        try:
            return bytes.fromhex(reply.decode())
        except ValueError:
            return b""

    def read_range(self, addr, total):
        """Read `total` bytes, consuming the stub's over-fetch rather than
        discarding it. Advancing by len(got) is what makes this both fast here
        and correct against a stub that returns exactly what was asked."""
        out = bytearray()
        while len(out) < total:
            got = self.read_block(addr + len(out),
                                  min(REQUEST_BYTES, total - len(out)))
            if not got:
                break
            out += got
        return bytes(out[:total])

    def read32(self, addr):
        b = self.read_block(addr, 4)[:4]
        return int.from_bytes(b, "little") if len(b) == 4 else None

    def detach(self):
        # Always detach, or flycast stays halted and looks frozen.
        try:
            self.cmd("D")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# --- pixel conversion -------------------------------------------------------
#
# Slice assignment does the interleave in C rather than one bytes() object per
# pixel. For the 16-bit formats a 65536-entry table is built once per run and
# then reused for every row and every frame of a --repeat sweep.

_TABLE_CACHE = {}


def _table16(fmt):
    t = _TABLE_CACHE.get(fmt)
    if t is None:
        t = []
        for v in range(65536):
            if fmt == 1:    # RGB565
                t.append(bytes((((v >> 11) & 31) << 3,
                                ((v >> 5) & 63) << 2,
                                (v & 31) << 3)))
            else:           # RGB555
                t.append(bytes((((v >> 10) & 31) << 3,
                                ((v >> 5) & 31) << 3,
                                (v & 31) << 3)))
        _TABLE_CACHE[fmt] = t
    return t


def convert_row(line, fmt, width, step):
    """One scanline of guest pixels -> RGB8 bytes, taking every `step`th pixel."""
    if fmt == 3:            # RGB0888, stored BGRx
        src = line[:width * 4]
        out = bytearray(3 * width)
        out[0::3] = src[2::4]
        out[1::3] = src[1::4]
        out[2::3] = src[0::4]
    elif fmt == 2:          # RGB888, stored BGR
        src = line[:width * 3]
        out = bytearray(3 * width)
        out[0::3] = src[2::3]
        out[1::3] = src[1::3]
        out[2::3] = src[0::3]
    else:                   # RGB565 / RGB555
        table = _table16(fmt)
        vals = struct.unpack_from(f"<{width}H", line, 0)
        out = bytearray().join([table[v] for v in vals])
    if step == 1:
        return bytes(out)
    thinned = bytearray(3 * ((width + step - 1) // step))
    thinned[0::3] = out[0::3][::step]
    thinned[1::3] = out[1::3][::step]
    thinned[2::3] = out[2::3][::step]
    return bytes(thinned)


def png(width, height, rgb_rows):
    """Minimal RGB8 PNG."""
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))


def grab(rsp, args, verbose):
    """One capture. Returns (out_width, rows) or None."""
    sof1 = rsp.read32(FB_R_SOF1)
    ctrl = rsp.read32(FB_R_CTRL)
    size = rsp.read32(FB_R_SIZE)
    if None in (sof1, ctrl, size):
        print("could not read the PVR framebuffer registers", file=sys.stderr)
        return None

    fmt = (ctrl >> 2) & 3
    enabled = ctrl & 1
    xw = (size & 0x3FF) + 1              # in 32-bit words
    yh = ((size >> 10) & 0x3FF) + 1
    modulus = (size >> 20) & 0x3FF
    bpp = BYTES_PER_PIXEL[fmt]
    width = args.width or (xw * 4) // bpp
    height = args.height or yh
    stride = width * bpp
    step = max(1, args.scale)

    if verbose:
        print(f"FB_R_CTRL={ctrl:#010x} enable={enabled} format={FORMATS[fmt]}")
        print(f"FB_R_SOF1={sof1:#010x}  FB_R_SIZE={size:#010x} "
              f"(xw={xw} words, yh={yh}, modulus={modulus})")
        print(f"-> reading {width}x{height} at {bpp} bytes/pixel"
              + (f", scale 1/{step}" if step > 1 else ""))
        if not enabled:
            print("NOTE: framebuffer read is DISABLED; the image may be stale.",
                  file=sys.stderr)

    row_pitch = stride + (modulus - 1) * 4
    wanted_rows = list(range(0, height, step))

    if modulus == 1 and step == 1:
        # Contiguous: one sweep, no per-row requests at all.
        flat = rsp.read_range(VRAM64 + sof1, stride * height)
        lines = [flat[y * stride:(y + 1) * stride] for y in wanted_rows
                 if (y + 1) * stride <= len(flat)]
    else:
        lines = []
        for y in wanted_rows:
            line = rsp.read_range(VRAM64 + sof1 + y * row_pitch, stride)
            if len(line) < stride:
                print(f"short read on row {y}", file=sys.stderr)
                break
            lines.append(line)

    if not lines:
        print("no rows read", file=sys.stderr)
        return None
    rows = [convert_row(line, fmt, width, step) for line in lines]
    return (len(rows[0]) // 3, rows)


def signature(rows):
    """Cheap 'has the screen changed' fingerprint: mean luma and a digest.

    Meant for driving menus, where the question is only whether the last
    keypress did anything. Comparing digests costs nothing and, unlike opening
    the PNG, does not need a human or a vision model in the loop."""
    blob = b"".join(rows)
    return zlib.crc32(blob) & 0xFFFFFFFF, (sum(blob) // max(1, len(blob)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output", nargs="?", help="PNG path (omit with --probe)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--scale", type=int, default=1,
                    help="keep every Nth row and column (N=4 reads 4x less)")
    ap.add_argument("--repeat", type=int, default=1,
                    help="capture N frames, numbered out-000.png; re-attaches "
                         "per frame because flycast halts the guest while a "
                         "debugger stays connected")
    ap.add_argument("--interval", type=float, default=0.0,
                    help="seconds between frames of a --repeat sweep")
    ap.add_argument("--probe", action="store_true",
                    help="print the signature only; write no file")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    if not args.probe and not args.output:
        ap.error("an output path is required unless --probe is given")

    # ONE ATTACH PER FRAME, DELIBERATELY.
    #
    # flycast halts the guest for as long as a debugger is connected, so a
    # --repeat sweep that held the socket open would return the same frame
    # every time -- measured: six captures over one attach, six identical
    # CRCs, on a screen that was animating. Detaching between frames is what
    # lets the console run, and it is cheap: connect plus the three PVR
    # register reads is ~20 ms against ~110 ms for the framebuffer itself.
    # dc-track.py re-attaches per sample for exactly this reason.
    prev_crc = None
    for i in range(max(1, args.repeat)):
        if i and args.interval:
            time.sleep(args.interval)
        try:
            rsp = Rsp(args.host, args.port, args.timeout)
        except OSError as exc:
            print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
            return 1
        try:
            t0 = time.time()
            got = grab(rsp, args, verbose=(i == 0))
        finally:
            rsp.detach()
        if got is None:
            return 1
        out_w, rows = got
        crc, luma = signature(rows)
        mark = "" if prev_crc is None else ("  CHANGE" if crc != prev_crc else "  =")
        prev_crc = crc
        if args.probe:
            print(f"frame {i}: {out_w}x{len(rows)} crc={crc:08x} luma={luma:3d} "
                  f"({time.time()-t0:.2f}s){mark}")
            continue
        path = args.output
        if args.repeat > 1:
            stem, _, ext = path.rpartition(".")
            path = f"{stem or path}-{i:03d}.{ext or 'png'}"
        with open(path, "wb") as fh:
            fh.write(png(out_w, len(rows), rows))
        print(f"wrote {path} ({out_w}x{len(rows)}) crc={crc:08x} luma={luma:3d} "
              f"({time.time()-t0:.2f}s){mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
