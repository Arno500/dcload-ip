#!/usr/bin/env python3
"""Poll a few dcload counters continuously and print only when they change.

Why not dc-peek in a shell loop: dc-peek re-runs sh-elf-nm and re-opens the
socket every invocation, so a sample costs seconds. The events being chased
here last well under one, and the machine re-boots a moment later and zeroes
everything. This resolves symbols once, holds the connection open, and polls.

    scripts/dc-track.py g_lbin_count g_last_load_addr --secs 120

Always detaches: flycast halts the guest while a debugger is attached, so
exiting without D leaves the machine looking exactly like the freeze being
investigated.
"""
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

NM = Path("/opt/toolchains/dc/sh-elf/bin/sh-elf-nm")
ELF = Path(__file__).resolve().parent.parent / "target-src/dcload/dcload"


def rsp(payload: str) -> bytes:
    return b"+$" + payload.encode() + b"#%02x" % (sum(payload.encode()) & 0xFF)


def read_reply(sock, timeout=4.0) -> str:
    sock.settimeout(timeout)
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")


def symbols() -> dict:
    out = subprocess.run([str(NM), str(ELF)], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(parts[2].lstrip("_"), int(parts[0], 16))
    return table


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="+")
    ap.add_argument("--secs", type=float, default=120.0)
    ap.add_argument("--interval", type=float, default=0.15)
    ap.add_argument("--pc", action="store_true",
                    help="sample pc/pr/r15 instead of memory (names ignored)")
    args = ap.parse_args()

    syms = symbols()
    addrs = []
    if args.pc:
        args.names = ["pc", "pr", "r15"]
        addrs = [(n, 0) for n in args.names]
    for n in ([] if args.pc else args.names):
        a = int(n, 0) if n.startswith("0x") else syms.get(n)
        if a is None:
            print(f"unknown symbol {n}", file=sys.stderr)
            return 2
        addrs.append((n, a))

    # One connection PER SAMPLE, always detached again. flycast halts the guest
    # for as long as a debugger is attached, so holding the socket open freezes
    # the very thing being observed -- and the stub then drops the connection.
    # Keeping the symbol table in memory is what makes this cheap enough to
    # poll at 100 ms, which dc-peek (a fresh sh-elf-nm per call) cannot do.
    def sample():
        try:
            s = socket.create_connection(("127.0.0.1", 3263), timeout=4)
        except OSError:
            return None
        out = []
        with s:
            try:
                if args.pc:
                    s.sendall(rsp("g"))
                    g = read_reply(s)
                    if len(g) < 18 * 8:
                        return None
                    w = [int.from_bytes(bytes.fromhex(g[i * 8:(i + 1) * 8]), "little")
                         for i in range(18)]
                    out = [w[16], w[17], w[15]]   # pc, pr, r15
                    s.sendall(rsp("D"))
                    return out
                for _, a in addrs:
                    s.sendall(rsp(f"m{a:x},4"))
                    raw = read_reply(s)
                    try:
                        out.append(int.from_bytes(bytes.fromhex(raw[:8]), "little"))
                    except ValueError:
                        out.append(None)
            except OSError:
                return None
            finally:
                try:
                    s.sendall(rsp("D"))
                except OSError:
                    pass
        return out

    last = None
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < args.secs:
            vals = sample()
            if vals is None:
                time.sleep(args.interval)
                continue
            if vals != last:
                stamp = time.monotonic() - t0
                cells = " ".join(
                    f"{n}={v:#010x}" if v is not None else f"{n}=?"
                    for (n, _), v in zip(addrs, vals)
                )
                print(f"[{stamp:7.2f}] {cells}", flush=True)
                last = vals
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
