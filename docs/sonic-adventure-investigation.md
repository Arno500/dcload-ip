# Sonic Adventure over the network — investigation record (2026-08-07 → 2026-08-10)

> **This is a historical record, not a description of the tree.** It is the
> journal of the investigation that ended in commit `535f117`, "Sonic Adventure
> boots and plays over the network". It was §§1–20 of `AGENTS.md` until that
> file was cut back to describing the code that is actually present.
>
> Read it for the *method* and for the *causes eliminated by measurement* — that
> is what stops the next person re-running two days of experiments. Do **not**
> read it as a description of the current build:
>
> - Many measurements were taken at base `0x8cf00000`, before the move to
>   `0x8c004000`, and with a resident/transient split that no longer exists.
> - Most of the build flags named here (`CDFS_ASYNC`, `CDFS_SYNC_CHUNK_SECTORS`,
>   `G1_DMA_IRQ_STUB`, `RX_DIRECT`, `RX_GATE_ON_ROK`, `VBR_INPLACE_PATCH`,
>   `BG_PUMP_ON_TMU0`…) are gone. The tree has one build flag, `GD_TRACE`, plus
>   the `GD_*` constants in `cdfs_syscalls.c`.
> - The `ABIN` progress-acknowledgement protocol and the four-slot pool are gone.
> - Several counters cited here (`g_gd_seq`, `g_gd_calls`, `g_gd_ticks`,
>   `g_bg_ticks`, `g_vbr_checks`) no longer exist.
>
> The conclusion — the part that still governs the design — is §3.14, and it is
> summarised in `AGENTS.md` §4.6. Everything before it is the search.

## 1.  Cohabitation with Sonic Adventure — measured, not assumed

From GDB reads on a live freeze plus 11 host logs (2026-08-07), with
`scripts/dc-integrity.py`. This EXTENDS AGENTS.md §4.4 and the agent memory
"dcload-memory-map"; where it repeats them it is a re-measurement at the
current base, not a new claim. Re-measure before trusting any of it after a
relocation.

- **The allocator ceiling is exactly `0x0cf00000`** — dcload's first byte.
  The earlier note recorded four reads ending exactly there; across 11 sessions
  it is **160**, at three different sizes with the same end address
  (`0x0cefe800+6144`, `0x0ceff800+2048`, `0x0cecb000+217088`) and never one byte
  past. `selfwr = 0` everywhere, so `dcload_owns_range()` has never had to
  refuse anything. Zero margin below us, confirmed at scale.
- **The 8 bytes at `0x8cf00000`/`+4` are still there and still unexplained.**
  Same two values as the earlier session (`0x8c88ee5c`, `0x00002fe7`), so this
  is one stable event, not drift. Two candidates remain open: the title's
  allocator writing a boundary tag at its ceiling, or — as the earlier session
  argued from the write surviving a different game stack — dcload's own code
  writing a two-word descriptor at the wrong address. **Not resolved.** Impact
  is limited to `start:` (boot-only) and the `0xdeadbeef` sentinel.
- **SA NEVER INSTALLS ITS OWN VBR — it moves into ours.** `VBR = 0x8cf0b400`
  (dcload's) throughout gameplay. SA writes one identical handler prologue at
  VBR+0x100, +0x400 and +0x600, and fills VBR+0x1c8..+0x2c4 with 63 copies of
  `0x8c65ea2c` — its default handler table, squarely inside the `+0x1c0..+0x300`
  hole dcload reserves for exactly this. The reservation is correct.
  **Not covered by the hole**: SA also writes at VBR+0x420/+0x430, on
  `exception.bin`'s register-name strings.
- **Consequence, and it is a big one: dcload gets NO interrupt on this title.**
  The VBR patch that used to live in `vbr_patch.c` opened with
  `if (vbr == DCLOAD_VBR) return;` — "loader phase, our table is live, nothing
  to do". On SA that is false during gameplay: our table is live but the
  *entries* are the title's. So it bailed on its first line forever
  (`g_vbr_checks = 0`, `g_vbr_patches = 0`) and — measured on a live freeze —
  **`interrupt_common`'s entry counter at VBR+0x7e8 was 0 across 250146 GD
  syscalls.** That is why `g_bg_ticks`/`g_bg_polls` were 0 and the background
  pump never ran: a previously unexplained measurement, now explained.
  **dcload runs only from GD syscalls, with no heartbeat of its own**, and
  cannot repair anything between two of them — see the kill sequence below.
  **All of that machinery has since been deleted** (`vbr_patch.c`,
  `interrupt_common`, `dcload_bg_service`, the legacy full-VBR swap,
  `VBR_INPLACE_PATCH`, `BG_PUMP_ON_TMU0`): the `+0x600` vector is back to stock
  dcload's `nop; rte; nop`. DreamShell's isoldr does not hook the VBR on this
  transport either — `ENABLE_IRQ` is commented out in its `Makefile.cfg`, so its
  base `net` loader does not even compile `exception_init()`; VBR hooking
  arrives there only with CDDA/MAPLE/UBC/GDB. Removing it bought back 632
  resident bytes (headroom to `_resident_end` went from 76 to 708) and 640
  bytes of stack.
  **If you ever bring it back**, two things must be true first: the phase test
  has to distinguish "loader phase" from "the game populated OUR table" (isoldr
  simply patches whatever `vbr()` returns and re-verifies by `memcmp`, with no
  phase model at all), and `interrupt_common` has to move — it sat at
  `0x8cf0ba04` = VBR+0x604, inside what SA overwrites. VBR+0x5f0 is 22 free
  `nop`s and would do for the stub itself.
- **Everything that executes during gameplay is byte-identical.** `.text`
  (minus those 8 bytes and `cfs_saved`, which dcload writes itself), `.rodata`
  (minus the IP/MAC strings dcload paints for the display) and
  `.transient.text` all match the ELF. dcload's stack had 768 bytes still free.
- **The freeze is not us.** It is **121.7 KB** of PowerVR display list written to
  `0x8c000000`: 3895 32-byte vertex parameters spanning
  `0x8c000000..0x8c01e6e0`. (Corrected 2026-08-07 — the earlier "~48 KB / 1529
  vertices" stopped at the edge of the dump, not the end of the stream. The new
  figure is a full block-by-block re-read, and a sweep of all 16 MB of RAM finds
  exactly **one** such run.) It destroys the BIOS syscall vector table —
  `0x8c0000bc` reads `0xffa8a8a8` instead of `0x8cf00410` (`cfs_redir` at the
  current base — derive it from `cfs_redir_k`, never write the literal) — and
  the title's own
  trampoline at `0x8c10d8a0` (`mov.l @(0x8c0000bc),r0 ; jmp @r0`) then jumps into
  on-chip register space (`PC = 0xffd8001c`) and the CPU is gone. The vector is
  **collateral, not a target**: `0xbc mod 32 = 28`, and bytes 28..31 of the
  repeating block are the vertex's offset colour, `a8 a8 a8 ff` — little-endian
  `0xffa8a8a8`, exactly what the vector reads.
- **The 3895 blocks take only TWO distinct values**, differing solely in the
  command word: `e0000000 41e65750 c2157c98 3ab24e11 00000000 00000000 ffffffff
  ffa8a8a8` (×2597) and the same with `f0000000` (×1298). `E0` is a vertex, `F0`
  a vertex + end-of-strip, and the alternation is `E,E,F` — **1298 triangles
  whose three vertices are byte-identical**, i.e. zero-area, plus one trailing
  vertex. That is TWO independent faults and they must be kept apart: the
  **destination** starts at zero, and the **source is not advancing**. A null
  buffer explains the first; it does not by itself explain the second.
- **No hardware path was mis-aimed** — full register sweep (2026-08-07,
  `scripts/dc-regs.py`), not just the two registers read earlier:
  `QACR0 = QACR1 = 0x10` (→ `0x10000000`, TA FIFO); `SB_C2DSTAT = 0x10000000`,
  `C2DLEN = C2DST = 0`, `LMMODE0 = LMMODE1 = 0`; `CHCR2 = 0x000012c0`, decoding
  as destination-**fixed** / source-**incrementing** / 32-byte block /
  auto-request with `DE = TE = 0` (channel idle, transfer complete); TA output
  pointers all in VRAM (`TA_OL_BASE = 0x00518c80`,
  `TA_ISP_BASE = TA_ITP_CURRENT = 0x00400000`). `DAR2 = 0` is **normal** on the
  Dreamcast — Holly supplies the CH2-DMA destination via `C2DSTAT`, not the SH4's
  DAR — so do not read it as a null pointer.
  Store queues are excluded by argument rather than measurement: with
  `QACR = 0x10` an SQ write lands at `0x10000000 | (offset & 0x03FFFFE0)`, i.e.
  **always in the TA FIFO** whatever the offset, so an SQ *cannot* produce a
  linear fill of RAM. What remains is **ordinary CPU stores through a pointer**,
  and that pointer was zero. `SAR2 = 0xacfdfe00` shows the healthy path: the
  title builds its lists in a RAM buffer, then CH2-DMAs them to the TA. One of
  those buffers came back null.
- **The freeze point is deterministic and now identified.** Three sessions end
  on exactly the same read: LBA `0x06de5a`..`0x06de61`, eight single sectors into
  one 2048-byte buffer at `0x0c7a7f60`. That is the **AFS archive index**
  (`"AFS\0"`, 0x7fc = 2044 entries × 8 bytes = 7.98 sectors, hence eight reads).
  The delivered bytes are byte-identical to the image, verified against the
  frozen DC's buffer. The title then issues **no further read**, idles normally
  (ExecServer+GetDrvStat, 120/s, `gd_drv_stat = 1`) for ~23 s, and dies.
  So the last thing we hand it is correct, and it dies idle — not mid-transfer.
- **The kill sequence, ordered.** The display list overwrites the vector table
  at vertex 6 (offset 0xbc), the title writes **~3889** more vertices, and only
  then calls the next GD syscall and dies (`PR = 0x8c64521e`, the ExecServer
  thunk; `r3 = 0x8c10d8a0`, the trampoline). dcload had ~3889 vertices' worth of
  time to repair `0x8c0000bc` and could not, because it has no interrupt (above).
  `gd_vector_check()` at syscall *entry* is structurally too late, and its zero
  reading proves nothing.

When reading GDI sectors by hand to compare against a delivery, the track's
logical start is `start_lba + 150`, not `start_lba` (`gdi.rs:84`). Forgetting
the pregap shifts every sector by 150 and makes correct data look corrupt —
it cost an hour here before four control reads caught it.

## 2.  The black screen is a transition, and SA's display lists are empty

Measured 2026-08-08 with the `dcdiag` counters (AGENTS.md §11), at **full speed** — 60
VBlank/s, not the 1.7/s a `cdb` breakpoint imposes. This supersedes the earlier
"SA waits for a punch-through list end that is never raised" reading, which was
a symptom.

- **Steady state**: zero completed renders (`SB_ISTNRM` bits 0/1/2), zero
  punch-through lists, 387 opaque and 129 translucent list-ends per 30 s.
- **The display lists are EMPTY.** Opaque lists: 129 opened, **129 TA
  parameters**. A ratio of exactly 1 parameter per list. No timeout or pacing
  effect produces that — the list builder is emitting nothing.
- **It is a transition, not an incapacity.** Cumulatively SA completes **425**
  renders and submits **12** punch-through lists, then stops. Reproduced on two
  independent flycast processes with the *same* two numbers. So the mechanism
  works and then something flips.
- **The GD side stops at the same moment**: `g_gd_idx_counts` shows ReqCmd = 119
  against ExecServer 45337 / GetDrvStat 22846, and `g_gd_seq` is a perfectly
  regular `(2, …)`, `(4, …)` ring — the title's idle poll. It stops rendering
  and stops reading together.

**The timeline, at frame resolution.** Polling from outside never caught it —
four attempts lost the race to process startup. The answer came from recording
the timestamps *inside* flycast (`dcdiag_pt_at`, `dcdiag_render_first_vb` /
`_last_vb`), which is readable cold, minutes later. Clock = the V-blank count:

| event | frame | ≈ time |
| --- | --- | --- |
| 1st punch-through list | 3031 | 50 s |
| 1st completed render | 3032 | 50 s |
| 2nd punch-through list | 3863 | 64 s |
| punch-through #3..#13 | **3960→3970, eleven CONSECUTIVE frames** | 66 s |
| **last completed render** | **3971** | 66 s |
| 9000 frames later | nothing | 216 s |

`dcdiag_ptparam_at` (opaque parameters at each of those moments): 198, 612,
614, 615 … 624 — so during the eleven-frame burst, **1–2 parameters per
frame**. SA renders eleven near-empty frames and then stops for good.

**Read corruption is exonerated.** The last render lands 2.6 s before a "there
was an error while uploading the binary, resending missing parts", which is
tempting — but `dcload-ip-rs --verify-reads` on the next session reports **100
reads checked, 100 clean, 0 MISMATCHED, 2 500 608 B compared, 0 B wrong**,
through that same incident (2% holes, 76 KiB resent, all repaired). The bytes
SA receives are correct.

So the open question is now **"what flips SA at frame 3971"**, with delivered
data proven correct and the emulator proven to be running at full speed.

`attendues = 0x15` is read from `_DAT_8c8a30f4`, confirmed by direct read.
Whether SA computes that mask or carries it as a constant is still unknown, and
it matters: if SA legitimately has no punch-through geometry at this stage, the
mask is the bug; if it does, the submission is.

Six dcload configurations (async/sync, chunked, G1 stub, machine-cleanup off,
and the move to base `0x8c004000` with the BIOS VBR restored) all give the same
plateau. No build flag reaches this.

## 3.  The GD driver is now a server task (isoldr model) — 2026-08-10

When this was written, §§1 and 2 above described code that had been reverted,
whereas this section described code that was in the tree. That distinction is
now moot: the tree is described by `AGENTS.md`, and all of this file is history.

### 3.1 What changed

`cdfs_syscalls.c` and `cdfs_redir.s` were rewritten to follow DreamShell
isoldr's SD path (`loader/syscalls.c`, `loader/gdc_syscall.s`). The old model
answered a read entirely inside `gdGdcReqCmd`, blocking in `bb->loop()` until
every byte had landed. The BIOS driver a title is written against is instead a
**coroutine**:

- `gdGdcInitSystem` parks the caller and runs `gdcServerMain()` forever. It is
  also reached lazily from the first `gdGdcExecServer`, so a title that never
  calls InitSystem still gets a server.
- `gdGdcExecServer` resumes the parked server; `gdcExitToGame()` parks it again
  and returns to the game. One hardware stack; the inactive side's frame is
  copied into `saved_regs[]` (96 longs). `g_gd_park_longs` publishes the depth
  actually used — measured shallow; if it approaches 80, enlarge the buffer.
- `gdGdcReqCmd` only queues (PROCESSING) and returns a channel; `req_count`
  follows the BIOS sequence (never 0 or 1) and only the newest channel is
  addressable. `gdGdcGetCmdStat` consumes COMPLETED once, then reports IDLE.
- Reads are chunked `GD_EMU_ASYNC` sectors at a time (default 8 = 16 KB =
  DreamShell's SA preset and exactly the BBA RX ring), with a yield between
  chunks. Each chunk's TX still happens at syscall top level, so the `pkt_buf`
  invariant of `cdfs_syscalls.c`'s header holds.
- The syscall table is isoldr's full 18 entries; the `r6 == -1` misc calls are
  no longer claimed. The old `cmp/hs` range check admitted one index past the
  table — now `cmp/hi`.

**Verified working**: 86 reads served per SA session, 16 KB chunked requests
visible on the host, `gd_server_live = 1`, lock taken and released correctly.
It did **not** fix SA (see §3.3).

Hazard specific to this design, written on the assembly too: a parked frame is
restored onto whatever `r15` the next ExecServer arrives with, so **no function
live across a yield may take the address of a local**.

### 3.2 `REIOS: Booting up` does NOT mean the machine reset

This reinterprets a lot of earlier evidence. `reios_boot` is registered as a
hook at address 0 (`reios.cpp`, `SYSCALL_ADDR(0xA0000000)`), so the line means
only that **the SH4 executed at physical address 0** — a jump through a null
pointer reaches it just as a reset vector does. flycast's CPU context is *not*
cleared on the way there.

For SA on this path there is **no SH4 exception at all**: the `[BBA-DIAG]`
logger in `Do_Exception` (verified present in the shipped binary — check the
string, not the source) prints nothing, neither the bounded prefix nor the
`FATAL exception while BL=1` line. So AGENTS.md §14's "untaken exception under BL=1"
is not what happens here, even though `go.s` does hand over `SR = 0x500000f0`.

You cannot breakpoint address 0: `scripts/dc-trap.py` arms a `Z0` there and its
self-test proves the BIOS ROM ignores flycast's `trapa` patch. The self-test is
the point — without it a miss looks like a result.

### 3.3 Where Sonic Adventure actually dies

Reproducible across every configuration tried, ~6 s after `EXEC`:

- 86 reads are served, then read 87 — **LBA `0x000811bf` → `0x0cef7000`** —
  gets its `LBIN`, and the DC never answers the `DBIN`. The host retries five
  times over 20 s and gives up. Then the guest reaches address 0.
- **It is the address, not the volume**: with `GD_EMU_ASYNC=1` it dies on the
  first 2048-byte chunk of that same read, at the same destination.
- **Not a stack collision**: sampled through the whole session, the guest `r15`
  never leaves `0x8c00f328..0x8c00f3a8`.
- Afterwards `dc-pc.py` shows PC in `rtl_bb_loop`/`set_ip_dhcp`/`PMCR_Read` —
  `set_ip_dhcp()` only runs under `is_main_loop`, so that is dcload's
  **pre-launch idle loop**, i.e. proof it rebooted rather than hung.
- The data itself looks sane: LBA `0x811bf` begins `ff 50 56 4d 48 …`, i.e. a
  `PVMH` texture archive.

### 3.4 Two instruments that were lying, now fixed

- `scripts/dc-integrity.py` had `EXCEPTION_BASE = 0x8CF0B400` hard-coded from
  the old high base. Every run reported hundreds of differences in memory
  dcload does not own. It now derives the address from `exception`'s own ELF
  section header (its `.stack` section sits at `0x3fffff00`, so a
  lowest-symbol scan finds that instead — use `objdump -h`).
- **`PM_BASE = 0x8cf0c000` is no longer safe.** With dcload back at the low
  base nothing caps SA's allocator there any more, and SA overwrites the
  block: after a failure it reads `boots = 1, reads = 0`, i.e. re-claimed.
  §1's "allocator ceiling is exactly `0x0cf00000`" was a consequence of
  dcload sitting at `0x8cf00000`, not an intrinsic property of the title.

### 3.5 Reachability: dcload now announces itself

`announce_presence()` (net.c) sends a gratuitous ARP from the main loop.
Without it a static `DREAMCAST_IP` is unreachable **by construction**: the
host's first packet is unicast so it must ARP, flycast's BBA bridge does not
open its capture device until the guest has transmitted once, and the host's
neighbour entry decays to `Unreachable` — a state in which Windows discards
datagrams while `send()` reports success. DHCP hid this because DISCOVER is
that first frame. `Makefile.cfg` now carries `DREAMCAST_IP = 192.168.1.130`;
revert it to `0.0.0.0` for DHCP, and remember AGENTS.md §14 (`make clean` required).

Also fixed: `cdfs_syscalls.c` did not include `<unistd.h>`, so the tree did
not build at all under GCC 15 (implicit `write` is an error now).

### 3.6 The fatal read, narrowed by elimination (2026-08-10, later)

Every experiment below was a single run against the same reproducible failure:
86 reads served, then LBA `0x000811bf`, and the guest reaches address 0.

**dcload answers the LBIN and dies during the PBIN burst.** The host sends
`LoadBinary` exactly ONCE and never retries it, so dcload received that packet
and replied to it. It then never answers the `DoneBinary` that follows twelve
`PartBinary` packets later. So both RX and TX are alive going in.

Exonerated, each by direct measurement:

| Suspect | How it was ruled out |
| --- | --- |
| Destination address | Server redirects the write to `0x0c800000` (`DCLOAD_REDIRECT_ABOVE`/`_TO`). Same failure. |
| Payload content | Server serves zeros for that LBA (`DCLOAD_ZERO_LBA`). Same failure. |
| Transfer size / chunking | `GD_EMU_ASYNC` 8 and 1 (16 KB and 2 KB chunks). Same failure, first chunk. |
| Host burst pacing | 10-packet bursts vs one packet per 500 us. Same failure. |
| RX ring rules 1/2/4/5/7 | All four applied (see below). Same failure. |
| The GD state machine | Survived its complete replacement by the isoldr model. |
| The coroutine | Stock dcload failed on the SAME LBA before the coroutine existed. |
| Stack collision | Guest `r15` never leaves `0x8c00f328..0x8c00f3a8`. |
| SH4 exception | flycast's `[BBA-DIAG]` logger (string verified in the binary) prints nothing. |

What is left is the **moment**, not the transfer. The one intervention that
changed the outcome was adding UDP round trips immediately before the read (a
`write(1,...)` trace): that run did not reboot and Sonic Adventure went on to
read a different LBA entirely. Whatever the cause is, it is sensitive to what
dcload was doing just before, not to what it is asked to do.

### 3.7 RX ring: four rules the stock tree violated

Fixed in `rtl8139.c` while chasing the above. None of them cured Sonic
Adventure, but each is a real defect against `bba-rx-ring-rules`, and one of
them was actively wedging the ring:

1. **`RT_INTRMASK` was 0.** The only line that ever set it (`0x53`) sits inside
   a large commented-out block. The mask governs whether the RX status bits are
   observable at all, so the `RT_INT_RX_ACK` arm of the poll loop was dead.
2. **No ungated `RxBufEmpty` fallback**, so frames that never re-assert RxOK
   were invisible.
3. **`CAPR` was published out of range (`0x7ff0`) on ring wrap.** This tells the
   chip everything is drained; it moves its pointer to CBA and discards the
   queue. Measured in exactly that state during a failure: **CAPR 6340 ahead of
   CBR 2212 with RxBufEmpty clear**, dcload receiving nothing while flycast
   reported every frame delivered (`dropped=0`).
4. **No header plausibility check.** The chip writes the status word last, so an
   early read yields the previous occupant's bytes; the length taken from it
   advanced `cur_rx` arbitrarily and desynchronised the ring for good.
5. **Overflow re-initialised the NIC unconditionally.** The ring is 16 KB and a
   chunked read pushes ~18 KB through it, so overflow is normal back-pressure.
   Now: drain first, re-init only if the ring refuses to empty.

### 3.8 The death is in SA's own code, not in the transfer (2026-08-10)

Two continuous traces, sampled at 50-100 ms with `scripts/dc-track.py` (a new
tool: it resolves symbols once and re-attaches per sample, because flycast
halts the guest for as long as a debugger stays attached, so a held connection
freezes the very thing being watched).

**Counters, right up to the end:**

```
[30.04] timeout_loop=6  syscall_retval=0xffffffff  g_lbin_count=0x55
[30.13] timeout_loop=0  syscall_retval=0x00000000  g_lbin_count=0x56   <- read 86 COMPLETED cleanly
[30.30] <everything garbage: the reboot>
```

Read 86 finishes normally — `timeout_loop` cleared, `syscall_retval` 0, the
86th LBIN accounted for — and the machine is gone 170 ms later.

**PC, across the same transition:**

```
[31.56] pc=0x8c603326  pr=0x8c053ad8  r15=0x8c00f3b8
[32.01] pc=0x8c053ad8  pr=0x8c053ad8  r15=0x8c00f3bc
[32.10] pc=0x8c65fdd2  pr=0x8c65f19e  r15=0x8c8925e0   <- SA, on SA's own stack
[32.19] pc=0x8c009442  pr=0x8c009440  r15=0x8c00f284   <- dcload, re-booting
```

dcload does not appear anywhere in the window. **Sonic Adventure is running its
own code, on its own stack, and jumps to address 0 by itself.** The request for
read 87 is the last thing dcload gets out; everything after it -- the host's
LBIN, the twelve PBINs, the unanswered DBIN, the 20 s timeout -- is aftermath
against a machine that has already gone.

Also settled here: `g_pmcr_backwards` stayed **0**, so the performance counter
never ran backwards and the "bogus instant timeout" theory is dead (the guard
added to `rtl_bb_loop` is still worth keeping -- an unsigned subtraction that
can wrap into a deadline is a trap regardless). The coroutine is likewise
exonerated by direct measurement: `saved_regs_ptr` alternates cleanly between
`saved_regs_end` and one parked frame, `g_gd_park_longs` is a constant 9 out of
96, and `gd_lock_byte` toggles 0x80/0x00 as it should.

**Where that leaves it.** Every accounting instrument says the transfers are
complete and correct, and the title still computes something fatal from them.
That is precisely the blind spot `docs/read-back-verification.md` describes:
corruption by RX-ring splice is invisible to packet accounting, because the
bytes are all delivered and all counted -- just not all right. The `--verify-reads`
flag that instrument needs **no longer exists in `dcload-ip-rs`** (it went with
the 2026-08-09 revert; only the doc survived). Re-implementing it -- read the
delivered range back with `SBIQ` between the last PBIN and the RETV, compare
against what was sent -- is the one measurement that has never been made on
this failure, and the only remaining hypothesis it does not already contradict.

### 3.9 Delivered bytes are PROVEN correct, and tracing moves the failure

Two results from the same evening, both of which change what is worth trying.

**1. Read-back verification now exists again, and it says the data is right.**
`dcload-ip-rs` re-implements `--verify-reads` as `DCLOAD_VERIFY_READS=1`: after
the last PartBinary and before the ReturnValue, it reads the delivered range
back with `SBIQ` and compares. Result on a full Sonic Adventure session:

```
VERIFY ok:       85
VERIFY MISMATCH:  0
```

Every byte the title receives is correct. The RX-splice-corruption hypothesis
of `docs/read-back-verification.md` -- the last one standing after §3.6 -- is
therefore **dead**, and with it the entire dcload data path.

Getting there required fixing `receive_data()` in the server: its
out-of-range test compared a **byte offset** against a **chunk count**
(`(size + CHUNK_SIZE) / CHUNK_SIZE`), so for any transfer longer than one
packet every chunk after the first was rejected as "bad", its slot never
filled, and the recovery loop re-requested it forever. Read-back only ever
worked for single-packet reads.

**2. Tracing carries the title PAST the fatal read.** Built with `GD_TRACE=1`
(the new compact one-write-per-event contract trace), the three chunks of LBA
`0x811bf` are all served and the title is told, correctly:

```
R 00000011 000811bf 00000012      ReqCmd DMAREAD, 18 sectors
S 0000004a 00000001 00004000      PROCESSING, 16384 transferred
S 0000004a 00000001 00008000      PROCESSING, 32768
S 0000004a 00000002 00009000      COMPLETED, 36864 = 18 x 2048 exactly
```

With `GD_TRACE=0` the very first of those three chunks never arrives. The read
that resisted every other change is served the moment a few UDP round trips
precede it. SA still dies later (88 reads instead of 86), so this is not a fix
-- but it is the first thing in the entire investigation to move the boundary.

**And it is NOT the RX ring being flushed.** That was the obvious reading, and
it is wrong: `GD_DRAIN_ITERS` (a bounded, clock-free `bb->loop()` before each
request, wired through `drain_iters` in `rtl8139.c`) leaves the failure exactly
where it was at both 256 and 50000 iterations. What the tracing supplies is the
round trips themselves, and why that matters is still unexplained. Left in the
tree at 0 so the next person to suspect the ring can see it was measured.

### 3.10 Conformity review against isoldr's ARCHITECTURE.md

`firmware/isoldr/ARCHITECTURE.md` is the reference document for this port. Four
real divergences were found by reading it against the tree; three are fixed.

**FIXED -- the SR handed to the game (`go.s`). The important one.**
dcload loaded `SR = 0x500000f0`: MD=1, RB=0, **BL=1, IMASK=15**. Every interrupt
level masked and exceptions blocked, so a title could not take a VBlank, a Maple
completion or anything else until it cleared those bits itself, and any
exception it did take became a manual reset instead of a fault. isoldr hands
`0x60000101` -- BL=0, IMASK=0, RB=1. This is why §1's measurement of SA's
interrupt entry counter read **zero across 250000 GD syscalls**: not a bad VBR,
nothing could be delivered through it. Note RB=1 flips the register bank, so the
entry address must leave r4 before SR is written (`mov r4,r14` first, as isoldr
does).

**FIXED -- the >= 100 sector bulk rule (`data_transfer`).** isoldr reads big
requests in one blocking shot rather than chunking them: "the game in loading
state (request big data), so we can increase general loading speed if load it
for one frame". SA issues a 105-sector read immediately before the read it dies
on, so this was not hypothetical. `GD_BULK_SECTORS`.

**FIXED -- `mode` reported by ChangeDataType.** 1024, not isoldr's 2048: that is
what the BIOS answers and what KOS asks for (0, 8192, 1024, 2048). Own
regression, introduced by this rewrite.

**DELIBERATE DIVERGENCE -- caches off at handoff.** isoldr writes `CCR = 0x0909`
(caches on) in `_boot_stub`; dcload writes `0x0808`. Kept off on purpose: isoldr's
transports are DMA, so the device writes the game's buffer and the loader only
purges afterwards, whereas dcload fills those buffers with CPU stores while the
game reads them back uncached (isoldr's own §8: "parce que le jeu relira le
buffer en non caché"). Caches off makes that coherent by construction. The CCR
write now lives in `go()` itself, running from P2 with the required settling
window, so the guest cannot be entered with caches on by any path.

**Verified compliant**: stack budget across a yield (the document's costliest
trap -- isoldr allows 80 bytes, we have 96 longs and measure a constant 9); never
returning FAILED for an unknown command; syscall vector written through P2;
`req_count` never 0 or 1; dcache purge after a CPU-delivered DMAREAD (done
per-packet in `cmd_partbin`); no FPU on any ISR path (we hook no VBR, which is
also what isoldr's `net` build does).

**Known remaining gaps, none of which SA exercises**: the `*_STREAM` commands
fall through to COMPLETED instead of being served or refused; `GETTOC2` does not
emulate the low/high density areas of a real GD; there is no `g2_lock()` around
CPU reads of the BBA (§9.3 warns a game's AICA DMA can wedge the bus -- newly
relevant now that interrupts are unmasked, and previously measured at zero
contention only while they were masked).

None of the three fixes moves SA's failure, which remains at LBA `0x811bf`.

### 3.11 The "Missing 1440 bytes at address" storm

Three separate things, fixed or characterised.

**1. A regression I introduced (§3.10's bulk rule).** Adopting isoldr's
"read >= 100 sectors in one shot" turned SA's 105-sector read into a single
215040-byte burst: 150 packets back to back into a 16 KB RX ring that fits
eleven. Result: **840 lost packets in one run.** isoldr can do this because its
transports are DMA with no ring; ours cannot. `GD_BULK_SECTORS` is now 0, and
the reasoning is written where the constant is so nobody re-imports the rule.

**2. Recovery cost one round trip PER PACKET.** `DoneBinary` names only the
first missing part, and `send_data()` resent exactly that one before asking
again -- 840 holes meant 840 resends and 840 DoneBinary exchanges, each able to
time out on its own. It now resends a run from the reported address, doubling
per iteration, capped at 64. Overshoot is free (a part the DC already holds is
rewritten with the same bytes). Measured on the same upload: 154 round trips ->
104, and the uncapped version was tried and rejected -- 18 round trips but
31472 parts resent (45 MB for a 6.4 MB upload) and 5.2 s -> 9.0 s.

**3. Upload burst was larger than the ring.** 15 packets back to back is ~22 KB
into a 16 KB ring. Now 8 (~12 KB). Holes: ~150 -> ~63.

**What is left is NOT congestion, and this is the useful part.** The remaining
~63 holes on a 6.4 MB upload are **strictly periodic** -- one packet in every
75 to 80, gaps clustering hard on 76/77/78 -- and **independent of pacing**:
bursts of 8 and of 4 give 63 and 64 holes respectively, while 4 makes the upload
38 % slower. Congestion does not behave like that.

One loss per ~77 packets is one loss per ~7 ring wraps. The ring is 16384 bytes
and a full frame occupies 1504 of them, which is 10.89 slots per wrap -- the
wrap point drifts, and a defect that only bites when the drift has accumulated
would show up at exactly this cadence. That is where to look; do not tune the
pacing further, it has been measured and it does not move.

### 3.12 A real hang found, and the failure finally MOVES

Tracing to the host (`GD_TRACE=1`, plus `GD_TRACE_DEST_FROM` for the one read
that matters) turned this from guesswork into measurement. Two genuine defects
came out of it, and the failure is no longer where it was.

**1. Two unbounded hardware waits in `rtl_bb_loop` -- a real hang.**
The "link change" branch resets the PHY and then spins:
`while (!(nic16[RT_MII_BMSR/2] & 0x20));` and the same for the follow-up
interrupt. Nothing guarantees either arrives. Measured with a poll-count
heartbeat (`g_rx_polls`): the loop went from ~45000 iterations per 0.3 s to
**ONE per 0.26 s** while `g_rx_frames` froze -- dcload nominally "in bb->loop"
but never looking at the ring again, the host retransmitting into a DC that had
stopped listening. Both waits are now bounded (`RTL_LINK_SPIN_LIMIT`), and the
poll loop stays healthy right up to the end. Every hardware wait in dcload is
supposed to be bounded; this pair had been missed.

**2. A trace that broke what it was measuring.** The `D` event was emitted
AFTER the CDFSREAD had been laid into `pkt_buf`. write() is itself a syscall
that builds into the same buffer, so `build_send_packet()` transmitted the
leftovers of the trace and the host never saw the read at all -- 17 traced
reads, 17 timeouts, zero requests received. Trace BEFORE building the command.
The file's own header warns that pkt_buf is single and shared.

**3. Where it stands now.** With both fixed, the 87th LoadBinary **is received**
(`g_lbin_count` reaches 87; it never did before) and the window is right
(`g_last_load_addr = 0x0cef7000`, `g_last_load_size = 0x4000`). The failure has
moved one layer down: **exactly 12 parts -- one full 16 KB chunk -- are refused
by `cmd_partbin`'s window check**, `g_last_reject_addr = 0x0cefade0`.

That address starts inside the window, and `0x0cefade0 + 1440 = 0x0cefb380`
overruns its end by 896. Refusing such a part outright loses bytes the host
believes it delivered, so the check now clamps to the window instead of
dropping (`g_pbin_clamped`). That did not unblock the title either, which means
the first of the twelve is being refused for a different reason than the last
-- the remaining question is what window was actually in force when part #1
arrived, and `g_last_reject_load`/`_end`/`_size` exist to answer it. Sampling
them is the difficulty: the machine is gone about 200 ms after the read starts,
and `dc-track.py` needs ~1 s to attach, so the window has to be caught by a
latch on the DC rather than by polling.

**Host-side changes made along the way**: `send_data()` now verifies the
LoadBinary ECHO (address and command) instead of accepting whatever packet
arrived next -- measured clean on this failure, so it is hardening rather than
a fix.

### 3.13 ROOT CAUSE: Sonic Adventure's stack runs through dcload's BSS

Measured, not inferred. Tracing the stack pointer the title calls us on (the
'K' event, reported on change) gives:

```
P 8c644f7a 8c00b9d0     <- SA calls a GD syscall with SP = 0x8c00b9d0
K 8c00b9d0 8c644f7a
```

`0x8c00b9d0` is inside dcload. The neighbourhood, from the link map:

```
8c00b060  _bb              <- THE ADAPTER POINTER: bb->loop(), bb->tx()
8c00b078  _g_gd_idx_counts
8c00b0f8  _syscall_data
8c00b220  _dmabuffer       <- 0x80c bytes; SA's SP lands here, at +0x7b0
8c00ba30  _bin_info        <- 11.6 KB packet map
8c00e7c0  _end
```

SA is using the BIOS work area as a stack -- which from its point of view is
free memory -- and that area is where dcload lives. Its stack grows DOWN from
0x8c00b9d0 and only has to travel about 2.4 KB to reach `_bb`. Once `_bb` is
overwritten, dcload's next `bb->loop()` or `bb->tx()` is an indirect call
through a corrupted pointer; land on zero and the guest executes at physical
address 0, which is exactly what "REIOS: Booting up" reports (§3.2). It is
also mutual: dcload writing `bin_info.map` writes into SA's live stack.

**This explains the entire investigation.** Deterministic (SA reaches that
depth at a fixed point in its loading sequence); no SH4 exception (jumping to
zero is a legal jump); dcload's counters reading as foreign data in earlier
samples (they ARE being overwritten -- that was SA's stack, not a mid-reboot
artifact as recorded in §3.8); the PC trace showing SA running its own code
while dcload is nowhere (SA is fine, dcload is what dies, on its next call);
and why every protocol-level fix -- destination, content, chunk size, pacing,
RX ring rules, GD state machine, coroutine -- was measured irrelevant. They
were all downstream of memory being shared with the title.

It is precisely the last and costliest trap in isoldr's ARCHITECTURE.md §11.2:
"Loader placé dans une zone que le jeu réutilise -- crash à un moment précis du
jeu, toujours le même." isoldr avoids it by being 13 KB for its network build
and by having a per-title preset database (§10.2, §10.3) whose whole purpose is
choosing where the loader may live. dcload is 41 KB and fills the hole.

**Where to go from here.** The fix is placement/footprint, not protocol:

- `bin_info` alone is 11.6 KB of the 41, and only needs to be large before
  EXEC: the biggest in-game transfer is 215040 bytes (150 parts) against 4678
  for the upload. Sizing the resident map for the in-game case and keeping the
  large one only for the pre-EXEC phase reclaims most of it.
- `_bb` and the other small hot state should sit at the BOTTOM of the image,
  furthest from where a title's stack roams, not directly under the big
  buffers.
- A guard is cheap and would have found this in minutes: latch the SP seen at
  syscall entry and refuse to proceed (or at least count it) when it falls
  inside [0x8c004000, _end).

### 3.14 SONIC ADVENTURE BOOTS AND PLAYS (2026-08-10)

Intro video, skippable, then gameplay in Station Square. **1624 disc reads
served, `boots = 1`** -- no reboot at any point. The ceiling had been 86 reads
and a guaranteed reset.

The cure was §3.13's root cause: **stop sharing memory with the title.**
Sonic Adventure enters GD syscalls with SP = 0x8c00b9d0, inside dcload's BSS,
and its stack grows down over `bb` -- the adapter pointer -- after which
dcload's next `bb->loop()` is an indirect call through garbage and the guest
executes at address zero. Nothing else mattered until that was fixed.

**What actually made it work, smallest first:**

1. **`BIN_INFO_MAP_SIZE` 11656 -> 256** (`commands.c`). 11.4 KB of packet map
   removed from the loader's footprint. The host splits any transfer larger
   than `MAX_XFER = 256 * CHUNK_SIZE` into successive LoadBinary transfers --
   **these two constants must stay in step.** The largest in-game read is
   215040 bytes; only the initial upload needed the old size.
2. **Maple `dmabuffer` moved to 0x8cfe8000** (`maple.c`). 2 KB more, and it was
   the buffer SA's stack pointer actually landed in. Address is isoldr's own
   free-high-RAM heuristic (ARCHITECTURE.md 10.2).
   Together these take `_end` from 0x8c00e7c0 to **0x8c00b310**, putting the
   whole loader below the title's stack instead of underneath it.
3. **`GD_YIELD_BETWEEN_CHUNKS = 0`** (`cdfs_syscalls.c`). Chunk on the wire,
   atomic to the game: each 16 KB piece fits the RX ring, but the title is
   never handed control mid-read. With the yield it died on delivery of chunk
   1; without it the full 36864-byte read completes.
4. **Bounded the two PHY waits** in `rtl_bb_loop` (§3.12) -- a real hang.
5. **Turn the tracing OFF.** `GD_TRACE_CALLER` costs a UDP round trip per
   event and SA alternates stacks on every GD syscall, so it fired ~120 times a
   second and was itself holding the title back: with it on, the game reached
   the AFS index reads and stopped; with it off, it plays. The instrument that
   found the bug must not be left in the running configuration.

Supporting fixes that were necessary but not sufficient: the RX ring rules
(§3.7), the gratuitous ARP and static IP (§3.5), the GCC 15 build fix, the
host's LoadBinary echo check and growing resend window (§3.11), and **two
copies of the same defect in `receive_data()`** -- a byte offset compared
against a chunk count, plus a full-length copy of the short final chunk, which
panicked the tool outright ("range end index 1440 out of range for slice of
length 30").

**Working configuration**: `GD_EMU_ASYNC=8`, `GD_BULK_SECTORS=0`,
`GD_YIELD_BETWEEN_CHUNKS=0`, `GD_TRACE=0`, `GD_TRACE_CALLER=0`,
`GD_DRAIN_ITERS=0`, `DREAMCAST_IP=192.168.1.130`, SR handoff `0x60000101`,
caches off from P2 in `go()`.

**The lesson worth keeping**: every measurement for two days pointed at the
transport -- lost packets, ring wedges, timeouts, a title that "died on a
specific disc read". All of it was downstream. The question that cracked it was
not "what is wrong with the transfer" but "what stack are we running on", and
it took three lines of instrumentation at syscall entry to answer.
