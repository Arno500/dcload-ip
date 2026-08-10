#!/usr/bin/env python3
"""Is dcload's own memory still dcload's? Byte-for-byte, over GDB.

WHY THIS EXISTS. Every other instrument here watches the *data path*: how many
payloads arrived, whether the frontier is contiguous, whether the delivered bytes
match what was sent (--verify-reads). None watches dcload's OWN code. So "the
title wrote over the loader" was invisible to all of them, and it is a class we
had concrete reason to suspect: Sonic Adventure's allocator hands out buffers
downward from a ceiling that is exactly 0x0cf00000 -- dcload's first byte. 160
CDFS reads across 11 sessions end exactly there and not one byte past. We live
on its doorstep with zero margin.

`selfwr` only proves no *CDFS* write targets us (dcload_owns_range refuses those).
This checks everything else the title can do.

WHAT IT FOUND, first run, on a live freeze (2026-08-07):
  - .text intact except 8 bytes at 0x8cf00000 -- `start:` and the 0xdeadbeef
    magic. Boot-only code and a sentinel only example programs read, so
    harmless, but it is proof the title writes AT our base address.
  - exception.bin at VBR+0x100/+0x400/+0x600 replaced by ONE handler prologue,
    identical at all three. The title installed its own exception dispatcher
    into the VBR dcload left it -- expected co-habitation, not damage.
  - Everything that runs during gameplay: byte-identical.
So the freeze is NOT the title destroying dcload.

WHAT IT COMPARES, AND WHAT IT DELIBERATELY DOES NOT.

  .text      Compared. Immutable code -- EXCEPT a few words that the .s files
             place inline (cfs_saved) and dcload writes at runtime. Those are
             named in BENIGN below, reported and not counted.
  .rodata    Compared, non-fatal. NOT actually read-only here: dcload paints the
             live IP and MAC into its string literals for the on-screen display.
  .data/.bss Skipped. Mutable by design; diffing them drowns the signal.
  .transient Compared, non-fatal. The linker script says outright these MAY be
             clobbered once the game runs. Calling an expected event a failure
             would train us to ignore the one that matters.
  exception  Compared against exception.bin, non-fatal, for the reason above.

For a verdict with no benign-list to maintain, take a baseline once the title is
running and diff against that instead -- it absorbs every legitimate write
automatically:

  scripts/dc-integrity.py --save-baseline /tmp/dc.base   # early, game running
  scripts/dc-integrity.py --baseline /tmp/dc.base --watch 5

Run it DURING a freeze. That is the moment the answer is worth having.
"""
import argparse
import bisect
import importlib.util
import pickle
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("dcpeek", HERE / "dc-peek.py")
peek = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(peek)

EXCEPTION_BIN = peek.REPO / "target-src" / "dcload" / "exception.bin"
EXCEPTION_ELF = peek.REPO / "target-src" / "dcload" / "exception"


def exception_base():
    """Where exception.bin is actually linked, read from its own ELF.

    This used to be the literal 0x8CF0B400. After dcload moved back to the low
    base the constant still pointed at the old high address, so every run
    reported hundreds of differences in memory dcload does not own -- an
    instrument that cries wolf, which is worse than no instrument. Derive it,
    the way everything that depends on the link map should.
    """
    objdump = Path(str(peek.NM).replace("-nm", "-objdump"))
    try:
        out = subprocess.run([str(objdump), "-h", str(EXCEPTION_ELF)],
                             capture_output=True, text=True).stdout
    except Exception:
        return None
    # Section headers, not symbols: exception carries a .stack section pinned
    # at 0x3fffff00, and a lowest-symbol scan happily returns that instead.
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == ".text":
            return int(parts[3], 16)
    return None

# Words that live inside .text because the .s files put them there, and that
# dcload writes at runtime. A difference here is correct behaviour, not damage.
BENIGN = {"cfs_saved"}

# The stub answers one `m` per round trip; 1 KB keeps each packet small while
# holding a full sweep to a few dozen round trips.
CHUNK = 1024


def sections():
    """(name, addr, size) for the allocated sections, from the ELF itself.

    Never hard-coded: dc-freeze.py once had a stride written down, the struct
    grew, and a healthy table read as corruption for a whole session.
    """
    objdump = peek.NM.with_name("sh-elf-objdump")
    out = subprocess.run([str(objdump), "-h", str(peek.ELF)],
                         capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            try:
                found[p[1]] = (int(p[3], 16), int(p[2], 16))
            except ValueError:
                pass
    return found


def read_mem(sock, addr, length):
    """Guest memory, or None. Short answers are refused, never padded."""
    out = bytearray()
    while length:
        n = min(CHUNK, length)
        sock.sendall(peek.rsp(f"m{addr + len(out):x},{n}"))
        try:
            raw = bytes.fromhex(peek.read_reply(sock))
        except ValueError:
            return None
        # dc-peek documents this stub answering MORE than asked; trust the
        # prefix only, and treat too-few as unreadable rather than guessing.
        if len(raw) < n:
            return None
        out.extend(raw[:n])
        length -= n
    return bytes(out)


def symbol_index(syms):
    pairs = sorted((a, n) for n, a in syms.items())
    return [a for a, _ in pairs], [n for _, n in pairs]


def name_for(addrs, names, addr):
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return "?"
    return f"{names[i]}+{addr - addrs[i]:#x}" if addr != addrs[i] else names[i]


def diff_runs(want, got, base):
    runs, start = [], None
    for i in range(len(want)):
        if want[i] != got[i]:
            if start is None:
                start = i
        elif start is not None:
            runs.append((base + start, i - start))
            start = None
    if start is not None:
        runs.append((base + start, len(want) - start))
    return runs


def report(label, base, want, got, addrs, names, fatal):
    """True when intact (or benign). Unreadable is not a claim either way."""
    end = base + len(want)
    if got is None:
        print(f"  {label:22} <target silent; says nothing about the data>")
        return True
    runs = diff_runs(want, got, base)
    if not runs:
        print(f"  {label:22} {base:08x}..{end:08x} {len(want):6} B  INTACT")
        return True

    hard = [(a, n) for a, n in runs
            if name_for(addrs, names, a).split("+")[0] not in BENIGN]
    nb = len(runs) - len(hard)
    tag = ("CLOBBERED" if hard else "benign only") if fatal else "differs"
    print(f"  {label:22} {base:08x}..{end:08x} {len(want):6} B  {tag}: "
          f"{sum(n for _, n in runs)} B in {len(runs)} run(s)"
          + (f", {nb} benign" if nb else ""))
    for a, n in runs[:14]:
        o = a - base
        who = name_for(addrs, names, a)
        mark = "  (benign)" if who.split("+")[0] in BENIGN else ""
        print(f"      {a:08x} +{n:<5} {who}{mark}")
        print(f"        want {want[o:o + 12].hex(' ')}")
        print(f"        got  {got[o:o + 12].hex(' ')}")
    if len(runs) > 14:
        print(f"      ... and {len(runs) - 14} more run(s)")
    return not (fatal and hard)


def gather(sock, secs):
    """The regions worth comparing: (label, base, reference, fatal)."""
    out = []
    for name, fatal in ((".text", True), (".rodata", False),
                        (".transient.text", False)):
        if name not in secs:
            continue
        addr, size = secs[name]
        ref = peek.elf_bytes_at(addr, size)
        if ref is not None and len(ref) >= size:
            out.append((name, addr, ref[:size], fatal))
    base = exception_base()
    if EXCEPTION_BIN.exists() and base:
        out.append(("exception.bin", base, EXCEPTION_BIN.read_bytes(), False))
    return out


def run(host, port, no_verify, save_baseline, baseline):
    syms = peek.symbol_table()
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
        return 1

    ok = True
    with sock:
        sock.settimeout(15)
        if not no_verify and not peek.verify_image(sock, syms):
            sock.sendall(peek.rsp("D"))
            peek.read_reply(sock)
            return 2

        regions = gather(sock, sections())
        base_img = pickle.loads(baseline.read_bytes()) if baseline else None
        addrs, names = symbol_index(syms)

        if save_baseline:
            snap = {}
            for label, addr, ref, _ in regions:
                got = read_mem(sock, addr, len(ref))
                if got is None:
                    print(f"baseline aborted: {label} unreadable", file=sys.stderr)
                    ok = False
                    break
                snap[label] = (addr, got)
            if ok:
                save_baseline.write_bytes(pickle.dumps(snap))
                print(f"baseline of {len(snap)} region(s) -> {save_baseline}")
        else:
            src = "baseline" if base_img else "the ELF"
            print(f"--- dcload image integrity (vs {src}) ---")
            for label, addr, ref, fatal in regions:
                if base_img is not None:
                    if label not in base_img:
                        continue
                    addr, ref = base_img[label]
                    fatal = True  # a baseline has no legitimate drift in it
                got = read_mem(sock, addr, len(ref))
                ok &= report(label, addr, ref, got, addrs, names, fatal)

        # ALWAYS detach: a closed socket leaves flycast halted, which looks
        # exactly like the freeze under investigation.
        try:
            sock.sendall(peek.rsp("D"))
            peek.read_reply(sock)
        except OSError:
            pass
    return 0 if ok else 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--watch", type=float, default=0.0,
                    help="re-check every N seconds; stops on the first clobber")
    ap.add_argument("--save-baseline", type=Path,
                    help="snapshot the regions now instead of comparing")
    ap.add_argument("--baseline", type=Path,
                    help="compare against a snapshot instead of the ELF")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    if args.watch <= 0 or args.save_baseline:
        return run(args.host, args.port, args.no_verify,
                   args.save_baseline, args.baseline)
    try:
        while True:
            print("=" * 68, time.strftime("%H:%M:%S"))
            rc = run(args.host, args.port, args.no_verify, None, args.baseline)
            if rc == 3:
                print("\nSTOPPING: dcload's own image has changed.")
                return 3
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
