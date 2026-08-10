#!/usr/bin/env python3
"""Sample the guest PC/PR over the GDB stub, then ALWAYS detach.

Answers the one question no dcload counter can: what code is the TITLE actually
executing. Sample fast (50ms) -- at 400ms the samples scatter across a frame and
hide a hot loop that a tight interval makes obvious.

ALWAYS detaches: flycast halts the guest while a debugger is attached, so a
script that exits without D leaves the machine looking exactly like the freeze
being investigated."""
import socket, sys, time, collections

def rsp(p): return b"+$" + p.encode() + b"#%02x" % (sum(p.encode()) & 0xFF)

def read_reply(s):
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        c = s.recv(8192)
        if not c: break
        buf += c
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")

def sample():
    try:
        s = socket.create_connection(("127.0.0.1", 3263), timeout=4)
    except OSError:
        return None
    with s:
        s.settimeout(4)
        try:
            s.sendall(rsp("g"))
            g = read_reply(s)
        except OSError:
            g = ""
        try:
            s.sendall(rsp("D")); read_reply(s)
        except OSError:
            pass
    if len(g) < 17 * 8:
        return None
    w = [int.from_bytes(bytes.fromhex(g[i*8:(i+1)*8]), "little") for i in range(24)]
    return w

print("  #   pc         pr         r15(sp)")
seen = {}
for i in range(10):
    w = sample()
    if not w:
        print(f"  {i}   <stub injoignable>"); time.sleep(0.4); continue
    pc, pr, r15 = w[16], w[17], w[15]
    seen[pc] = seen.get(pc, 0) + 1
    print(f"  {i}   0x{pc:08x} 0x{pr:08x} 0x{r15:08x}")
    time.sleep(0.4)
print("\nPC distincts:", " ".join(f"0x{p:08x}(x{c})" for p, c in sorted(seen.items(), key=lambda kv: -kv[1])))
