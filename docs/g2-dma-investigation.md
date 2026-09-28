# G2 DMA: the CPU stays free while the bus moves the bytes

2026-09-28 / 29. The summary and the rules are AGENTS.md §4.16; this is the
record of what was measured, in order. The goal: less time spent inside the
loader, fewer stutters.

## 0. Why

`docs/wince-investigation.md` 9r had put the remaining cost of an asynchronous
disc read on the console at 1.5 ms of CPU a chunk, all of it the copy of the
frames out of the BBA's SRAM over G2 (~140 us a frame, the CPU waiting on the
bus for every word), with SR.BL set. The CD-DA rings were written the same way
(~0.34 ms a sub-fetch, plus ~9 ms frozen at every PLAY or seek: the floor over
the whole 61 KB ring). Both are G2 traffic; a G2 DMA frees the CPU for their
duration.

## 1. The bench (`G2DMA_BENCH=1`, `g2bench.c`)

Console, Crazy Taxi 2's session. 1 tick of TMU2 = 0.08 us.

| Transfer | Time | Rate | Wrong words |
| --- | --- | --- | --- |
| RX, 1536 B, CPU through the GAPS window (`pktcpy`) | 99 us | 15.5 MB/s | 0 |
| RX, DMA ch 1 through the window | 94 us | 16.3 MB/s | 0 |
| RX, DMA ch 1 at the SRAM's own address | 94 us | 16.3 MB/s | 0 |
| AICA, 2368 B, CPU | 336 us | 7.0 MB/s | 0 |
| AICA, DMA ch 0 | 441 us | 5.4 MB/s | 0 |
| AICA, DMA ch 1, 2, 3 | 314 us | 7.5 MB/s | 0 |

Torture (256 rounds: a DMA on each side in flight while the CPU writes and reads
the AICA as a title's driver would): 0 wrong words either side, positive
control 1, `g_bench[30]` 0 (the DMAs were over when the CPU's part was).
Conclusions: **DMA is not faster than the CPU** (the bus is the limit), all four
channels work, so the gain is the CPU's freedom only, and a synchronous RX DMA
(step 4 of the plan) buys nothing. The bench used the Maple page (4 KB, idle at
boot) as its buffer: `.hiram` had no room.

## 2. CD-DA to the AICA by DMA

Edges (at most 28 bytes a side) by CPU, aligned body by DMA on channels 2 and 3,
left running (AGENTS.md §4.13 rule 7). The right block is moved up to 31 bytes
in the staging buffer to be congruent to its ring; `cdda_prime()` floors nothing
in ADPCM. Console (Snow Surfers): no glitch heard, counters clean.

Bytes: the feature cost 796 B against 352 B free under the HIGH bound. Paid by
removing the legacy 1024-byte payload mode (~224 B), a smaller `g2dma_start`,
and finally by moving `_stack` of the HIGH family from `base+0xb000` to
`base+0xbc00` (3 KB; the host's `loaders::layout()` follows). It stays 1 KB under
`.hiram`: the host's relocator classifies words by value, and `_stack` equal to
`.hiram`'s start would be both.

## 3. RX by DMA, from the tick (Katana titles)

`rx_is_pbin()` peeks 64 bytes, `rx_dma_start()` DMAs the frame into
`raw_current_pkt`, the tick returns; `rx_settle()` processes it when the DMA is
over. Sets `loaders-g2dma-rx*` in the host's project root.

| Set | What | Result (2 s of load to the menu) |
| --- | --- | --- |
| `rx` (first) | as above, DMA-end interrupt on our IML2 | tick 13.5 ms, `g_irq_tick_max` 0.3 ms, but load ~0.7 MiB/s |
| `rx2`/`rx3` | counters | `g_irq_rx_dma` 0; latency DMA to processing 6.2 ms |
| `rx4` | + why the end does not interrupt | `g_irq_dma_ist` = `g_irq_dma_armed` = 1 per DMA, IMASK 0, our mask `0x10000`; still `g_irq_rx_dma` 0 |
| `rx5` | chip ack before the DMA, chip interrupt armed too | worse: tick 95.6 ms (4.7 %), same throughput. Reverted |
| `rx6` | **a finished DMA, at any level, forces a look** | 71/71 chunks finished by the tick, latency 81 us, tick 0.75 ms a chunk, max 0.3 ms, **~1.5 MiB/s, no lag** |

### The finding

`g_irq_iml` showed Crazy Taxi's IML4 NRM mask at `0x7f000`: bits 12..18, Ext1's
DMA end (bit 16) among them. The end of our DMA was therefore taken as the
**title's** IML4 interrupt (evt `0x360`, above our IML2), never as `0x3a0`: that
is why `g_irq_rx_dma` (counted only at our level) stayed 0 while the any-level
counter `g_irq_dma_ist` counted one per DMA. Those entries fell into
`irq_tick()`'s "a read was looked at a moment ago" limit (0.5 ms), and the frame
waited for the next interrupt of the title's. One frame per entry at ~590
entries a second is ~830 KB/s: the throughput seen. Each hypothesis (bit never
set, mask overwritten by the title, IMASK blocking IML2) had its counter
(`g_irq_dma_ist`, `_armed`, `_imask`, `_mask`), and the answer was none of the
three: the interrupt was delivered, elsewhere.

### The checksum, then cleanup (2026-09-29)

`process_pkt` was ~46 us a frame on the estimate, most of the tick's 0.75 ms a
chunk. The UDP checksum (a 16-bit loop with a carry test per word, ~18 us over
1440 bytes) was rewritten with 32-bit reads and one fold (`rx7`); verified on
the PC against the old loops, 38 426 cases, no difference. The diagnostic
counters of the investigation above were then removed (`_end` from
`base+0x93dc` to `base+0x9310`; the CD image's slack from 36 B to 240 B); only
`g_rx_dma_frames` and `g_g2dma_timeouts` stay. The counter names in the tables
above are the ones of that time.

### Left

The RAM-to-RAM copy of `cmd_partbin()` and its purge (est. 13 us a frame). A zero
copy (DMA to the destination, checksum read from there: write before verify)
would gain ~10-15 us a frame, ~0.15 ms of a chunk of ~8 ms and 2-3 % of the load
throughput, which is bounded by the host round trips and not by the tick. Not
worth its bytes and its change of guarantee for now.

## 4. Not done

- Windows CE: no RX interrupt exists there, so RX stays by CPU; the DMA would
  wait for the next interrupt of CE's.
- The LAN Adapter has no DMA path.
- `loaders/` (the deployed set) is not updated: the sets under
  `loaders-g2dma-*` are for testing; deploying `rx6` changes the HIGH layout, so
  the host and the loader set must go together (AGENTS.md §14.19).
