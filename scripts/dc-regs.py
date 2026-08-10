#!/usr/bin/env python3
"""Dump the Dreamcast MMIO registers that decide WHERE a write lands.

Motivation: the terminal state on Sonic Adventure is ~48 KB of TA parameter
stream (0xE0000000/0xF0000000 command words, 32-byte stride) sitting at
0x8c000000 instead of the TA FIFO, which destroys the BIOS syscall vector table
at 0x8c0000bc. The open question is who aimed it there.

flycast's GDB stub implements NO watchpoints (Z2/Z3/Z4 all answer empty) and its
UBC is register storage with no compare logic, so the write cannot be trapped as
it happens. What CAN be done is read the registers that select the destination:
a mis-aimed transfer leaves its aim behind.

  QACR0/QACR1   store-queue destination, bits [4:2] -> physical addr [28:26].
                0x10 -> 0x10000000 (TA FIFO).  0x0C -> 0x0C000000 (RAM!).
  SB_C2DSTAT    CH2-DMA destination. 0x10000000 is the TA FIFO.
  SH4 DMAC ch2  SAR2/DAR2/DMATCR2/CHCR2 -- DAR2 is where a DMA was writing, and
                DMATCR2 how much was left. These are NOT reset after a transfer,
                so they are the closest thing to a post-mortem of the last DMA.
  TA_*          the TA's own output pointers; if these pointed into low RAM the
                PVR itself would be the writer and no CPU trap would ever fire.

Reads go through ReadMem32_nommu, i.e. the real register handlers. These are
config/status registers, not FIFOs, so reading them is side-effect free.

Usage:
  scripts/dc-regs.py                # dump everything
  scripts/dc-regs.py --raw 0xa05f8124 --len 4
"""
import argparse
import socket
import sys

# name, address, one-line meaning
REGISTERS = [
    ("--- store queues (CPU -> TA path) ---", None, ""),
    ("QACR0",        0xFF000038, "SQ0 dest sel; 0x10=TA FIFO, 0x0C=RAM@0x0C000000"),
    ("QACR1",        0xFF00003C, "SQ1 dest sel; same encoding"),
    ("MMUCR",        0xFF000010, "MMU on/off (SQ translation differs when on)"),

    ("--- CH2 DMA (Holly side) ---", None, ""),
    ("SB_C2DSTAT",   0xA05F6800, "CH2-DMA destination; 0x10000000 = TA FIFO"),
    ("SB_C2DLEN",    0xA05F6804, "CH2-DMA length"),
    ("SB_C2DST",     0xA05F6808, "CH2-DMA start/status"),
    ("SB_LMMODE0",   0xA05F6884, "area1 access mode, TA FIFO vs direct texture"),
    ("SB_LMMODE1",   0xA05F6888, "same, second window"),

    ("--- SH4 DMAC channel 2 (the CH2-DMA engine itself) ---", None, ""),
    ("SAR2",         0xFFA00020, "source address, POST-transfer value"),
    ("DAR2",         0xFFA00024, "DESTINATION address -- the key reading"),
    ("DMATCR2",      0xFFA00028, "transfer count remaining"),
    ("CHCR2",        0xFFA0002C, "channel control/status (bit0 DE, bit1 TE)"),
    ("DMAOR",        0xFFA00040, "DMA operation register (master enable)"),

    ("--- TA / PVR output pointers (PVR as writer) ---", None, ""),
    ("PARAM_BASE",   0xA05F8020, "ISP/TSP parameter base in VRAM"),
    ("REGION_BASE",  0xA05F802C, "region array base in VRAM"),
    ("TA_OL_BASE",   0xA05F8124, "object list write base"),
    ("TA_ISP_BASE",  0xA05F8128, "ISP/TSP write base"),
    ("TA_OL_LIMIT",  0xA05F812C, "object list limit"),
    ("TA_ISP_LIMIT", 0xA05F8130, "ISP/TSP limit"),
    ("TA_NEXT_OPB",  0xA05F8134, "next object pointer block"),
    ("TA_ITP_CURRENT", 0xA05F8138, "current ISP/TSP write pointer"),
    ("TA_ALLOC_CTRL", 0xA05F813C, "OPB allocation control"),

    ("--- the victim, and its neighbourhood ---", None, ""),
    ("vector@0x8c0000bc", 0x8C0000BC, "BIOS GD syscall vector; should be dcload's"),
    ("word@0x8c000000",  0x8C000000, "start of the stray parameter stream"),
]


def rsp(payload: str) -> bytes:
    return b"+$" + payload.encode() + b"#%02x" % (sum(payload.encode()) & 0xFF)


def read_reply(sock) -> str:
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    body = buf.split(b"$", 1)[-1].split(b"#", 1)[0]
    return body.decode(errors="replace")


def read32(sock, addr):
    sock.sendall(rsp(f"m{addr:x},4"))
    reply = read_reply(sock)
    try:
        raw = bytes.fromhex(reply)[:4]
    except ValueError:
        return None
    if len(raw) < 4:
        return None
    return int.from_bytes(raw, "little")


def qacr_meaning(value):
    """QACR bits [4:2] supply physical address bits [28:26] for store queues."""
    area = (value >> 2) & 0x7
    dest = area << 26
    note = {0x10000000: "TA FIFO (correct)", 0x0C000000: "SYSTEM RAM (!!)"}
    return f"-> writes reach {dest:#010x}  {note.get(dest, '')}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--raw", help="read this address instead of the table")
    ap.add_argument("--len", type=int, default=4)
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
        return 1

    with sock:
        sock.settimeout(5)
        if args.raw:
            addr = int(args.raw, 16)
            sock.sendall(rsp(f"m{addr:x},{args.len}"))
            print(f"{addr:#010x}: {read_reply(sock)}")
        else:
            for name, addr, meaning in REGISTERS:
                if addr is None:
                    print(f"\n{name}")
                    continue
                value = read32(sock, addr)
                if value is None:
                    print(f"  {name:<18} @ {addr:#010x} = <read failed>")
                    continue
                line = f"  {name:<18} @ {addr:#010x} = {value:#010x}"
                if name.startswith("QACR"):
                    line += f"  {qacr_meaning(value)}"
                elif meaning:
                    line += f"  {meaning}"
                print(line)

        # ALWAYS detach: flycast halts the emulation while a debugger is
        # attached, and closing the socket alone leaves it halted.
        try:
            sock.sendall(rsp("D"))
            read_reply(sock)
        except OSError:
            print("WARNING: detach failed, flycast may be left halted", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
