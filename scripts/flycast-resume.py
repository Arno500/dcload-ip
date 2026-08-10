#!/usr/bin/env python3
"""Resume a flycast instance that is halted waiting on its GDB stub.

flycast built with -DENABLE_GDB_SERVER=ON starts the emulation SUSPENDED and
will sit there forever until a debugger resumes it -- setting
`Debug.GDBWaitForConnection = no` in emu.cfg does NOT prevent this in practice.
Nothing appears in flycast.log past "REIOS: Booting up", which reads exactly
like a hang; it isn't, it just needs a resume.

Sending a GDB "detach" (`$D#44`) is the cheapest way to do it: flycast's
handler calls DebugAgent::detach() -> emu.start(), i.e. plain resume, and then
we disconnect so :3263 stays free for a real debugging session later.

Usage: scripts/flycast-resume.py [host] [port] [connect_timeout]
Exit status 0 = resume acknowledged.

The connect timeout defaults to a fraction of a second, and deliberately so.
WSL2 runs in `networkingMode=Mirrored` with `firewall=true` on this host, so
127.0.0.1 does reach Windows' loopback -- but the Hyper-V firewall DROPS SYNs to
closed Windows ports instead of refusing them. A closed port therefore costs a
full connect timeout rather than an instant ECONNREFUSED, so a generous timeout
turns every too-early attempt into a multi-second stall (this used to be 5s, and
the caller retried it up to 20 times before the stub had even bound its socket).
A stub that IS listening on loopback accepts in about a millisecond, so prefer
retrying a short timeout over waiting on a long one.
"""
import socket
import sys

DEFAULT_CONNECT_TIMEOUT = 0.5


def rsp(payload: str) -> bytes:
    """Wrap a payload in the GDB remote serial protocol framing."""
    checksum = sum(payload.encode()) & 0xFF
    return b"+$" + payload.encode() + b"#%02x" % checksum


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3263
    connect_timeout = float(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_CONNECT_TIMEOUT

    try:
        sock = socket.create_connection((host, port), timeout=connect_timeout)
    except OSError as exc:
        print(f"cannot reach flycast GDB stub at {host}:{port}: {exc}", file=sys.stderr)
        return 1

    # Separate, longer budget for the reply: reaching this point proves something
    # is listening, so we are no longer at risk of paying for a black-holed SYN.
    with sock:
        sock.sendall(rsp("D"))
        sock.settimeout(5)
        try:
            reply = sock.recv(64)
        except socket.timeout:
            reply = b""

    # flycast answers "+$OK#9a". Any reply containing OK means it took.
    if b"OK" in reply:
        print("flycast resumed")
        return 0
    print(f"unexpected reply from stub: {reply!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
