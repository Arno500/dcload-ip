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
"""
import argparse
import socket
import struct
import sys
import zlib

VRAM64 = 0xA5000000
FB_R_SOF1 = 0xA05F8050
FB_R_CTRL = 0xA05F8044
FB_R_SIZE = 0xA05F805C

FORMATS = {0: "RGB555", 1: "RGB565", 2: "RGB888", 3: "RGB0888"}


def rsp(p):
    return b"+$" + p.encode() + b"#%02x" % (sum(p.encode()) & 0xFF)


def read_reply(sock):
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        c = sock.recv(65536)
        if not c:
            break
        buf += c
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")


def read_mem(sock, addr, n):
    sock.sendall(rsp(f"m{addr:x},{n}"))
    reply = read_reply(sock)
    try:
        return bytes.fromhex(reply)[:n]
    except ValueError:
        return b""


def read32(sock, addr):
    b = read_mem(sock, addr, 4)
    return int.from_bytes(b, "little") if len(b) == 4 else None


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=20)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
        return 1

    with sock:
        sock.settimeout(20)
        try:
            sof1 = read32(sock, FB_R_SOF1)
            ctrl = read32(sock, FB_R_CTRL)
            size = read32(sock, FB_R_SIZE)
            if None in (sof1, ctrl, size):
                print("could not read the PVR framebuffer registers", file=sys.stderr)
                return 1

            fmt = (ctrl >> 2) & 3
            enabled = ctrl & 1
            xw = (size & 0x3FF) + 1          # in 32-bit words
            yh = ((size >> 10) & 0x3FF) + 1
            modulus = (size >> 20) & 0x3FF
            bpp = {0: 2, 1: 2, 2: 3, 3: 4}[fmt]
            width = args.width or (xw * 4) // bpp
            height = args.height or yh
            print(f"FB_R_CTRL={ctrl:#010x} enable={enabled} format={FORMATS[fmt]}")
            print(f"FB_R_SOF1={sof1:#010x}  FB_R_SIZE={size:#010x} "
                  f"(xw={xw} words, yh={yh}, modulus={modulus})")
            print(f"-> reading {width}x{height} at {bpp} bytes/pixel")
            if not enabled:
                print("NOTE: framebuffer read is DISABLED; the image may be stale.",
                      file=sys.stderr)

            stride = width * bpp
            rows = []
            for y in range(height):
                addr = VRAM64 + sof1 + y * (stride + (modulus - 1) * 4)
                line = b""
                while len(line) < stride:
                    want = min(1024, stride - len(line))
                    part = read_mem(sock, addr + len(line), want)
                    if len(part) != want:
                        break
                    line += part
                if len(line) < stride:
                    print(f"short read on row {y}", file=sys.stderr)
                    break
                out = bytearray()
                if fmt == 1:      # RGB565
                    for i in range(0, stride, 2):
                        v = line[i] | (line[i + 1] << 8)
                        out += bytes((((v >> 11) & 31) << 3,
                                      ((v >> 5) & 63) << 2,
                                      (v & 31) << 3))
                elif fmt == 0:    # RGB555
                    for i in range(0, stride, 2):
                        v = line[i] | (line[i + 1] << 8)
                        out += bytes((((v >> 10) & 31) << 3,
                                      ((v >> 5) & 31) << 3,
                                      (v & 31) << 3))
                elif fmt == 2:    # RGB888, stored BGR
                    for i in range(0, stride, 3):
                        out += bytes((line[i + 2], line[i + 1], line[i]))
                else:             # RGB0888
                    for i in range(0, stride, 4):
                        out += bytes((line[i + 2], line[i + 1], line[i]))
                rows.append(bytes(out))

            if not rows:
                print("no rows read", file=sys.stderr)
                return 1
            with open(args.output, "wb") as fh:
                fh.write(png(width, len(rows), rows))
            print(f"wrote {args.output} ({width}x{len(rows)})")
        finally:
            # Always detach, or flycast stays halted and looks frozen.
            try:
                sock.sendall(rsp("D"))
                read_reply(sock)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
