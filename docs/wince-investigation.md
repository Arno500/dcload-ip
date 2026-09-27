# Windows CE titles — investigation log

2026-09-20 → 27. Test title: **Sega Rally 2 v1.000 PAL** (GDI, boot-sector md5
`91a39a9f85e1806f06be8cfbda0866c2`). The summary and the current state are in
AGENTS.md §4.15; this file keeps the measurements, so nobody has to redo them.

**Where it stands (2026-09-27):** the title boots, reaches its menus, races,
saves and loads, on the console and under flycast. What got it there, in
order: the three GD doors (§4, §7), the first sector dropped (§6), the TOC
staged instead of written into a virtual address (§7e-7f), CE's RAM and the
loader's place in it (§7g), PIO streams (§7h), retries that do not leave CE
waiting on an interrupt (§7i), no write outside the loader's span (§7k), late
answers not burning retries (§7l), and above all **no thread switch inside a
network exchange** (§7o-7q). Left: the menu stops ~0.1 s whenever the title
reads its streamed audio, and CD-DA glitches when CE goes ~700 ms without
calling the driver (§7t-7u) -- both need the interrupt hook (§8).

Hypotheses this log carries that turned out wrong: the CD-DA channel clash of
§7m (the freeze survived a loader without CD-DA, §7n), the VMU/VMUPro of §7n
(it was the preemption of §7o), and the yield per chunk of §7r (§7s). They
are left in place because the measurement that killed each one is next to it.

## 1. The failure

The host uploaded `0WINCEOS.BIN` to `0x0c010000` and sent `EXEC`. Within
milliseconds dcload reported an address error (read, `expevt 0x0e0`). The dump
arrives through the `write(1, …, 272)` syscall — 272 is `sizeof(exception_struct_t)`
— which is why the host log shows a `Write` just before the exception.

Decoded against `go.S`:

| field | value | reading |
| --- | --- | --- |
| `pc` (= SPC) | `0x8c012100` | a `bra`; the fault is in its **delay slot** at `+2` (SH4 sets SPC to the branch) |
| `pr` | `0xac8a0000` | `go.S`'s `_dcload_base + 0x20000000`: loader at `0x8c8a0000`, no `jsr`/`bsr` ran |
| `sr` (= SSR) | `0x60000101` | `go.S`'s SR plus T=1 — `go.S` ends with `clrt`, so a compare ran |
| `vbr`, `fpscr` | `0x8c00f400`, `0x40000` | exactly as `go.S` left them |
| r0b0..r7b0 | `0x60000101`, `0xa0000000`, …, `0xac010000` | `go.S`'s own registers, banked out at `ldc r0,sr` |
| r8..r14 | 0 | `go.S` zeroed them |
| r15 (= SGR) | `0xffffff8e` | `& 3 == 2`: a longword access through it is misaligned — that *is* `0x0e0` |
| fr0..fr7 | `0x8c8a00fc`, …, `0xdeadbeef` | leftovers of the loader's `fmov.d` copies, not the title's |

So the title ran a handful of straight-line instructions and died. Nothing
relocated.

## 2. What was ruled out

- **Transport / scrambling.** `0WINCEOS.BIN` extracted from the GDI (LBA 548536,
  1 257 472 B, track 21, `+16` in each 2352-byte sector) holds 43 ASCII runs of
  ≥ 40 characters, e.g. `d:\dragon11\private\gemini\d3dim\d3dim\wince\..\d3ddev.cpp(1932):`.
  Descrambling leaves one. The file is plain; a GDI always is.
- **Something CE-specific in IP.BIN.** Compared with Crazy Taxi, Snow Surfers,
  Sonic Adventure and Sonic Adventure 2:
  - code header `0x100..0x800`: 1525 non-zero bytes on all five; Sega Rally 2
    differs from Crazy Taxi in 72 bytes of metadata at `+0x108..+0x150` and one
    byte at `+0x28e`;
  - bootstrap 1 (`0x800..0x6000`): identical except one 5890-byte run at
    `0x8c00b820..0x8c00cf22`. **That run is the licence-screen bitmap**, loaded
    at `0x8c008804` (`mov.l 0x8c00883c,r3 ! 8c00b820`) into a struct passed to
    a blit routine with a framebuffer address and 300/298/320/90. It is not
    code. (An earlier reading of it as "the CE starter" was wrong.)
  - bootstrap 2 (`0x6000..0x6154`, 340 B): byte-identical to Sonic Adventure
    2's. It is a handoff stub: CCR = 0, SR = `0x700000f0`, r0–r14 = 0,
    SP = VBR = `0x8c00f400`, FPSCR = `0x40000`, a byte loop zeroing
    `0x8c00fc00..0x8c010000`, then `jsr @*(0x8c00e004)` = `0xac010000`.
  - bootstrap 1's own entry (`0x8c008300`) sets CCR `0x92b`, r15 `0x7e001000`,
    PR `0xac00b700` and jumps to the licence code; `0x8c00b700` is the region
    string table (`bra` over each 32-byte slot, "For EUROPE."), then
    `r15 = 0x8d000000; jmp 0x8c00d820`, the common loader, which calls the GD
    driver through `0x8c0000bc` with `r6 = 0, r7 = index`. Identical on Crazy
    Taxi.

## 3. What the disc is

The root directory (track 3 PVD at file sector 16, root at ISO LBA 45020):
`NK.EXE` (CE kernel, 279 504 B), `NKNODBG.EXE`, `NKSCIFKD.EXE`, `COREDLL.DLL`,
`GWES.EXE`, `FILESYS.EXE`, `DDRAW.DLL`, `D3DIM.DLL`, `DSOUND.DLL`,
`WSEGACD.DLL`, `QUARTZ.DLL`, fonts, ~250 files, and **`RALLY2.EXE`**
(851 512 B) — the game. `0WINCEOS.BIN` is the ROM image that starts CE.

## 4. The gate: BIOS driver or G1?

If CE drove the ATA registers directly, a network-served disc would be
impossible. Absolute constants such as BIOS vectors are not relocated in a PE,
so a literal scan of every executable on the disc is sound for them:

| file | `0x8c0000bc` | `0x8c0000c0` | `0x8c0010f0` | G1 `0x?05f70xx` |
| --- | --- | --- | --- | --- |
| `0WINCEOS.BIN` | 0 | 0 | **23** | 0 |
| `COREDLL.DLL` | 0 | 1 | 0 | 0 |
| `WSEGACD.DLL` | 0 | 0 | 0 | 0 |
| `NK.EXE` | 0 | 0 | 0 | 1 (`0xa05f70e0`) |

(Car `.BIN` files and `ADVTELOP.DLL` also match the G1 pattern; they are
textures and data.) The 23 literals in `0WINCEOS.BIN` sit at
`0x88d10..0x8a9dc`, all 4-aligned, in literal pools of a module linked at
`0x01dbxxxx`. Every one is loaded and called as `jsr @rN` with `r6 = 0` and
`r7` = function index — across 33 call sites: 0 ReqCmd ×18, 1, 2, 3
InitSystem, 4, 6 ReqDmaTrans, 7, 8, 11, 12, 13 (PIO). **CE calls the BIOS GD
driver, by its body address rather than its vector.** The gate is open.

For comparison, the four Katana test titles' `1ST_READ.BIN` each carry
`0x8c0000bc` 13 times and neither `0x8c0010f0` nor `0x8c0000c0`.

This is isoldr's third door (`gdc_syscall_patch()`, `syscalls.c:1687`): for
every non-KOS binary it writes a trampoline over `0x8c001000`/`0x8c0010f0`, or
rewrites the literal `0x8c0010f0` in the title. dcload held only `0xac0000bc`
until 2026-09-26, so a CE title's disc access went to the physical drive.

## 5. isoldr, for reference

(`/opt/toolchains/dc/kos/ds/firmware/isoldr/loader`, `modules/isoldr/module.c`.)

- **Detection**: boot file named `0WINCEOS.BIN` → `BIN_TYPE_WINCE`,
  `exec.lba++`, `exec.size -= 2048`; else `"ECEC"` at file offset 64 → WinCE
  with no skip. `exec.addr = 0xac010000` for every type.
- **Boot mode**: all 98 DreamShell presets with `type=3` use `mode=0` (DIRECT).
  `BOOT_MODE_IPBIN` and `_TRUNC` both enter bootstrap 2; bootstrap 1 is never
  run. The four `Load_IPBin()` patches land at 2×/4× their intended offsets
  (element indices on `uint16*`/`uint32*` where byte offsets were meant —
  compare `git show 40cd09f8`), and are inert anyway.
- **Interrupts** (only with `irq=1`, 42 of 98 CE presets, and only in isoldr
  builds with `ENABLE_IRQ`): `exception_init(*(u32*)(exec.addr+0x0c) + 0x30)`
  before launch; three instructions at `VBR+0x600` become `nop; bra; nop`,
  the trampoline body goes to `VBR+0x5EC`, the title's handler resumes at
  `VBR+0x606`. For CE only, the trampoline keeps `mov.l interrupt_stack, r15`
  (`0x8c011000`) and `wince_entry` rebuilds r0/r1/T from bank-1 r7
  (`@(8,r7)`, `@(0x0C,r7)`, `@(0x28,r7)`) and `*(VBR+0x68c)`, restoring r15
  from SGR.
- **MMU**: nothing. `mmu_disable()`/`mmu_restore()` are never called,
  `boot_stub` never writes MMUCR, and buffer addresses go to `addr & 0x1fffffff`.
- Others: heap kept low (`0x8c008800`), AICA DMA made blocking for CE
  (`PCM_TRANS_DMA_BLOCKED`), SCIF logging suppressed inside interrupts.

## 6. The framing question — settled 2026-09-26: skip the first sector

On this disc, with isoldr's one-sector skip, the payload opens
`bra +0x18; nop` and then code whose literals are `0x16088`, `0x40e80`,
`0x468b8` — the same low, CE-virtual character as offset 0, whose first
instructions read `*(0x0000000c)` (BIOS ROM word `0x240a5039`) and
`jsr @0x00040f58`. `*(payload+0x0c)+0x30` is `0x67d31ff4`, not a VBR, and no
4-aligned offset in the first 128 KB gives one. `"ECEC"` appears nowhere in the
file.

**What settles it is the CE ROM header.** `ROMHDR` sits at file offset
`0x132638`: `dllfirst 0x01c60000`, `dlllast 0x02000000`, `physfirst
0x8c010000`, `physlast 0x8c1421d4`, 24 modules, `ulRAMStart 0x8c143000`,
`ulRAMEnd 0x8cef0000`, `ulDrivglobStart 0x8cef0000` (+`0x110000`). The module
table follows it, and its name pointers resolve to `nk.exe`, `coredll.dll`,
`filesys.exe`… **only if file offset `0x800` is at `0x8c010000`**; from offset
0 they land in code. The pointer to `ROMHDR` itself (`0x8c141e38`) is at file
offset `0x2408`. So the ROM image starts one sector into the file, as isoldr
and the host have it, and the first sector is not loaded by CE's own
arithmetic either. (No ISO extended-attribute record explains the sector: the
directory record's XAR length is 0.)

The module table, for reading CE code: code sections execute in place at
`data`, a physical address whose file offset is `data - 0x8c010000 + 0x800`,
and are mapped at `real`. `wsegacd.dll` — vbase `0x01db0000`, code
`0x8c098000` (file `0x88800`) mapped at `0x03db1000` — is **the GD driver and
the CD file system in one module**; a module's own calls use `vbase`-relative
addresses, so for it `V = F + 0x01d28800`. Others: `nk.exe` (`0x8c010000`),
`coredll.dll` (vbase `0x01e00000`), `filesys.exe`, `gwes.exe`, `device`
(`wdevice.exe`), `wdmlib`/`wdmoem`, `maple.dll`, `ddraw`/`d3dim`/`ddhal`,
`sndcore`/`dsound`, `dinputx`, `ole32`, and the game's `MGame*.dll`. There is
no separate CDFS module: `IOCTL_CDROM_READ_SG` (`0x24c04`) appears only in
`wsegacd.dll`.

## 7. What changed (2026-09-26)

Loader (`_end` `0x8c00bfc8` → `0x8c00c000`, +56 B, zero margin left):
- `cdfs_redir.s`: `0xac0000c0` held with `0xbc` (saved, restored, armed
  together); `_gd_bios_entry` exported.
- `cdfs_syscalls.c`: `overlaps_dcload()` from `dcload_base`/`end`/`.hiram`
  instead of `0x0c004000..0x0c010000`.

Host (`dcload-ip-rs`):
- `boot::WinCe`: CE detected by name or `"ECEC"`; the first sector of
  `0WINCEOS.BIN` dropped; `--no-wince` to upload whole.
- a CE title is entered through the disc's second bootstrap (as
  `--boot-ipbin`), automatically.
- `dispatch::gd_body_patches`: every 4-aligned `0x8c0010f0`/`0xac0010f0` in the
  payload rewritten to the running loader's `_gd_bios_entry`, verified, and
  guarded against reloads. For every title; no Katana title measured has one.
- `identify` prints `wince` and `gd body` lines.

## 7b. First console run with the doors held (2026-09-26)

Entered at `0x0c00e000` (the disc's second bootstrap), loader at `0x8c8a0000`.
**CE reached the emulated GD driver for the first time**: two TOC requests
(408 B each) into `0x080df654` and `0x080df2e0`, then a 16 KB `ReadSector` at
LBA `0x9e80f` (649231) into `0x0ce99000`. The disc ends at LBA 549300, and the
host died on the out-of-range read (`read_sector(...)?` in `receive_syscalls`).

- The TOC destinations are **not physical RAM**: they are CE process-slot
  addresses, valid only through the MMU. The loader writes them with CPU stores
  in the title's context (`cmd_partbin` does not convert addresses), so the MMU
  should translate them — unverified. If it did not, CE computed its read from
  an uninitialised buffer, which would explain the LBA.
- The read's destination `0x0ce99000` is page-aligned and inside RAM: most
  likely a physical DMA buffer.
- CE's GD driver issues, by static count of its `ReqCmd` sites: 16 PIOREAD,
  17 DMAREAD, 19 GETTOC2, 20-23, 24 INIT, 26, 27 SEEK, 30 REQ_MODE,
  31 SET_MODE, 33, 34, 36 REQ_STAT, 38, 39. REQ_MODE is one the emulation
  completes without writing anything back.

Host changes after this run: an unservable read is answered with a failing
ReturnValue and the session stays up; every TOC is read back from where it was
written and the log says whether it arrived (`toc_readback`).

## 7c. The TOC read-back froze the console (2026-09-26, second run)

The host read the first TOC back with `SendBinQ` from `0x080df654`, inside the
title's GETTOC2 wait, right after `send_data` had delivered it (echo and
DoneBinary both answered, so the WRITE through the virtual address went
through). The read never answered, and nothing did after it — the stack watch
went silent for good. `cmd_sendbinq` reads with the same CPU loads and the
same alignment handling (`SH4_aligned_memcpy`, 32-bit here) as `cmd_partbin`
writes, so why the load hangs where the store did not is not known; a TLB miss
taken from inside the loader's packet path is the obvious suspect. The
read-back was removed the same day: an instrument that reads a title's virtual
memory from inside its GD call is inside the blast radius (AGENTS.md 14.13).
The host now logs the TOC it sends (`debug`) instead.

## 7d. Third run: the TOC is right, the LBA comes from elsewhere

The host logged both TOCs: area 0 first `0x41010000`, last `0x01020000`,
leadout `0x4100b05e`; area 1 first `0x41030000`, last `0x41150000`, leadout
`0x410861b4` — correct for this disc, in KOS's format. CE then asked for LBA
`0x9e80f` all the same, got the failure, and stopped calling the driver
(black screen; `--diag` unanswered, since dcload only listens inside GD calls).

- In CE's GD driver the read's `param[0]` is copied from a caller argument
  (`0x8a4b0..0x8a4c2`): the LBA is computed by the layer above, which a real
  drive would never have given this sector to.
- No combination of the TOC's FADs with the usual offsets gives 649231, and the
  value appears nowhere in `0WINCEOS.BIN`.
- isoldr answers REQ_MODE/SET_MODE exactly as we do (force-complete, "TODO").
  One real divergence: for area 0 on a GD, isoldr reports leadout
  `0x01001A2C` (FAD 6700, CTRL 0) where this host reports `0x4100b05e`.

Instrument for the next run: a `GD_TRACE=1` loader set, deployed to the host's
`loaders-trace/` (md5 `04c43193…`, `_end` +56 B, HIGH family so the bound does
not apply), used with `--loader-dir loaders-trace`. It sends every `ReqCmd`
(`R cmd param0 param1`), refusal and `GetCmdStat` to the host console, so the
commands CE issues before the bad read, and their answers, become visible.

## 7e. Fourth run, with the trace: where 0x9e80f comes from (2026-09-26)

The `GD_TRACE` set gave CE's whole conversation with the driver:

```
R 18 0 0            INIT                     S 2 2 0     completed
R 13 0 080df654     GETTOC2 area 0           S 3 2 198   completed
R 13 1 080df2e0     GETTOC2 area 1           S 4 2 198   completed
R 11 9e80f 8        DMAREAD FAD 0x9e80f, 8   S 5 1 0     processing
  ReadSector 0x9e80f -> 0x0ce99000 refused by the host (past the disc)
F 9e80f 0ce99000 0  the chunk failed         S 5 1 0     processing; nothing more
```

No REQ_MODE, SET_MODE, GETSES or SEEK: **the LBA is computed from the two
TOCs alone.** Reading `wsegacd.dll` (section 6 for the mapping):

- **Media check** (`0x8c8b4`): `IOCTL_CDROM_READ_TOC` with no input → area 0
  (the first GETTOC2), then the **mount** (`0x8c0bc`): `READ_TOC` with input
  byte 1 → area 1 (the second). The two buffers are the driver's locals, 884 B
  apart — exactly the mount's extra frames.
- **The driver's READ_TOC** (`0x8a014`): `memset(buf, 0xff, 408)`, GETTOC2 into
  it, `bswap32` each word (`0x89aac`), then FirstTrack = byte 1 of word 99,
  LastTrack = byte 1 of word 100, and for each track `word[track-1]` → CTRL
  (low nibble), and FAD (bytes 1..3) → MSF with `/75`, `%75`, `/60`, `%60`
  into CE's `CDROM_TOC`. **Our format is exactly what it expects**; with our
  table the mount computes 45016.
- **The mount**: the first data track of area 1 (CTRL bit 2), `LBA =
  M*4500 + S*75 + F - 150`, `+16`, reads the volume descriptors from there
  (`0x8c258`, through `IOCTL_CDROM_READ_SG`). The DeviceIoControl's return is
  not checked.
- **The read**: `LBA + 150` → FAD, and a 1-sector request is filled through an
  8-sector read-ahead into the driver's cache (hence 8).

**`0xffffffff` explains the number to the unit.** FAD `0xffffff` = 16777215:
M = (16777215 / 4500) & 0xff = **144**, S = (16777215 / 75) % 60 = **16**,
F = 16777215 % 75 = **15** → 144×4500 + 16×75 + 15 − 150 + 16 = **649081**,
+150 = **649231 = `0x9e80f`**. So CE converted a track entry that was the
driver's `0xff` pre-fill: **the area-1 TOC was not in CE's buffer.** If none of
it arrived, FirstTrack reads `0xff` and the "entry" is word 254, which falls at
`0x080df6d8` — inside the area-0 buffer, on an unused word, `0xffffffff`
either way; if only the tail arrived, word 2 is the pre-fill. Both give the
same LBA; neither is possible with the table in place.

Why the host's write to `0x080df2e0` (a CE process-slot address, MMU on) did
not reach what CE read is **not established**. It fits the read-back freeze of
7c: a read of the same virtual address from inside the loader hung the bus,
which is what an access to physical area 2 (`0x08000000`, nothing there)
does, and a posted write there is silently lost. ReqCmd's own reads of CE's
parameter block (same page) do work.

Also learned:

- **DMAREAD's buffer is physical.** The driver locks the caller's buffer and
  passes the page frame (`<< 12`), `0x0ce99000`, to DMAREAD — the G1 DMA takes
  physical addresses. dcload wrote it with CPU stores as P0, which under CE's
  MMU is a virtual address in process slot 6.
- **CE needs no hardware interrupt for a read that succeeds at once.** Its
  wait loop (`0x891ac`) calls ExecServer then GetCmdStat: COMPLETED → done;
  PROCESSING with `status[3] == 1` (`CMD_WAIT_IRQ`) → `STATUS_PENDING` and
  `EventModify(h, 3)` (= `SetEvent`) on its service thread's event, which
  calls the driver again; PROCESSING otherwise → `Sleep(5)` and poll;
  STREAMING → `gdGdcReqDmaTrans`/`SetPioCallback`. A read dcload answers in
  one ExecServer completes on that one kick. A **failed chunk** does not: the
  retry yields with `CMD_WAIT_IRQ` still set, and nothing wakes the thread
  again — which is the silence after `F`.

Changed the same day (loader):

- `GetTOC()` receives the table into `gd_toc_stage` (`.hiram`) and copies it
  to the title's buffer with `memcpy_32bit` — what the BIOS does (a CPU copy
  in the caller's context), without the host writing through the title's MMU
  and without the FPU (`SH4_aligned_memcpy` uses `fmov.d` for an 8-aligned
  destination such as `0x080df2e0`, 4-aligned `0x080df654` went through
  32-bit stores).
- `gdGdcReqCmd()` turns a DMAREAD destination into its P1 alias
  (`phys | 0x80000000`).
- `GD_TRACE` builds trace, around the copy: `M` (MMUCR, SR, PTEH), `U` (the
  UTLB entry mapping the buffer, probed from P2), and when mapped `P`/`V`
  (words 0, 2, 99 through the physical page and through the virtual address,
  read with interrupts masked, so the instrument can take no TLB miss).
- Default `_end` `0x8c00c000` → `0x8c00c038`: **56 B over the bound**
  (AGENTS.md 4.6). Deployed only as `loaders-trace/` (md5 `bf52347f…`);
  `loaders/` stays at `33a7947a…` until the console confirms.

## 7f. Fifth run: the copy lands in area 2 (2026-09-26 21:46)

Same command, `loaders-trace/` `bf52347f…` at base `0x8c8a0000`
(`gd_toc_stage` = `0x8c8acc00`, and the host's `LBIN` went there). The
sequence did not change (INIT, TOC 0, TOC 1, DMAREAD `0x9e80f`, `F`, silence),
but the trace names the cause:

| line | area 0 (`0x080df654`) | area 1 (`0x080df2e0`) |
| --- | --- | --- |
| `M` MMUCR SR PTEH | `ac007c01 40000001 fffffc03` | `ec00f801 40000001 fffffc03` |
| `U` VA, address, data | `080dff03 0ce9837c` | same entry |
| `P` before copy | `00000000 00000000 ffffffff` | `ffffffff ffffffff 00000000` |
| `V` before copy | `ffffffff` ×3 | `ffffffff` ×3 |
| `P` after copy | `ffffffff` ×3 | `ffffffff` ×3 |
| `V` after copy | `ffffffff` ×3 | `ffffffff` ×3 |

- MMU on (AT=1), SR `0x40000001` (MD=1, BL=0, IMASK 0), ASID 3. Both buffers
  are in one **4 KB copy-back page**, VPN `0x080df000` → PPN `0x0ce98000`,
  valid, dirty, PR=3, ASID 3, not shared.
- Before the copy, the virtual view holds the driver's `0xff` pre-fill and the
  physical view does not yet: the fill is still in dirty cache lines. That is
  the instrument working (positive control).
- **After the copy the virtual view is unchanged.** The table should have put
  entry 0 (area 0) and entries 2 and 99 (area 1) there. The physical view
  turning `0xff` is only those dirty lines being written back meanwhile.

**Cause:** `memcpy_32bit` — like every copy in `memfuncs.c`, and so
`SH4_aligned_memcpy` in `cmd_partbin` — computes its offset with `memdiff()`,
which masks **both** addresses to 29 bits, and stores at `src + diff`: into the
**source's segment**. From the P1 staging buffer (or the P1 packet buffer, for
the host's direct write before) `0x080df654` became `0x880df654` — area 2,
which is empty on the Dreamcast. The 408 bytes went nowhere, every time. With
the MMU off (every Katana title) any address a title hands over is RAM in any
segment, which is why this never showed.

It probably explains two older observations too, neither re-tested:

- the §7c freeze: `SBIQ` of a virtual address loads through the MMU but
  stores into `pkt_buf`'s physical address **in the source's segment (P0)**, a
  translated store at an address CE does not map;
- AGENTS.md §8's P2 read-back bug: a `PBIN` to P2 from a P1 packet buffer is
  stored cached through P1, and `cmd_loadbin` does not purge a P2 destination,
  so `SBIQ` through P2 reads the RAM underneath.

**Fix, deployed as `loaders-trace/` only** (md5 `1ff3c0af…`): `GetTOC()`
copies the staged table with a plain `volatile` word loop (disassembly checked:
`mov.l @r10+,r3; dt; mov.l r3,@r2`), so the stores go through the title's
translation. `memdiff()` itself is not changed: every copy in the loader goes
through it, and the hot paths were measured with it as it is. A **PIOREAD**
into a virtual buffer would still land in area 2 through `cmd_partbin`; CE has
issued only DMAREAD so far. Default `_end` `0x8c00c030` (48 B over the bound);
`loaders/` unchanged (`33a7947a…`); CDI redeployed, all 17 sectors of the new
`1st_read.bin` found in it.

## 7g. Sixth run, and where CE's RAM is (2026-09-27)

`loaders-trace/` `1ff3c0af…`: the trace stopped after area 0's `P` line; the
`V` line never reached the host. The loader was still alive -- it answered the
host's `SBIQ` two seconds later, from inside a `bb->loop()` -- so it was waiting
for a `RETV`. `write()` sends once and waits for its answer with no deadline,
so one lost packet on a trace line hangs the title for good (the §14.13 lesson
again). The M/U/P/V instrument is now behind `GD_TRACE_VADDR` (default 0):
eight fewer round trips per boot. Redeployed as `loaders-trace/` `984b2348…`.

The host's stackwatch then said the console was "not running the loader":
`g_gd_sp_min` read back `0x080df62c`, which is CE's thread stack, a virtual
address. It is a false alarm; the host assumes every stack is in P1 RAM.

**CE owns almost all of RAM.** Its ROMHDR (at `0x8c141e38`) says:

| field | value |
| --- | --- |
| physfirst..physlast | `0x8c010000..0x8c1421d4` (the ROM image) |
| ulRAMStart / ulRAMFree / ulRAMEnd | `0x8c143000` / `0x8c14d000` / `0x8cef0000` |
| ulDrivglobStart / Len | `0x8cef0000` / `0x110000` (to `0x8d000000`) |

So the kernel's page pool is `0x8c143000..0x8cef0000`, and driver globals take
the rest up to `0x8d000000`. The pages CE handed out first (`0x0ce98000` for the
thread stack, `0x0ce99000` for the DMA buffer) are 352 KB under `ulRAMEnd`: it
allocates from the top. The loader the host placed at `0x8c8a0000` is inside
that pool and will be overwritten when CE's allocations reach it.

What the image itself names outside the pool (4-aligned literals):
`0x8c008000` and `0x8c008100`, each copied 256 bytes at a time by code next to
the GD driver's calls (**CE reads IP.BIN's header at run time**); `0xac004000`
and `0xac008000`, read by the kernel to test for a word `0x000b003b` there;
`0xacef8f00..03` in driver globals. isoldr's two Sega Rally 2 presets are
`0x8c001100` and `0x8c004000`; its 13 KB image ends below `0x8c008000`. Ours is
32 KB and cannot live there.

The clean place is one CE is told not to use: lower `ulRAMEnd` by the loader's
span in the uploaded image (the ROMHDR is data CE reads at boot, and the only
place `0x8cef0000` appears as `ulRAMEnd`) and put the loader at the new
`ulRAMEnd`, between the pool and the driver globals. That is a host change and
has not been made.

## 7h. Seventh run: the TOC fix works; CE streams (2026-09-27)

`loaders-trace/` `984b2348…` (TOC copied with a plain loop). CE now reads what
it should: DMAREAD at FAD `0xb06e` (the PVD, `01 43 44 30 30 31` = `CD001`),
`0xb072` (the root directory), `0x8578c` (`MZ`, an executable), then
**`PIOREAD_STREAM_EX` (39)** at `0x8578c` for `0x11c` sectors, 39 at
`0x858a7`/`0x1e`, **`DMAREAD_STREAM_EX` (38)** at `0x858c4`/`0x3e`, 39 at
`0x85902`/`0xa` -- each force-completed with nothing delivered (`S … 2 0`) --
and then nothing. Consecutive requests overlap by one sector: the file system
reads byte ranges.

How CE's driver (`wsegacd.dll`) runs a stream, from the disassembly
(dis addresses, `V = dis - 0x8a2e7000`):

- The request handler (`0x8c099e40`) splits a scatter-gather read into a head,
  whole sectors and a tail. **Aligned** (no head, no tail) goes to the DMA
  builder (`0x8c099a84`): DMAREAD when the locked pages form one physically
  contiguous run, **DMAREAD_STREAM_EX** otherwise -- which, with pages handed
  out top-down, is most multi-page buffers. Everything else goes to PIO:
  PIOREAD for one buffer, **PIOREAD_STREAM_EX** for a list, whose entries are
  the caller's *virtual* buffers.
- The poll loop (`0x8c0989ac`: ExecServer, GetCmdStat) on STREAMING, first
  time: for DMA, `ReqDmaTrans` of the first piece; for PIO,
  `SetPioCallback(0x01db55f0, ctx)` and the step function `0x01db1878`, which
  does `CheckPioTrans` and, while bytes are owed, `ReqPioTrans` of the next
  `min(entry, owed)` bytes. The callback is that step function. So **PIO needs
  no interrupt**: the driver that delivers a piece calls the callback, which
  asks for the next.
- **DMA pieces are chained by an interrupt thread** (`0x8c098650`) waiting on
  SYSINTR 21, the G1 DMA-end interrupt: `CheckDmaTrans`, then `ReqDmaTrans`
  of the next list entry. The GD service thread waits on SYSINTR 20 with a
  15 s timeout (`ReadAbort` when it expires). Neither interrupt exists on this
  transport. isoldr's Sega Rally 2 preset has `irq=1` and `dma=1`: it gets the
  real interrupts from its IDE/GD devices.

What was done:

- **Loader**: streams implemented (`data_stream()`): STREAMING, pieces served
  a stage at a time from `gd_stage` (now 4 KB, shared with the TOC) and copied
  with `memcpy.S` -- to the address given for PIO, to P2 of the physical page
  for DMA -- and, for PIO, the title's callback called from the server while
  bytes are owed (isoldr does the same). `Req*/Check*Trans` follow the BIOS
  contract (1 while a piece is pending, else 0 and the bytes owed). PIOREAD
  and DMAREAD into a translated address also go through `gd_stage`
  (`gd_is_virtual()`: P0 with MMUCR.AT). GetCmdStat reports STREAMING.
- **Host** (`src/wince.rs`): the one branch that sends an aligned read to DMA
  (`0x8c099f74`, `bf` → `bra`, word `0x51f6a01e`) now goes to PIO, so CE never
  starts a DMA stream. Its single-buffer DMA reads stay DMA and complete
  without an interrupt.
- **Host**: the loader goes at the top of CE's RAM, `0x8cee0000`, and the
  upload lowers `ulRAMEnd` to it (§7g). The syscall loop ignores reads into
  the loader's `_gd_stage`. The stackwatch says "virtual stack" instead of
  "not the loader".

Default `_end` is now `0x8c00c218` (536 B over the bound); `loaders/` is still
`33a7947a…`. `loaders-trace/` is `0eff9990…`. The host must be rebuilt.

## 7i. Eighth run: streams work; one lost request stops CE (2026-09-27)

Host rebuilt with `src/wince.rs`, `loaders-trace/` `0eff9990…`, loader at
`0x8cee0000`. **The PIO streams work**: CE read the PVD, the root, then
several modules (`MZ` at `0x8578c`, `0x8592c`, `0x85931`, `0x85947`), UTF-16
file names, streams of `0x11c`, `0x1e`, `0x3e`, `0xa`, `0x21` sectors, each
served in one ExecServer with the pieces chained through CE's callback (`Q`
lines) -- **1.04 MiB in 0.74 s, 228 reads**, no DMA stream ever requested
(the host's PIO patch held).

Then a DMAREAD of 8 sectors at `0x8594f` never reached the host (no
`Received ReadSector` for it): the loader's 250 ms deadline expired (`F`),
the server yielded to retry, and GetCmdStat answered PROCESSING with
`status[3] = CMD_WAIT_IRQ`. CE's GD thread reads that as "the drive will
interrupt", returns STATUS_PENDING and sleeps on SYSINTR 20 (15 s, then
ReadAbort, then again), so nobody called ExecServer: the retry never ran,
the loader stopped answering, black screen. The risk named in §7e.

Fixed: with the MMU on, the retry yield clears `status[3]`, so CE sleeps 5 ms
and polls, which drives the retry. A failed chunk inside a stream is retried
at once instead of from the next ExecServer, because a title polls a
STREAMING channel only on an interrupt. `loaders-trace/` `83016196…`.

## 7j. Ninth run: the trace itself hangs it (2026-09-27)

`loaders-trace/` `83016196…`. The same load as §7i up to the stream at
`0x85902` (800 KiB, 191 reads); its `S … 3` (STREAMING) arrived, the `Q` line
the server writes next never did, and the loader then answered the host's
stack read from inside `bb->loop()`: waiting for the `RETV` of a trace line
whose packet was lost. `write()` sends once and waits with no deadline, so
under GD_TRACE a single loss is permanent -- the §7g failure again. Both
losses of §7i and §7j are DC→host and come just after CE has run between two
of our packets; the cause is not known (a G2 collision with CE's own traffic
is one candidate, §4.5 known gaps). Disc reads now survive a loss; the trace
does not. Next run without GD_TRACE: `loaders-wince/` `895fb76e…`, the
default build (`make loaders`), kept out of `loaders/` because of `_end`.

## 7k. Tenth run: the title boots; a manual save crashes it (2026-09-27)

`loaders-wince/` `895fb76e…`, no trace. **Sega Rally 2 boots and reaches its
menus.** A manual save from the menu then crashed the console, with nothing
in the host log: the last disc activity was 444 KiB read contiguously from
`0x5294c` (sound data, by the look of it), then only the stack watch finding
no loader to answer -- which is normal while CE makes no GD call.

What was found by reading the loader rather than the log: `ReadSectors()`
wrote the **post-mortem block** at the fixed address `0x8cf0c000` on every
disc read (nine words, `pm[PM_READS]++` and `pm[PM_CHUNKS]++` among them).
The address predates relocation and was never moved with the loader. For CE
it lies inside the driver globals, `ulDrivglobStart 0x8cef0000 + 0x1c00c`:
some driver's state was being incremented on every sector the title read.
A driver used for the first time at a save (the VMU path) would be the first
to mind. Nothing read the block, so it was removed rather than moved
(`_end` 0x8c00c218 → 0x8c00c180). Not proven to be the crash: next run tells.
Next set: `loaders-wince/` `027ebad2…`.

## 7l. Eleventh run: a stall of the ring, then stale answers (2026-09-27)

`loaders-wince/` `027ebad2…` (post-mortem block removed). Black screen before
the menu. The end of the log: a read of LBA `0x64a68` into `gd_stage`, asked
at 59.151, .401, .652, .903 -- four 250 ms deadlines with nothing accepted --
then at 60.153, .155, .156, .157, .158: five attempts one millisecond apart,
then nothing. The loader never answered the stack watch again.

Read against the code: a disc read's ReturnValue carries `address 0`, and
`cmd_retval()` stops RX. Whatever held the ring for ~1 s let go at 60.15,
and the late answers of the timed-out attempts arrived in order: each late
ReturnValue ended the current attempt's wait over a window its own parts had
not filled yet, `bin_window_complete()` said hole, and the attempt failed in
a millisecond. The retries (`GD_READ_RETRIES` 4) burnt, the stream FAILED,
and CE's driver does not come back from that.

Two changes (`loaders-wince/` `c717aa17…`): `ReadSectors` waits past a
ReturnValue over an incomplete window until its own deadline
(`g_cdfs_read_stale`), and under the MMU a read gets 20 retries (5 s) instead
of 4. **Not addressed: what stalls the ring for a second.** A G2 collision
with CE's sound streaming (the data read just before looks like PCM) is the
candidate named in AGENTS.md §4.5's known gaps (`g2_lock()`); `g_rx_hdr_defer`,
`g_rx_resync`, `g_rx_overflow` and `g_rx_reinit` would say, if read after a
stall that the title survives.

## 7m. Twelfth run: the save starts, then everything stops (2026-09-27)

`loaders-wince/` `c717aa17…`. The menu is reached again and a manual save
shows its animation, then the picture freezes and the music stops. The log's
last disc reads (`0x528ac..0x528d2`, 2 sectors each into `gd_stage`) all
completed; nothing after them, and the stack watch finds no loader to answer.
The music is CD-DA played by the loader, and it stops when the title stops
calling the GD driver: CE as a whole, or its GD thread, stopped.

Candidate, from the preset rather than the log: DreamShell gives this title
`cdda=0x12210` = SRC_DMA | DST_SQ | POS_TMU2 | **CH_ADAPT** -- isoldr picks
free AICA channels for it instead of the fixed 62/63 (the host warns about
it on every run). A save adds voices; if CE's sound driver takes 62/63, our
engine's key-off, rewrites and theft restarts land on its voices. A/B without
a build of the host: `loaders-wince-nocdda/` `c7a27adf…` (`WITH_CDDA=0`, no
AICA access at all). If that saves, the next job is adaptive channels.

## 7n. Thirteenth run, without CD-DA: the freeze moves to loading the save (2026-09-27)

`loaders-wince-nocdda/` `c7a27adf…` (no AICA access at all). The title froze
at start-up while **loading** its save, before the menu; the disc reads before
it all completed, with no retry. So the freeze is not our CD-DA channels (or
not only them): it follows the memory card. The previous two runs had crashed
*during* a save to the same card -- the VMUPro in A1, which the host had
switched to the game's own card (`MK-5101950`) before `EXEC` -- so that card
may now hold a half-written save, and this run may have frozen on reading it.
Next: `--no-vm2` and a plain VMU, then the same save on the VMUPro without
dcload as the control.

## 7o. flycast, through dcload: CE preempts the loader (2026-09-27)

The user reproduced the freeze at "Save and load" under flycast with
`loaders-wince/` `c717aa17…` (no music there either). Attached over GDB after
the freeze:

- **The loader is intact**: `scripts/dc-integrity.py --elf <relocated ELF>`
  finds `.text` equal to the ELF except the state words `cdfs_redir.s` keeps
  there (`cdfs_saved`, `saved_regs`, the GD lock), `.rodata` except the painted
  IP/MAC strings, and `exception.bin` except the six relocated jump-table
  literals. CE does not overwrite the loader.
- **`g_gd_in_transfer` = 1 while the CPU runs user code** (SR `0x8001`, PC =
  r0 = `0xfffffd3b`, a CE API trap address, PR in a DLL loaded from disc).
  The GD thread was in `ReadSectors`'s wait and CE's scheduler had switched
  to another thread under it.
- The read in progress: `PIOREAD_STREAM_EX` (39), LBA `0x85d43`, 31 sectors,
  1 KB delivered, a 61048-byte piece pending. **`g_cdfs_read_fails` 49 =
  `g_fine_timeouts` 49 = `g_cdfs_read_stale` 49 = `g_rx_overflow` 49**: every
  attempt overflowed the ring, got only its ReturnValue, and ran out its
  deadline -- time that passed while the loader was not running.
- Caveat: flycast's GDB stub stops on every MMU exception while attached, and
  every CE API call is one; the PC above is where our attach stopped it, and
  flycast stayed halted.

Change: the adapter loops mask interrupts (IMASK 15) while MMUCR.AT is set
(`bb_irq_hold()`). `loaders-wince/` `d100eb80…`, `_end` `0x8c00c2c8`.

## 7p. flycast again, with the loop masked: the switch happens before the loop (2026-09-27)

`loaders-wince/` `d100eb80…`. The freeze came in the middle of saving. One
GDB attach, everything read in it: CE's kernel was in its scheduler (PC
`0x8c0176da`, SR `0x40008000`), alive; the loader's `.text` intact again;
`g_gd_in_transfer` 1 again; the read in progress a `PIOREAD` (16) of 301
sectors into a virtual buffer, at LBA `0x528d2` -- **the same LBA as the last
request in the console log of the save crash (§7m)**. `g_cdfs_read_fails` 35
= `g_fine_timeouts` = `g_cdfs_read_stale` = `g_rx_overflow`.

flycast's RTL8139 raises RxOverflow only when the ring has no room
(`rtl8139_do_receive`), so the ring really was full: the loader had not read
it for a long time with reception on. `build_send_packet()` turns reception
on and transmits before `bb->loop()` -- where the mask of §7o began -- and
after a deadline reception stayed on while the title ran. Now: IMASK 15 over
each whole exchange (`ReadSectors`, `GetTOC`, `cdda_fetch`) and `bb->stop()`
at the end of each. `loaders-wince/` `ffea9faa…`, `_end` `0x8c00c380`.

## 7q. Masked across the exchange, and still parked: the stack is virtual (2026-09-27)

`loaders-wince/` `ffea9faa…`, flycast, "random freezes". One attach: the
kernel's scheduler again (PC `0x8c0176da`), `.text` intact,
`g_gd_in_transfer` 1, a `DMAREAD` of 8 sectors to `0x8ce89000` at LBA
`0x85b7c`, `g_cdfs_read_fails` 28 = `g_rx_overflow` 28 = `g_cdfs_read_stale`
28. With IMASK 15 over the whole exchange the GD thread still left it, so it
left through an exception: the loader runs on the caller's stack, CE's thread
stacks are virtual (`g_gd_sp_min` `0x0205f0c4`), and CE's TLB-miss handler
only refills a page it already has (`0x8c012510`: `ldtlb; rte`); anything
else goes to its general handler, which sets SR to `0x40008000` (IMASK 0)
before it does anything (`0x8c012234`). Now `gd_exchange()` switches to the
loader's own stack (`_stack`, P1) under the mask for the exchange.
`loaders-wince/` `c6d906f4…`, `_end` `0x8c00c330`.

## 7r. The freezes are gone; the menu stutters, and CD-DA misbehaves (2026-09-27)

`loaders-wince/` `c6d906f4…` (exchange masked and on the loader's stack). No
freeze reported. Two new symptoms:

- **The menu stops ~0.5 s every ~3.5 s.** The host log shows why: the menu
  music is streamed as data, a `PIOREAD` of 604 KiB (151 two-sector round
  trips through `gd_stage`, 0.39-0.47 s) every ~3.5 s -- 172 KB/s, i.e. 44.1
  kHz stereo PCM. First tried: yield after every chunk under the MMU (CE's
  thread Sleep(5)s and polls). **Wrong, see 7s.**
- **In race, the CD-DA volume jumps to full at random, and there is ADPCM
  corruption.** The level was read at key-on only and 0 was taken for "never
  set" = full: a fade to 0 followed by a new track played it at full. It is
  now re-read by the channel watchdog and 0 mutes once any level has been
  seen (two zero reads in a row). The corruption is not explained; the first
  suspect is CE's sound driver owning sound RAM where our rings sit (below
  `0x150000`, chosen for Snow Surfers). A/B: `loaders-wince-ringtop/`
  `87488728…` (`CDDA_RING_TOP=0x200000`) against `loaders-wince/` `b7bf89c6…`.

## 7s. Yielding per chunk made it worse; the stage is three sectors (2026-09-27)

With the per-chunk yield the menu stopped for longer and the music stopped
afterwards. The counters of that session: `GetDrvStat` 0 -- CE never calls
it, so CD-DA is fed only by the GD server's own activity --
`g_cdda_svc_gap_max` 550 ms, `g_cdda_room_min` 79 ms (the lead nearly gone);
one read failure, no overflow. So the title waits for the whole read, and a
Sleep per chunk lengthens it: throughput is what counts. The yield is
removed, and `GD_STAGE_SECTORS` is 3 (6 KB, what `.hiram` has room for
without moving anything): 101 round trips for the menu's 604 KB instead of
151. More needs a larger stage, i.e. a layout change on both sides.
`loaders-wince/` `d3b68f5c…`, `loaders-wince-ringtop/` `f0de1002…`.

A recording of the in-race music (console, 24.7 s) matches none of the 18
audio tracks (best normalised correlation 0.08 over the first 4 s), so what
was heard is not a CD track played cleanly -- engine sound mixed in, data
streamed audio, or our output wrong from the start; not settled.

## 7t. The menu music is CD-DA, and the ring was replaying itself (2026-09-27)

A console recording of the menu (23.5 s, "menu music SR2.wav") against track
8 (the counters' `g_cdda_last_lba` `0x2bb52` = FAD 178962): polarity
inverted by the capture, 1-s windows relocated over the whole track, corr
0.6-0.96. Every **1.386 s** -- exactly our ring, 26 sub-fetches -- the audio
jumps back, and each loop starts **0.1135 s** (~2 sub-fetches) later than the
one before: the AICA was replaying the ring with two new sub-fetches per loop.
Counters: `GetDrvStat` 0 (CE never calls it), `g_cdda_svc_gap_max` 550 ms,
`g_cdda_room_min` 60 ms, `g_cdda_mutes` 0. The services come from ExecServer
only, hundreds of ms apart, and `CDDA_FETCHES_PER_SERVICE` 2 assumed one per
frame: the lead ran out, and the lead -- modulo one loop -- then read "full"
while the AICA replayed stale audio (the flaw §4.13 names). `cdda_fill()` now
budgets the sub-fetches the gap consumed plus two, up to the whole lead.
`loaders-wince/` `9f3e374d…`, `loaders-wince-ringtop/` `c6fde3c2…`.

## 7u. The menu stop: a bigger stage, a shorter pause (2026-09-27)

CD-DA much better with the catch-up (one mute, `g_cdda_room_min` 11.7 ms,
`g_cdda_svc_gap_max` 704 ms -- close to the 893 ms lead; residual glitches).
The menu still stopped, shorter. From the host log's timestamps a round trip
costs ~1.5 ms fixed plus ~0.17 ms a sector, so the number of trips is what
the title waits for. Two changes:

- **dcload**: `gd_stage_big`, 5 sectors (10 KB), in a NOLOAD section
  `.gdstage` right after `_end` -- the loader's own stack region, which is
  dead while a title runs -- used for reads into translated buffers and for
  streams under the MMU only (in the LOW family that range is a Katana
  title's stack). The network exchange moved off `_stack` onto the Maple DMA
  page (4 KB, unused under a title). 302 sectors: 61 trips instead of 101.
- **host**: the pause after the LoadBinary (600 us, sized for a 16 KB
  window's cache purge) is scaled to the window, never below 100 us; and the
  staging exemption covers `_gd_stage_big` as well as `_gd_stage`.

`loaders-wince/` `8d190907…`. The host must be rebuilt for the second half.

## 8. Next: the interrupt hook

Everything the loader does for a CE title happens when CE calls it, and CE
calls it only when it uses the drive. Two symptoms remain and both come from
that: a streamed read is served synchronously while the title waits (§7u),
and CD-DA is fed only when ExecServer runs (§7t). isoldr answers both with an
interrupt hook (`exception_init` + `wince_entry`, §5): the loader would serve
a read in the background and feed the music on its own schedule, completing
the request the way the drive does. Points to settle first:

1. Which interrupt: CE owns the VBR (`0x8c012110` in this image) and TMU0 (its
   tick); a G1/GD interrupt the driver already waits on would let the loader
   complete a request by raising what CE expects (status[3] = WAIT_IRQ, then
   the GD SYSINTR) instead of by polling.
2. The network from interrupt context: nothing may transmit while `pkt_buf` or
   `bin_info` is in use (§4.5 invariants), and the exchanges already run
   masked on their own stack (`gd_exchange`), which is the property an
   interrupt-driven service needs.
3. Re-verify the VBR before patching it, as isoldr's `exception_vbr_ok()`
   does; a wrong patch shows no screen and no exception.
