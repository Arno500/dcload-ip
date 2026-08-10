# Read-back verification (`--verify-reads`)

Host-side instrument in `dcload-ip-rs`. Added 2026-08-07.

## Why it exists

Every counter this project has, on both sides, is **arrival accounting**: how
many payloads were accepted (`pbin_ok`), credited (`winbound`), made contiguous
(`wincomplete`), acknowledged (ABIN). Not one of them looks at a byte of
**content**.

So a read can be reported perfect — window complete, frontier at the end,
`diverge` 0, `pbin_drop` 0 — while the destination holds the wrong bytes. The
only symptom is the title doing something inexplicable a long way downstream,
with a spotless transfer log behind it. That is the shape of the Sonic Adventure
freeze: the title ends up spinning in a parse loop over a buffer it will not
accept, and no counter says why.

This is the only instrument that separates **"all the bytes arrived"** from
**"the right bytes arrived"**.

## How it works

After the last PBIN of a CDFS sector read and **before the `RETV`** that
releases the game, the host issues `SBIQ` for the delivered range, collects the
`SBIN` chunks, and compares byte for byte against what it sent.

That window is the whole trick: dcload has stopped writing the buffer and the
game has not started reading it, so it belongs to neither side. A read-back
*after* the RETV would be comparing against whatever the title has since done
with a buffer it very often reuses.

No DC-side change was needed. `cmd_sendbinq` is already resident
(`commands.c:1162`) precisely so memory can be dumped while a game runs.

## What it proves, and what it does not

**Proves**: the bytes at the destination, read through the same alias
`cmd_partbin` wrote them through — both take `cmd_addr` verbatim,
`commands.c:1056` writes and `commands.c:1215` reads — are the bytes we sent.

**Does not prove**: what the *game* sees. A cache-alias or DMA-coherency fault
between dcload's store and the title's load is outside its reach, and a clean
verification does not rule one out.

## What it found (2026-08-07)

Seven mismatches over ~350 verified reads, all the same shape: **one payload,
damage from a 32-byte boundary to the payload's end, holding the bytes of a
payload N positions later at the same intra-payload offset** (N = 1,2,3,5).

The start offsets were 0, 352, 480, 1088, 1280, 1344 — all ≡ 0 (mod 32). The
payload sits at window offset 56, so a *source* cache-line boundary would fall
at ≡ 8; the destination is 32-aligned and CHUNK_SIZE is 45×32, so a
*destination* boundary falls at ≡ 0. That phase, plus an A/B showing that
reading the RX ring uncached the way KOS does (`RX_PKTWIN_UNCACHED=1`) changes
nothing, ruled out the ring and pointed at `cmd_partbin`'s copy.

Cause, then captured directly (`g_rx_reenter_info`): a nested `rx_deliver` at
SP `0x8c892590` while the outer was at `0x8cfdfe9c` — 7.7 MB apart, a different
*stack* — with `g_bg_ticks` at 0. The title issues a GD syscall from a second
execution context while one is already running, which re-enters the drain loop
and replaces the single `current_pkt` staging buffer underneath the
`SH4_aligned_memcpy` still reading it.

Fixed by a re-entrancy guard on `rtl_bb_poll_once` (`g_poll_reentry_declined`).
Declining is lossless: it returns before touching the ring, so the outer poll
still drains those frames. Measured at the same point (`pbin_ok` ≈ 5000):

| | reenter | declined | rxrst | credlost |
| --- | --- | --- | --- | --- |
| before | 26 | — | 4-6 | 13-16 |
| after | **0** | **28** | **0** | **0** |

`credlost` going to zero is the notable part: it had survived every previous
slot-attribution fix and was assumed to be an attribution problem. It was a
symptom of nested draining.

## Usage

```sh
dcload-ip-rs --host <ip> u-exec <bin> -d <image> --verify-reads      # every read
dcload-ip-rs --host <ip> u-exec <bin> -d <image> --verify-reads 10   # one in ten
```

Off by default. It costs a full second copy of each verified read, in the
opposite direction, on the link that is already the bottleneck — **start with
sampling**, not full coverage: at `--verify-reads 1` a multi-megabyte read adds
seconds to a `RETV` the title is blocked on, and that is itself a perturbation
of the thing being measured.

## Reading the output

A clean read is `debug`. Three outcomes are louder:

```
ERROR READ-BACK MISMATCH on LBA 0x000799f1 at 0x0cd00000+0x7bc1: 1240 of 34816
      compared bytes differ, spanning +0x7bc1..+0x8100 across 1 payload(s) of 1440
ERROR   sent 70 85 e0 06 ... | got 00 00 00 00 ... | destination still zero here:
        this range was never written
```

- **`N payload(s)`** is the number to read first. `1` means a single PBIN is
  missing or misplaced inside an otherwise perfect window — an accounting fault.
  Many means the tail never landed — a transport fault. The byte total alone is
  compatible with either.
- The **diagnosis** after the hexdumps answers *what the wrong bytes are*:
  - `destination still zero here: this range was never written` — the frontier
    claimed a chunk that never arrived.
  - `these bytes belong at +0xNNNN of this same read, displaced by N bytes` — a
    payload landed at the wrong offset. Only ever said when the sample occurs
    **exactly once** in the read; game data repeats, and naming the first match
    would be a confident lie. When it repeats, it says so instead.
  - `these bytes are from nowhere in this read` — stale buffer, or another
    writer.

```
WARN Read-back of LBA 0x0007ca18 could not verify 91328 of 186368 bytes
     (the DC did not answer at all -- it is not servicing the network);
     the 95040 bytes we did see all match
```

**Never an error, and never counted as a mismatch.** A lost read-back packet
says nothing about the data underneath it; conflating the two would make every
result untrustworthy. The parenthesis distinguishes three different facts: the
DC silent, the time budget exhausted, or packets lost in flight.

A running total is restated every 50 verified reads and at session end.

## Bounds (all in `dispatch.rs`)

| Constant | Value | Why |
| --- | --- | --- |
| `VERIFY_REQUEST_BYTES` | 32 KiB | `cmd_sendbinq` blasts a whole request back with **no pacing** (`commands.c:1201`). The request size *is* the burst the host socket must absorb: 32 KiB is 23 packets. A 1.2 MB read asked for in one go would be 850 and would simply lose most of them. |
| `VERIFY_PASSES` | 3 | Sweeps over still-missing chunks before giving up. |
| `VERIFY_REQUEST_TIMEOUT` | 400 ms | The DC services the network only from inside the game's GD syscalls, so answers come at the title's polling cadence. |
| `VERIFY_TOTAL_BUDGET` | 2.5 s | **Hard ceiling per read.** Without it, a DC that stopped answering turns a 1.2 MB read into 36 requests × 3 passes × the timeout — ~43 s added to a freeze, with the title blocked on the RETV throughout. An instrument must never outweigh what it measures. |
| `VERIFY_SAMPLE_BYTES` | 64 | Sample length used to locate displaced data. Long enough that a match is unlikely to be chance. |

## Scope

CDFS sector reads (`DCLoadClientCmds::ReadSector`) only — the path under
investigation. The TOC and the `/pc/` file-server reads (`fs.rs`) go through the
same `send_data` window and could be hooked the same way; `verify_read_back` is
written to be callable from anywhere.

## Tests

`mod verify_tests` in `dispatch.rs` covers the two pure halves — the
missing-chunk request planner and the comparison — because the I/O half cannot
be faked (`polling::Events` is opaque). The comparison tests are the ones that
matter: `compare_never_blames_a_chunk_that_did_not_come_back` and
`diagnose_refuses_to_localise_ambiguous_data` both guard against this tool
producing confident false reports, which is the only way it can do harm.
