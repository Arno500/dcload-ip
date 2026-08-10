#!/usr/bin/env python3
"""Log the interrupt-routing state continuously, so a freeze is caught in the act.

WHY THIS EXISTS. The 2026-08-07 freeze (see the agent memory
"sa-freeze-interrupt-storm") was diagnosed entirely from the state AFTER it
happened, and that turned out to be the limit of what could be concluded. The
frozen fingerprint is unambiguous -- SB_ISTNRM bit 12 (Maple DMA done) latched
forever, INTEVT stuck at 0x320, every PC sample in interrupt context, the game's
main thread starved -- but the causal question is about the TRANSITION, and by
the time you look the transition is minutes in the past.

The control run settles what this is not. Booting the same GDI directly in
flycast, with no dcload and no host tool, is healthy and stays healthy: bit 12
never latches, INTEVT alternates 0x320/0x360 (0x360 dominating, it is SA's own
DMA bank), and the CPU is in the main thread. SA programs IDENTICAL IML masks in
both runs, so dcload is not changing the interrupt routing -- what changes is
which line ends up asserted. So the thing worth recording is the moment 0x360
stops being delivered.

  scripts/dc-irqwatch.py                       # log to stdout + the default file
  scripts/dc-irqwatch.py --period 3 --out /tmp/irq.log
  scripts/dc-irqwatch.py --no-verify           # target is not a dcload build

WHAT IT DOES. One connection per sample, one pass, always followed by a GDB
detach -- a script that exits without D leaves flycast halted, which then looks
exactly like the freeze under investigation. It samples slowly while the title is
making progress, and when progress stops it says so and switches to a dense
burst, so the log holds both the run-up and the transition.

FREEZE TEST -- TWO triggers, because there are two terminal states.
  (a) STARVED: cdfs_queue_next stopped for --freeze-secs while the frame counter
      still advances. Frames alone are not enough (rendering is driven from the
      ISR and survives a starved main thread), and a stalled read counter alone
      is not enough either (the title is idle between levels quite legitimately).
  (b) DEAD: reads AND frames AND g_gd_ticks all stopped. This is PC = 0xffd8001c,
      the title's trampoline jumping into on-chip register space after the BIOS
      vector table at 0x8c0000bc was overwritten.
An earlier version had only (a), and so silently MISSED every (b) freeze --
including one it was running through. A detector that can only see half the
failure modes reads as "nothing happened".

Counter addresses are resolved from the ELF, never written down: adding a counter
shifts .data and hard-coded addresses then read as plausible nonsense. The two
SA-specific addresses are the exception and are CLI-overridable -- they belong to
the game, not to dcload, so no symbol exists for them.
"""
import argparse
import importlib.util
import socket
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Reuse dc-peek's symbol table, RSP framing and image-identity check rather than
# duplicating them -- the verification in particular must not drift out of sync.
_spec = importlib.util.spec_from_file_location("dcpeek", HERE / "dc-peek.py")
peek = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(peek)

# dcload counters, by symbol name. Progress first, then the error counters that
# say whether the data path had anything to do with it.
COUNTERS = ["cdfs_queue_next", "cdfs_slots_in_use", "g_gd_ticks",
            "g_gd_stat_handle", "g_gd_stat_ret", "g_partbin_dropped",
            "g_cdfs_credit_lost", "g_rtl_rx_resyncs",
            # GD syscall occupancy (cdfs_syscalls.c). g_sc_nested is the one to
            # watch: it counts syscalls the title issued from a context where
            # one was already in flight, i.e. from its interrupt handler.
            "g_sc_depth", "g_sc_nested", "g_sc_nested_sp",
            "g_sc_max_ticks", "g_sc_max_idx", "g_sc_max_sp",
            "g_poll_deadline_hits", "g_poll_reentry_declined", "g_rx_reentered"]

# Holly interrupt banks. The pending banks are what latch; the IML banks decide
# whether a latched bit can reach the CPU at all.
ASIC = [("ISTNRM", 0xa05f6900), ("ISTEXT", 0xa05f6904), ("ISTERR", 0xa05f6908),
        ("IML2NRM", 0xa05f6910), ("IML2EXT", 0xa05f6914), ("IML2ERR", 0xa05f6918),
        ("IML4NRM", 0xa05f6920), ("IML4EXT", 0xa05f6924), ("IML4ERR", 0xa05f6928),
        ("IML6NRM", 0xa05f6930), ("IML6EXT", 0xa05f6934), ("IML6ERR", 0xa05f6938)]
# The video timing generator: the HBlank interrupt mode lives here, and a mode
# that fires every line instead of once a frame would be a storm all by itself.
SPG = [("SPG_HBLANK_INT", 0xa05f80c8), ("SPG_VBLANK_INT", 0xa05f80cc),
       ("SPG_CONTROL", 0xa05f80d0), ("SPG_STATUS", 0xa05f810c)]
MAPLE = [("MDSTAR", 0xa05f6c04), ("MDTSEL", 0xa05f6c10),
         ("MDEN", 0xa05f6c14), ("MDST", 0xa05f6c18)]

NRM_BITS = {3: "vblank-in", 4: "vblank-out", 5: "hblank", 11: "pvr-dma",
            12: "MAPLE-DMA-done", 13: "maple-vbl", 14: "gdrom-dma",
            15: "aica-dma", 19: "punchthru", 21: "ch2-dma",
            30: "EXT-agrege", 31: "ERR-agrege"}


def read_reply(s):
    buf = b""
    while b"#" not in buf or len(buf.split(b"#")[-1]) < 2:
        c = s.recv(65536)
        if not c:
            break
        buf += c
    return buf.split(b"$", 1)[-1].split(b"#", 1)[0].decode(errors="replace")


def rd(s, addr, n=4):
    s.sendall(peek.rsp(f"m{addr:x},{n}"))
    try:
        raw = bytes.fromhex(read_reply(s))
    except ValueError:
        return None
    # The stub has been seen answering more than asked; trust only the prefix.
    return int.from_bytes(raw[:n], "little") if len(raw) >= n else None


def sample(host, port, syms, args):
    """One full pass. Returns a dict, or None if the stub is unreachable."""
    try:
        s = socket.create_connection((host, port), timeout=4)
    except OSError:
        return None
    s.settimeout(4)
    out = {}
    try:
        # Rapid repeats: a bit that is set on every one of them is latched, not
        # merely caught mid-flight. This is the whole difference between "the
        # maple interrupt fires often" and "nobody is acknowledging it".
        out["nrm_rapid"] = [rd(s, 0xa05f6900) for _ in range(6)]
        out["asic"] = {n: rd(s, a) for n, a in ASIC}
        out["spg"] = {n: rd(s, a) for n, a in SPG}
        out["maple"] = {n: rd(s, a) for n, a in MAPLE}
        out["cnt"] = {}
        for name in COUNTERS:
            addr = syms.get("_" + name) or syms.get(name)
            if addr is not None:
                out["cnt"][name] = rd(s, addr)
        out["frame"] = rd(s, args.frame_addr)
        out["intevt"] = rd(s, args.intevt_addr)
        out["nest"] = rd(s, args.intevt_addr + 4)
        s.sendall(peek.rsp("g"))
        g = read_reply(s)
        if len(g) >= 23 * 8:
            w = [int.from_bytes(bytes.fromhex(g[i * 8:(i + 1) * 8]), "little")
                 for i in range(23)]
            out["pc"], out["pr"], out["r15"], out["sr"] = w[16], w[17], w[15], w[22]
        else:
            out["pc"] = out["pr"] = out["r15"] = out["sr"] = 0
    finally:
        # ALWAYS detach: closing the socket leaves flycast halted, which then
        # looks exactly like the freeze under investigation.
        try:
            s.sendall(peek.rsp("D"))
            read_reply(s)
        except OSError:
            pass
        s.close()
    return out


def fmt(v, w=8):
    return "?" * w if v is None else f"{v:0{w}x}"


def line(t, v):
    nrm = v["nrm_rapid"]
    latched = 0xffffffff
    for x in nrm:
        latched &= (x if x is not None else 0)
    return (f"{t:7.1f} pc={fmt(v['pc'])} sr={fmt(v['sr'])} "
            f"nrm={fmt(nrm[0] if nrm else None)} latch={fmt(latched)} "
            f"ext={fmt(v['asic']['ISTEXT'], 4)} err={fmt(v['asic']['ISTERR'], 4)} "
            f"intevt={fmt(v['intevt'], 4)} nest={v['nest']} "
            f"reads={v['cnt'].get('cdfs_queue_next')} "
            f"gdtick={v['cnt'].get('g_gd_ticks')} frame={v['frame']} "
            f"mdst={v['maple']['MDST']}")


def detail(v):
    o = ["  --- detail ---"]
    latched = 0xffffffff
    for x in v["nrm_rapid"]:
        latched &= (x if x is not None else 0)
    o.append("  ISTNRM x6 : " + " ".join(fmt(x) for x in v["nrm_rapid"]))
    if latched:
        o.append("  LATCHE    : " + ", ".join(
            f"{i}:{NRM_BITS.get(i, '?')}" for i in range(32) if latched >> i & 1))
    for group, items in (("asic", ASIC), ("spg", SPG), ("maple", MAPLE)):
        o.append("  " + "  ".join(f"{n}={fmt(v[group][n])}" for n, _ in items))
    o.append("  " + "  ".join(f"{k}={val}" for k, val in v["cnt"].items()))
    o.append(f"  pc={fmt(v['pc'])} pr={fmt(v['pr'])} r15={fmt(v['r15'])} "
             f"sr={fmt(v['sr'])} intevt={fmt(v['intevt'], 4)} nest={v['nest']}")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--period", type=float, default=3.0,
                    help="seconds between samples while the title progresses")
    ap.add_argument("--burst-period", type=float, default=0.3,
                    help="seconds between samples once a freeze is declared")
    ap.add_argument("--burst", type=int, default=60,
                    help="how many dense samples to take at the freeze")
    ap.add_argument("--freeze-secs", type=float, default=25.0,
                    help="no new sector read for this long (frames still moving)")
    ap.add_argument("--out", default="/tmp/dc-irqwatch.log")
    ap.add_argument("--no-verify", action="store_true")
    # SA's own addresses: its VBR stub stores the live INTEVT register here, and
    # the word after it is the ISR nesting depth. Not dcload symbols.
    ap.add_argument("--intevt-addr", type=lambda x: int(x, 0), default=0x8c65ea20)
    ap.add_argument("--frame-addr", type=lambda x: int(x, 0), default=0x8c8a2ffc)
    args = ap.parse_args()

    syms = peek.symbol_table()
    out = open(args.out, "a", buffering=1)

    def emit(text):
        print(text, flush=True)
        out.write(text + "\n")

    if not args.no_verify:
        try:
            s = socket.create_connection((args.host, args.port), timeout=5)
        except OSError as exc:
            print(f"cannot reach flycast GDB stub: {exc}", file=sys.stderr)
            return 1
        with s:
            s.settimeout(5)
            ok = peek.verify_image(s, syms)
            try:
                s.sendall(peek.rsp("D"))
                read_reply(s)
            except OSError:
                pass
        if not ok:
            print("refusing to log against a different build (--no-verify to force)",
                  file=sys.stderr)
            return 2

    emit(f"=== dc-irqwatch start, periode {args.period}s, seuil de gel "
         f"{args.freeze_secs}s ===")
    t0 = time.time()
    last_reads = None
    last_reads_t = t0
    last_frame = None
    last_gd = None
    frozen = False
    try:
        while True:
            v = sample(args.host, args.port, syms, args)
            t = time.time() - t0
            if v is None:
                emit(f"{t:7.1f} <stub injoignable>")
                time.sleep(args.period)
                continue
            reads = v["cnt"].get("cdfs_queue_next")
            frame = v["frame"]
            emit(line(t, v))

            if reads != last_reads:
                last_reads, last_reads_t = reads, time.time()
                if frozen:
                    emit("  *** REPRIS : le titre redemande des secteurs ***")
                    frozen = False
            frames_moving = last_frame is not None and frame != last_frame
            gd_moving = last_gd is not None and v["cnt"].get("g_gd_ticks") != last_gd
            last_frame = frame
            last_gd = v["cnt"].get("g_gd_ticks")

            stalled = time.time() - last_reads_t >= args.freeze_secs
            # (b) beats (a): if nothing at all moves, say DEAD rather than
            # STARVED -- they need different follow-up.
            kind = None
            if not frozen and stalled:
                kind = "AFFAME (frames vivantes)" if (frames_moving or gd_moving) \
                    else "MORT (frames ET syscalls GD arretes)"
            if kind:
                frozen = True
                emit(f"\n*** GEL DECLARE a t={t:.1f}s -- {kind} : aucune lecture "
                     f"depuis {time.time() - last_reads_t:.0f}s ***")
                emit(detail(v))
                emit(f"--- rafale de {args.burst} echantillons a "
                     f"{args.burst_period}s ---")
                for i in range(args.burst):
                    bv = sample(args.host, args.port, syms, args)
                    if bv is None:
                        emit(f"  burst {i}: <injoignable>")
                    else:
                        emit("  " + line(time.time() - t0, bv))
                    time.sleep(args.burst_period)
                emit(detail(sample(args.host, args.port, syms, args) or v))
                emit("*** fin de rafale ; retour a la cadence lente ***\n")
            time.sleep(args.period)
    except KeyboardInterrupt:
        emit("=== arret demande ===")
        return 0


if __name__ == "__main__":
    sys.exit(main())
