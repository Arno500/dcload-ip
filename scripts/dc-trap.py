#!/usr/bin/env python3
"""Break the guest at an address and print the SH4 context that got there.

WHY THIS EXISTS. flycast's REIOS logs "REIOS: Booting up" from a hook planted
at physical address 0 (reios.cpp, SYSCALL_ADDR(0xA0000000)). That line is
therefore NOT proof of a machine reset -- it is proof that the SH4 EXECUTED AT
ADDRESS ZERO, which is what a jump through a null pointer looks like. flycast
does not reset the CPU context to get there, so PR, R15 and the general
registers still describe whoever jumped. This catches that moment and reads
them, which names the caller directly.

    scripts/dc-trap.py 0xa0000000 [--wait 120]

SELF-TEST, AND WHY IT IS NOT OPTIONAL. Breakpoints here are software: flycast
writes `trapa #0x20` over the target opcode (debug_agent.h insertMatchpoint).
Address 0 is BIOS ROM, and a ROM that ignores the write would leave the
breakpoint silently unarmed -- so "nothing was caught" would mean nothing at
all. The script writes, reads back, and refuses to continue unless the opcode
actually changed.

Always detaches. A target left halted looks like a freeze to every other
instrument, and that mistake has been made here before.
"""
import argparse
import socket
import subprocess
import sys
from pathlib import Path

HOST, PORT = "127.0.0.1", 3263
NM = Path("/opt/toolchains/dc/sh-elf/bin/sh-elf-nm")
ELF = Path(__file__).resolve().parent.parent / "target-src/dcload/dcload"

# Register order is flycast's Sh4RegList (core/debug/debug_agent.h).
REGS = ([f"r{i}" for i in range(16)]
        + ["pc", "pr", "gbr", "vbr", "mach", "macl", "sr", "fpul", "fpscr"]
        + [f"fr{i}" for i in range(16)]
        + ["ssr", "spc"])


def rsp(payload: str) -> bytes:
    return b"+$" + payload.encode() + b"#%02x" % (sum(payload.encode()) & 0xFF)


def read_reply(sock, timeout=5.0) -> str:
    sock.settimeout(timeout)
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")


def cmd(sock, payload: str, timeout=5.0) -> str:
    sock.sendall(rsp(payload))
    return read_reply(sock, timeout)


def symbols() -> dict:
    out = subprocess.run([str(NM), str(ELF)], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(int(parts[0], 16), parts[2].lstrip("_"))
    return table


def name_of(syms: dict, addr: int) -> str:
    """Nearest preceding symbol. dcload lives at 0x8c00xxxx; a P2 alias of it
    resolves the same, so normalise the segment before looking up."""
    if addr == 0:
        return "NULL"
    probe = (addr & 0x1FFFFFFF) | 0x8C000000
    best = None
    for a, n in syms.items():
        if a <= probe and (best is None or a > best[0]):
            best = (a, n)
    if best is None or probe - best[0] > 0x4000:
        return "?"
    return f"{best[1]}+0x{probe - best[0]:x}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", help="address to trap, e.g. 0xa0000000")
    ap.add_argument("--wait", type=float, default=120.0, help="seconds to wait for the hit")
    args = ap.parse_args()
    addr = int(args.addr, 0)

    syms = symbols()
    sock = socket.create_connection((HOST, PORT), timeout=10)
    try:
        cmd(sock, "?")

        before = cmd(sock, f"m{addr:x},2")
        cmd(sock, f"Z0,{addr:x},2")
        after = cmd(sock, f"m{addr:x},2")
        if before == after:
            print(f"SELFTEST FAILED: opcode at {addr:#x} unchanged ({before!r}) after Z0.",
                  file=sys.stderr)
            print("The breakpoint is not armed; a miss would prove nothing. Aborting.",
                  file=sys.stderr)
            return 2
        print(f"SELFTEST-OK: {addr:#x} {before} -> {after} (armed)")

        sock.sendall(rsp("c"))
        try:
            stop = read_reply(sock, timeout=args.wait)
        except socket.timeout:
            print(f"no hit within {args.wait:.0f}s")
            return 1
        print(f"HIT: stop reply = {stop}")

        raw = cmd(sock, "g")
        vals = [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little")
                for i in range(0, min(len(raw), len(REGS) * 8), 8)]
        ctx = dict(zip(REGS, vals))

        print(f"  pc  = {ctx.get('pc', 0):08x}  {name_of(syms, ctx.get('pc', 0))}")
        print(f"  pr  = {ctx.get('pr', 0):08x}  {name_of(syms, ctx.get('pr', 0))}   <- who jumped")
        print(f"  r15 = {ctx.get('r15', 0):08x}   sr = {ctx.get('sr', 0):08x}"
              f"   vbr = {ctx.get('vbr', 0):08x}")
        print(f"  spc = {ctx.get('spc', 0):08x}  ssr = {ctx.get('ssr', 0):08x}")
        for i in range(0, 16, 4):
            print("  " + "  ".join(f"r{j:<2}={ctx.get(f'r{j}', 0):08x}" for j in range(i, i + 4)))

        # The return address chain often names the real caller when pr has
        # already been overwritten by the jump itself.
        stack = cmd(sock, f"m{ctx.get('r15', 0):x},64")
        if stack and not stack.startswith("E"):
            words = [int.from_bytes(bytes.fromhex(stack[i:i + 8]), "little")
                     for i in range(0, len(stack), 8)]
            print("  stack:")
            for k, w in enumerate(words):
                if 0x8C000000 <= ((w & 0x1FFFFFFF) | 0x8C000000) and w > 0x1000:
                    print(f"    [{k:02d}] {w:08x}  {name_of(syms, w)}")
        return 0
    finally:
        try:
            cmd(sock, f"z0,{addr:x},2")
            sock.sendall(rsp("D"))
        except Exception:
            pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
