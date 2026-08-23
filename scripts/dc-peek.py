#!/usr/bin/env python3
"""Read Dreamcast memory through flycast's GDB stub, without running GDB.

Useful when the GDB tooling is unavailable or wedged, and for quick counter
polling: it opens a socket, issues one `m<addr>,<len>` packet per symbol and
disconnects, so it never leaves the emulation halted.

Symbols are resolved from the dcload ELF with sh-elf-nm, so you can ask for
names rather than addresses.

Usage:
  scripts/dc-peek.py g_rtl_rx_resyncs g_partbin_dropped
  scripts/dc-peek.py 0x8ce05c18 --len 4
"""
import argparse
import socket
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# The default build. Since the loader can be relinked per game and chainloaded
# (target-src/dcload/loaders/dcload-0x8cfe8000.elf and friends), this is only
# right when nothing has moved -- pass --elf for a relocated session, or every
# symbol resolves to an address the running image does not use. The image check
# below catches that, which is exactly what it is for.
ELF = REPO / "target-src" / "dcload" / "dcload"
NM = Path("/opt/toolchains/dc/sh-elf/bin/sh-elf-nm")


def symbol_table():
    out = subprocess.run([str(NM), str(ELF)], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
            # dcload's C symbols carry a leading underscore in the ELF.
            table.setdefault(parts[2].lstrip("_"), int(parts[0], 16))
    return table


def symbol_sizes() -> dict:
    """Symbol -> byte size, from `nm -S`.

    Needed so callers can DERIVE an array's element stride instead of writing it
    down: a slot table parsed at a stale 40-byte stride after the struct grew to
    44 once read as convincing garbage and was reported as memory corruption that
    did not exist. Symbols without a size are simply absent.
    """
    out = subprocess.run([str(NM), "-S", str(ELF)], capture_output=True, text=True,
                         check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            try:
                size = int(parts[1], 16)
            except ValueError:
                continue
            table[parts[3]] = size
            table.setdefault(parts[3].lstrip("_"), size)
    return table


def rsp(payload: str) -> bytes:
    return b"+$" + payload.encode() + b"#%02x" % (sum(payload.encode()) & 0xFF)


# --- Identity check: is the running image the one these symbols came from? ---
#
# Symbols are resolved from the ELF in the working tree, but the Dreamcast runs
# whatever was packaged into the CDI. Rebuild the ELF without repackaging and
# every address printed below is meaningless -- and the failure is SILENT and
# convincing: it prints plausible 32-bit numbers, because it is reading dcload's
# own .data and .rodata at the wrong offsets.
#
# That cost two full debugging sessions. The tell, in hindsight: counters that
# CANNOT be what they claim (g_partbin_accepted = 0 after hundreds of successful
# transfers, g_cdfs_credit_lost = 0x8100ffff) and, decisively, values IDENTICAL
# across two runs minutes apart -- a counter cannot do that. The CDI even had the
# exact same byte size as the fresh one, so nothing about it looked stale.
#
# So verify instead of hoping: read a word of CODE from the target and compare it
# with the same address in the ELF. Code is immutable and resident, so any
# mismatch means the running image is not this build.
# Several candidates, tried in order, because an anchor is a NAME and names go
# away: this list used to be the single symbol _cmd_partbin_commit, which was
# removed with the windowed-upload accounting, and from then on every dc-peek
# run printed "anchor not in ELF, cannot verify image" and read on regardless --
# i.e. the guard against reading a stale image was itself silently dead, which
# is exactly the failure mode the comment above describes. Anything in this list
# must be CODE (immutable and resident) and load-bearing enough that its
# disappearance would be noticed.
ANCHOR_SYMBOLS = ("_cmd_partbin", "_cmd_loadbin", "_cmd_execute", "_gdcServerMain")
ANCHOR_LEN = 16


def elf_bytes_at(addr: int, length: int) -> bytes | None:
    """The ELF's own contents at a virtual address, via objdump's hex dump."""
    objdump = NM.with_name("sh-elf-objdump")
    try:
        out = subprocess.run(
            [str(objdump), "-s", f"--start-address={addr:#x}",
             f"--stop-address={addr + length:#x}", str(ELF)],
            capture_output=True, text=True, check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    data = bytearray()
    for line in out.splitlines():
        parts = line.split()
        # " 8cf05730 0f4e2fe6 ...  ....."  -- address, then up to 4 hex groups.
        if len(parts) < 2:
            continue
        try:
            int(parts[0], 16)
        except ValueError:
            continue
        for group in parts[1:5]:
            if len(group) % 2 or not all(c in "0123456789abcdefABCDEF" for c in group):
                break
            data.extend(bytes.fromhex(group))
    return bytes(data[:length]) if len(data) >= length else None


def verify_image(sock, syms) -> bool:
    """True when the target's code matches this ELF at the anchor address."""
    addr = name = None
    for candidate in ANCHOR_SYMBOLS:
        addr = syms.get(candidate) or syms.get(candidate.lstrip("_"))
        if addr is not None:
            name = candidate
            break
    if addr is None:
        print(f"warning: no anchor of {ANCHOR_SYMBOLS} is in the ELF, "
              "cannot verify image", file=sys.stderr)
        return True
    want = elf_bytes_at(addr, ANCHOR_LEN)
    if want is None:
        print("warning: could not read anchor bytes from the ELF, cannot verify",
              file=sys.stderr)
        return True
    sock.sendall(rsp(f"m{addr:x},{ANCHOR_LEN}"))
    reply = read_reply(sock)
    try:
        got = bytes.fromhex(reply)
    except ValueError:
        print(f"warning: anchor read failed ({reply!r}), cannot verify image",
              file=sys.stderr)
        return True
    # Compare only the bytes we asked for. The stub has been observed answering
    # MORE than the requested length (22 bytes for 16), and treating trailing
    # framing as content would turn a correct deploy into a false MISMATCH --
    # which would be worse than the trap this check exists to close. Too FEW
    # bytes is not something to interpret at all.
    if len(got) < ANCHOR_LEN:
        print(f"warning: anchor read returned {len(got)} of {ANCHOR_LEN} bytes, "
              "cannot verify image", file=sys.stderr)
        return True
    got = got[:ANCHOR_LEN]
    if got == want:
        return True
    print(
        "MISMATCH: the running image is NOT this build.\n"
        f"  {name} @ {addr:08x}\n"
        f"    ELF says    {want.hex()}\n"
        f"    target says {got.hex()}\n"
        "  Every symbol address below would be wrong, so nothing is printed.\n"
        "  Repackage and redeploy the CDI (the loop script's --skip-build path\n"
        "  does NOT re-run mkdcdisc), then peek again. Pass --no-verify to\n"
        "  override, e.g. when reading raw addresses of a known-old image.",
        file=sys.stderr,
    )
    return False


def read_reply(sock) -> str:
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    body = buf.split(b"$", 1)[-1].split(b"#", 1)[0]
    return body.decode(errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="+", help="symbol names or 0x addresses")
    ap.add_argument("--len", type=int, default=4, help="bytes to read (default 4)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the running-image identity check (see verify_image)")
    ap.add_argument("--elf", default=None,
                    help="ELF to resolve symbols from. Give this when the host "
                         "has chainloaded a relinked loader, e.g. "
                         "target-src/dcload/loaders/dcload-0x8cfe8000.elf; the "
                         "default is the tree's own build at the stock base.")
    ap.add_argument("--base", default=None,
                    help="shorthand for --elf: a base address like 0x8cfe8000, "
                         "resolved to target-src/dcload/loaders/dcload-<base>.elf")
    args = ap.parse_args()

    global ELF
    if args.elf:
        ELF = Path(args.elf)
    elif args.base:
        ELF = REPO / "target-src" / "dcload" / "loaders" / f"dcload-{args.base}.elf"
    if not ELF.is_file():
        print(f"no such ELF: {ELF}", file=sys.stderr)
        return 1

    # The anchor is a symbol, so the table is needed for verification too.
    syms = symbol_table()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
        return 1

    rc = 0
    with sock:
        sock.settimeout(5)
        if not args.no_verify and not verify_image(sock, syms):
            # Still detach, or flycast stays halted. Same reason as below.
            try:
                sock.sendall(rsp("D"))
                read_reply(sock)
            except OSError:
                pass
            return 2
        for target in args.targets:
            if target.startswith("0x"):
                addr = int(target, 16)
            elif target in syms:
                addr = syms[target]
            else:
                print(f"{target}: unknown symbol", file=sys.stderr)
                rc = 1
                continue
            sock.sendall(rsp(f"m{addr:x},{args.len}"))
            hexdata = read_reply(sock)
            if not hexdata or hexdata.startswith("E"):
                print(f"{target} @ {addr:08x}: read failed ({hexdata!r})", file=sys.stderr)
                rc = 1
                continue
            raw = bytes.fromhex(hexdata)
            # SH4 here runs little-endian; show both raw bytes and the value.
            value = int.from_bytes(raw, "little")
            print(f"{target} @ {addr:08x} = {value} (0x{value:x}) bytes={raw.hex()}")

        # ALWAYS detach before closing. flycast halts the emulation while a
        # debugger is attached, and simply closing the socket leaves it halted
        # -- the machine then looks frozen (counters stop, pings fail, the host
        # tool times out) for reasons that have nothing to do with the guest.
        # `D` maps to DebugAgent::detach() -> emu.start(), i.e. resume.
        try:
            sock.sendall(rsp("D"))
            read_reply(sock)
        except OSError:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
