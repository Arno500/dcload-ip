# dcload-ip — Agent Notes

> The operating manual for AI coding agents working in this repository. It
> describes **the code that is present**; measurements are kept only where they
> justify a non-obvious choice. Investigation logs live in `docs/` (§17).
>
> If this document conflicts with the code, the code wins. Section numbers are
> cited from source comments ("AGENTS.md 4.6"), so keep them stable.

## 1. What this project is

`dcload-ip` is a **Sega Dreamcast network loader**. Two halves talk over UDP:

1. A **Dreamcast-side** program (`dcload`, at `0x8c004000` by default, copied
   there by the 1st_read bootstrap — §5). It implements ARP, ICMP, UDP and an
   optional DHCP client on the BBA (`HIT-0400`) or LAN Adapter (`HIT-0300`),
   and serves commands to upload (`LBIN`/`PBIN`/`DBIN`), execute (`EXEC`),
   read memory (`SBIN`/`SBIQ`), proxy GDB, pass Maple packets (`MAPL`) and
   drive the SH4 performance counters (`PMCR`). For a launched title it also
   **emulates the GD-ROM drive** (§4.5), including **CD-DA playback** (§4.13),
   serving sectors from a disc image on the PC.
2. A **PC-side** program. `dc-tool-ip` (C, in this repo) is the reference
   tool. The GD-ROM and CD-DA emulation is served by a separate Rust host,
   `dcload-ip-rs` (§16).

Fork of KallistiOS' `dcload-ip`, overhauled by Moopthehedgehog, maintained by
Mickaël Cardoso (SiZiOUS) and contributors. License **GPLv2** (`COPYING`).

Version `2.0.4`, set in `Makefile.cfg` **only** (both Makefiles pass it as
`-DDCLOAD_VERSION`). `README.md` still says 2.0.2 and `CHANGES` stops at 2.0.1.

No autotools/CMake/Meson: hand-written GNU Makefiles including `Makefile.cfg`
and `Makefile.hostdetect` from the repo root.

## 2. Repository layout

```
.
├── Makefile                # top-level driver: include Makefile.cfg + recurse
├── Makefile.cfg            # toolchain / IP / tunables / VERSION — the file users edit
├── Makefile.hostdetect     # sets MINGW, MINGW32, MINGW64, CYGWIN, MACOS, BSD, WINDOWS
├── AGENTS.md               # this file (CLAUDE.md is a symlink to it)
├── README.md, CHANGES, NETWORK, COPYING
├── Cdif131e.pdf / .txt     # SPI / GD-ROM command spec — reference, do not edit
├── .gdbinit                # arch sh4, endian little, target :3263
├── .gitlab-ci.yml          # primary CI (two gcc jobs)
├── .github/workflows/      # sync-to-gitlab.yml — mirrors master → GitLab
├── .vscode/                # sh-elf-gdb attach config to :3263
├── host-src/tool/          # dc-tool-ip
├── target-src/dcload/      # the DC loader → dcload.bin + exception.bin
│   └── loaders/            # `make loaders` output (§4.11)
├── target-src/1st_read/    # CD bootstrap → scrambled 1st_read.bin
├── target-inc/             # header-only include path (no Makefile)
├── example-src/            # 3 demo programs using dcload syscalls
├── make-cd/, make-cdi/     # burn a CD-R / build a .cdi (not in the root make)
├── docs/                   # §17
└── scripts/                # debugging instruments, §11
```

Surprises:

- `host-src/machine/` and `host-src/sys/` do **not** exist.
- `target-inc/` is an include path (`-I../../target-inc`, `-I../target-inc`).
- Shipped DC artifacts: `target-src/dcload/dcload.bin`, `exception.bin`,
  `target-src/1st_read/1st_read.bin`. ELFs and `dc-tool-ip` are gitignored.
- `*.asm` next to `.o` files are **build output** listings (§6).
- `scif.c`/`scif.h` (serial console) and `bswap.s` are **not built**.

## 3. Build system

### 3.1 Toolchain

- A **KallistiOS SH-ELF toolchain**; the prefix comes from `$(KOS_CC_BASE)`.
  `source /opt/toolchains/dc/kos/environ.sh` before `make`, or every cross
  target fails with "command not found".
- `target-src/1st_read/Makefile` runs `$(KOS_BASE)/utils/scramble/scramble`;
  build it once in `$KOS_PATH/utils/scramble` if KOS was built from source.
- A non-KOS sh4-elf toolchain works with `USING_KOS_GCC` commented out in
  `Makefile.cfg` (the alternate block is hard-coded under `/mnt/c/DreamcastKOS/`).
- The host tool is plain `gcc`. The tree builds under **GCC 15**.

### 3.2 Host detection (`Makefile.hostdetect`)

Sets `BSD`, `MACOS`, and `MINGW`/`MINGW32`/`MINGW64`/`CYGWIN`/`WINDOWS` from
`uname -s` (`MINGW64` via MSYS2's `$MSYSTEM_CHOST`). `include` it; do not
duplicate the logic.

### 3.3 Root Makefile

`SUBDIRS = host-src target-src example-src`, `target-src: host-src`, `install:`
recurses into `host-src/tool`. `1st_read` `.incbin`s `dcload.bin` and
`exception.bin`, so `dcload` must build first.

### 3.4 Common commands

```sh
source /opt/toolchains/dc/kos/environ.sh
make                          # host tool + dcload + 1st_read + examples
sudo make install             # dc-tool-ip → $(TOOLINSTALLDIR)
make -C target-src/dcload     # incremental sub-builds (also host-src/tool, target-src/1st_read)
make -C target-src/dcload loaders          # the relocatable loader for the Rust host (§4.11)
make -C target-src/dcload config DCLOAD_BASE=…   # print the resulting layout
make clean                    # objects, maps, *.asm
make distclean                # also *.bin and loaders/
```

### 3.5 `Makefile.cfg`

- `USING_KOS_GCC = 1`; `HOSTCC`/`HOSTCFLAGS` (`gcc -Og`, `.exe` and `-D_WIN32`
  on Windows); `TARGETCFLAGS = -Os -ml -m4-single-only`;
  `TOOLINSTALLDIR = /opt/toolchains/dc/bin`.
- `WITH_BFD` — `0` links dc-tool against `libelf` (default), `1` against sh-elf
  `libbfd`+`libiberty` (forced under MinGW). `BFDLIB`/`BFDINCLUDE`/`ELFLIB`/
  `ELFINCLUDE` must point at the **sh-elf** headers.
- `TARGETCCVER` — clamped to 4, so examples use `dc4.x`.
- `VERSION = 2.0.4`. `STANDALONE_BINARY` (Windows only, `-static`).
- `SAVE_MY_FANS = 0|1` — slows dc-tool's poll loop to save laptop CPU.
- `EXCEPTION_SECONDS = 15` — on-screen register dump duration.
- `DREAMCAST_BBA_RX_FIFO_DELAY_{COUNT,TIME}` and the `…_LAN_…` pair — burst
  pacing for dc-tool → dcload. Raise `TIME` ~100 µs at a time on `link change…`
  (BBA) or LAN adapter hangs; presets are in the file's comments.
- `DREAMCAST_IP = 0.0.0.0` — `0.x.x.x` selects DHCP. A static address is the
  other mode; `scripts/flycast-debug-loop.sh` builds with
  `DREAMCAST_IP=192.168.1.130`. A static address needs `announce_presence()`
  (§4.7). **`make clean` after changing it** (§14.6);
  `target-src/dcload/.built-ip` records it, but only the debug-loop script
  writes that stamp. **A loader set must be built with the same IP as the CD
  image**: a chainloaded loader inherits the IP only through the BBA's SRAM
  (§4.9), which flycast does not have.

## 4. The Dreamcast binary (`target-src/dcload`)

### 4.1 Artifacts

- `dcload` — ELF linked with `dcload.x`; `dcload.bin` is ~32 KB with defaults.
- `exception` — `exception.S` alone, `-Ttext=0x8c00f400`; `exception.bin` is
  2048 bytes. Both `.bin`s are `.incbin`'d by `target-src/1st_read/loader.s`.
- `dcload.map` / `exception.map` — read them; `_end` matters (§4.6).

### 4.2 Sources

Compiler flags disable everything that could move code behind your back:
`-Wall -Wextra -ffreestanding -std=gnu11 -fno-zero-initialized-in-bss
-fno-common -fomit-frame-pointer -fno-strict-aliasing -fno-unwind-tables
-fno-asynchronous-unwind-tables -fno-exceptions -fno-delete-null-pointer-checks
-fno-stack-protector -fno-stack-check -fno-merge-constants
-fno-merge-all-constants`. Link: `-Wl,--warn-common -Wl,--no-undefined
-Wl,-Tdcload.x -nostartfiles -nostdlib -static -Wl,-z,now … -lgcc`.

| File | Role |
| --- | --- |
| `dcload-crt0.s` | stack, zero BSS and `.hiram`, call `main`; the fixed jump table and the `0xdeadbeef` magic the example programs check. |
| `dcload.c` | `main`: adapter detection, DHCP retry, video, the command loop; `setup_machine()` (only with `ISOLDR_SETUP_MACHINE=1`), `zero_game_ram()`, guest tick. |
| `go.S` / `go.h` | the handoff to a launched program — SR, CCR, entry (§4.7). Read its header first. |
| `disable.s` | turns off the SH4 cache (must run from P2). |
| `startup_support.c` | BSS helpers, C++ constructors, video mode; `STARTUP_Get_Cable()` reads the cable off PDTRA (0 VGA, 2 RGB, 3 composite) for video setup **and** the VERS reply. |
| `video.s` / `video.h` | on-screen text (uses the BIOS font through `0x8c0000b4`). |
| `packet.c/.h`, `bswap.h` | packet builder/parser, byte order. |
| `net.c/.h` | ARP/ICMP/UDP glue, `announce_presence()`. |
| `adapter.c/.h` | adapter interface (`bb`), the TMU2 fine deadline. |
| `hiram.h` | `HIRAM_BUF`: put a large buffer in `.hiram` instead of BSS. |
| `rtl8139.c/.h` | BBA driver, RX ring (§4.8), warm start (§4.9). |
| `lan_adapter.c/.h` | LAN Adapter driver. |
| `dhcp.c/.h` | DHCP client (§4.10). |
| `perfctr.c/.h` | SH4 performance counters; counter 1 times the DHCP lease and adapter timeouts. |
| `memfuncs.c/.h`, `memcpy.S`, `memcmp.c` | aligned mem* fast paths. |
| `maple.c/.h` | Maple bus driver; its DMA buffer is outside the image (§4.4). |
| `cdda.c/.h` | CD-DA playback (§4.13). The header of `cdda.c` is the design description. |
| `g2dma.c/.h` | the four G2 DMA channels by polling, `g2dma_quiesce()` (§4.16). |
| `g2bench.c` | `G2DMA_BENCH=1` only: what G2 DMA does on a console, with a torture round (§4.16). |
| `cdfs.h`, `cdfs_redir.s`, `cdfs_syscalls.c` | GD-ROM emulation (§4.5); also the boot-time drive spin-down (§4.14). Both carry explanatory headers. |
| `syscalls.c/.h` | host syscalls `DC00`–`DC24` (§8). |
| `commands.c/.h` | the command dispatcher and the LoadBinary window (`bin_info`). |
| `exception.S` | exception display **and** the VBR table handed to the title; `+0x600` is `nop; rte; nop`. |
| `irq.c/.h`, `irq_hook.S` | the interrupt hook in a Windows CE title's own vector table (§4.15). |

### 4.3 Build flags

All in `target-src/dcload/Makefile`, which triggers a rebuild when edited.
GD constants are in §4.5, CD-DA flags in §4.13.

**Size knobs.** Footprint is correctness (§4.6). Measured 2026-09-12 as the
change in `_end` from the defaults of then (`_end = 0x8c00c58c`; since the CD-DA
simplification of 2026-09-19, the paced fill of 09-20 and the Maple copy fix
of the same day, the unacknowledged sector path of 09-20 and the drive
spin-down of 09-20 the defaults gave `0x8c00bfc8`; the third GD door and the
symbol-derived read block of 09-26 (§4.5) bring them to `0x8c00c000`):

| Flag | Default | Effect | `_end` saved |
| --- | --- | --- | --- |
| `PKT_BUFS_IN_HIRAM` | `1` | packet buffers and the CD-DA staging buffer in `.hiram` instead of BSS | 5440 B |
| `WITH_CDDA` | `1` | CD-DA engine (§4.13) | 5152 B if 0 (2026-09-20) |
| `WITH_LAN_ADAPTER` | `1` | HIT-0300 driver; 0 = BBA only | 2040 B if 0 |
| `WITH_PMCR_CMD` | `0` | serve `PMCR` (perfctr.c stays); off since 2026-09-28 -- no host sends it | 808 B if 0 |
| `WITH_MAPLE` | `1` | serve `MAPL` | 656 B if 0 |
| `WITH_GD_SPINDOWN` | `0` | stop the real drive at boot (§4.14); off since 2026-09-28 to pay for the Katana hook | 160 B if 0 |
| `WITH_IRQ_HOOK` | `1` | the interrupt hook (§4.15) | 792 B if 0 (2026-09-27, phase 1) |
| `GD_ASYNC_KATANA` | `1` | the interrupt hook in Katana titles too, and their disc reads through the asynchronous engine (§4.5); needs `WITH_IRQ_HOOK` | ~150 B |
| `WITH_MARK_CMD` | `1` HIGH, `0` LOW | serve `MARK` (§8): witness words the host uses to learn what a title writes | 432 B |
| `IRQ_IDLE_LISTEN` | `1` HIGH, `0` LOW | the hook's tick listens to the network (≤ 1 ms every 50 ms) when nothing else owns it, so `--diag`, the stack watch and `MARK` are answered while a title does not read its disc | ~220 B |
| `RTL_RX_DMA` | `1` HIGH, `0` LOW | the BBA's RX frames by G2 DMA from the tick (§4.16); 0 = every frame by CPU | ~150 B |
| `GA_KATANA_READS` | `1` | with `GD_ASYNC_KATANA`, a Katana title's disc reads go through the asynchronous engine; 0 keeps the hook (CD-DA from the tick, idle listen) with synchronous reads | -- |
| `DCLOAD_GC_SECTIONS` | `1` | `--gc-sections`; also reveals dead code | 328 B |
| `DCLOAD_LTO` | `0` | `-flto`. **Off on purpose**: it changes the depth of the C frame the GD coroutine parks (§4.5 — check `g_gd_park_longs` on hardware) and suppresses the `*.asm` listings | 2428 B if 1 |

Combinations, re-measured 2026-09-20: `WITH_LAN_ADAPTER=0 WITH_MAPLE=0
WITH_PMCR_CMD=0` gives `0x8c00b200`; adding `DCLOAD_LTO=1` gives `0x8c00a8c0`.

**Diagnostic and experimental flags** (all default 0 unless stated):

| Flag | What it does |
| --- | --- |
| `GD_TRACE` | Trace GD requests to the host console. Each event is a UDP round trip, which perturbs timing enough to stop a title booting. |
| `GD_SERVICE_EVERY_SYSCALL` | Poll the network at the top of ReqCmd/GetCmdStat/GetDrvStat. **Sonic Adventure dies with it** (isolated 2026-08-29, mechanism unknown). |
| `GUEST_TICK` (+`_PERIOD`, `_WATCHDOG`, `_BLOCK`) | TMU0 sampler of the guest PC, block at `0x8cff8000` (survives a reset + CD reboot). |
| `ISOLDR_HANDOFF` = `ISOLDR_SR` + `ISOLDR_REGS` + `ISOLDR_SETUP_MACHINE` | isoldr's machine state at launch. Measured 2026-08-16: **`ISOLDR_REGS` breaks Sonic Adventure**; the other two were never judged alone. |
| `GUEST_CACHES_ON` | Hand the title CCR `0x0909` instead of `0x0808`. |
| `GUEST_IRQ_MASKED` | Hand SR with IMASK=15. Diagnostic only (go.S). |
| `DCLOAD_CLEAR_IPBIN`, `DCLOAD_ZERO_GAME_RAM` | Clear the IP.BIN region / the title's RAM at start-up, as a real boot would. Faithfulness only (`CLEAR_IPBIN` changed nothing on SA2). |
| `G2DMA_BENCH` | At boot, measure which G2 DMA channels reach the BBA's SRAM and the AICA, and run a torture round (§4.16); results in `g_bench[]` and on screen. Forces `GD_STAGE_BIG_SECTORS=2`. Costs ~1.2 KB: never in a shipped set. |
| `DCLOAD_EMIT_RELOCS` | `ld -q`: keep relocations (used by `make loaders`). Loaded bytes unchanged. |

In `rtl8139.c`: `RTL_WARM_START` (1) — adopt a BBA a previous dcload brought
up (§4.9); ~512 B.

`DREAMCAST_IP`, `EXCEPTION_SECONDS` and `VERSION` come from `Makefile.cfg`.

### 4.4 Memory map

`dcload.x` is **generated** from `dcload.x.in` by the C preprocessor:
`ram : ORIGIN = DCLOAD_BASE, LENGTH = DCLOAD_STACK - DCLOAD_BASE`. The base is
a build variable because it is per-game (§4.11). Defaults (LOW layout):

| Address | What |
| --- | --- |
| `0x8c004000` | dcload's base. `+4` = `0xdeadbeef` magic, `+8` = syscall trampoline pointer (the example-program ABI). |
| `0x8c00ce78` | `_end` with default flags (2026-09-28, with the interrupt hook and asynchronous reads; over the `0x8c00c000` bound, §4.6). Code and BSS are all below it. |
| `_end`..`_stack` | the loader's stack; also `.gdstage` (NOLOAD, 10 KB), the big GD stage, used only under a CE title (§4.15). |
| `0x8c00f400` | `_stack` (LOW layout), **the VBR handed to the title**, the link address of `exception`, and the BIOS VBR. Only `_stack` moves with the base. |
| `0x8c010000` | the title's load address; `exception.bin` ends just before it. |
| `0x8cfe8000` | Maple DMA buffer (2 KB), outside the image (`_maple_dma_buffer`). |
| `0x8cfe9000` | `.hiram`, 12 KB reserved (NOLOAD, zeroed by crt0): packet buffers and CD-DA staging buffer. `dcload.x` asserts it does not reach the Maple buffer. |

Link-time asserts: `(_stack - _end) > 800` and `_end` within `ram`. There is
no resident/transient split any more.

Why this base: it is DreamShell's most common preset (§4.11), the guest VBR
`0x8c00f400` is the BIOS VBR, and the example ABI expects it.

dcload itself does not install a VBR: the only `ldc …,vbr` is in `go.S`, for
the launched program. dcload runs on whatever VBR the bootstrap left.

### 4.5 GD-ROM emulation

`cdfs_redir.s` + `cdfs_syscalls.c` emulate the BIOS GD driver, on isoldr's
model. The driver a title is written against is a **coroutine**, so:

- `gdGdcReqCmd` queues, answers `PROCESSING`, returns a channel. No I/O.
- `gdcServerMain` is an endless dispatch loop. `gdGdcInitSystem` or the first
  `gdGdcExecServer` enters it; `gdGdcExecServer` resumes it and
  `gdcExitToGame()` parks it. One hardware stack: the inactive side's frame is
  copied into `saved_regs[]` (96 longs, 11 overhead). `g_gd_park_longs`
  publishes the depth — near 80 means enlarge the buffer.
- `data_transfer_emu_async` reads in `GD_EMU_ASYNC`-sector chunks, one host
  round trip each, **without yielding to the title between chunks** (it yields
  only before retrying a failed chunk). Between chunks it feeds CD-DA
  (`GD_CDDA_BETWEEN_CHUNKS`).
- **A chunk is one round trip, and the loader judges it** (2026-09-20). The
  host sends the LoadBinary, the parts and the ReturnValue without pausing for
  the echo or for a DoneBinary probe (`send_sectors`, §16). `ReadSectors`
  therefore calls `bin_window_close()` **before** building the request — so
  completion is judged on this chunk's own window and not on the previous,
  already complete one — and tests `bin_window_complete()` when the
  ReturnValue releases it. A hole fails the chunk (`g_cdfs_read_holes`) and
  `data_transfer_emu_async` asks again: one extra round trip on a loss instead
  of two on every chunk. **A short chunk must never be reported COMPLETED**;
  the title would run the bytes. **But a ReturnValue over a hole is not
  necessarily ours** (2026-09-27): a disc read's carries nothing, so the late
  answer to an earlier attempt, queued in the ring, can end the wait of the
  next one. `ReadSectors` then keeps waiting until its own deadline
  (`g_cdfs_read_stale`) instead of failing in a millisecond; the late answer's
  parts are the same bytes for the same place, so they count. **A late answer
  for another destination is refused at the door** (2026-10-02,
  `g_bin_read_want`, `g_gd_stale_lbin`): its LoadBinary used to replace the
  waiting read's window, fill it and pass `bin_window_complete()`, so the read
  was COMPLETED with nothing delivered -- the GTA III port (KOS) then ran on a
  cache block holding a sector read 16 reads earlier and asserted in
  `LoadCollisionModel`. `ReadSectors` and `ga_next` publish their destination
  for the wait; `cmd_loadbin` refuses any other one without touching the
  window, as the CD-DA door does. It also sets `bin_echo_suppress(1)` for the
  wait: nobody reads the echo any more, and sending it would put one of our
  frames on the wire inside the host's burst — the collision already on record
  on the audio path.
- **Asynchronous reads under Katana titles** (`GD_ASYNC_KATANA`, 2026-09-28,
  BBA only; `docs/wince-investigation.md` 9l-9n). The interrupt hook goes into
  Katana titles' vector table too (their entry is six nops, §4.15), and a read
  of more than one sector goes through the engine written for Windows CE: the
  read is posted at once, the title runs, and **the hook's tick collects each
  chunk (4 sectors, ~9 KB of the 16 KB ring) and posts the next as soon as it
  is back**, between frames. The MMU is off, so the destination is physical
  and the host writes it directly, 6 sectors a chunk (`GA_PHYS_SECTORS`: what
  the ring holds undrained). **The BBA's own RX interrupt wakes the tick**
  (§4.15), so a chunk is collected when it lands rather than at the title's
  next interrupt. Two versions without the hook failed on the
  same console (9l, 9m): one chunk per ExecServer (once a frame) loaded menus
  at ~350 KB/s, and a spin budget for reads of 32+ sectors brought loads to
  0.5-1 MB/s but stalled play on Crazy Taxi's 73-sector music reads -- no size
  tells a loading screen from a stream. This **yields between chunks**, which
  `GD_YIELD_BETWEEN_CHUNKS` measured fatal to Sonic Adventure in 2026-08 --
  while SA's stack still ran through the loader's image (§4.6), the cause of
  every other SA death of that week; re-measure SA before trusting either
  reading. `GD_ASYNC_KATANA=0` leaves Katana titles unhooked and synchronous.
- `gdGdcGetCmdStat` reports progress (`COMPLETED` consumed once, then `IDLE`;
  `req_count` never 0 or 1). `gdGdcGetDrvStat` reports PLAYING while a read or
  CD-DA is live and calls `cdda_service()` **before** taking the GD lock.
- `CMD_REQ_STAT` and `CMD_GETSCD` report the CD-DA position while music plays.
  Unmodelled commands are force-completed, never failed (a title that gets
  FAILED for a routine command tends to give up).

**Three doors, not one** (2026-09-26). The BIOS GD driver is reachable three
ways, and a title served from the host must not get through any of them to the
real drive:

| Door | Who uses it | How it is held |
| --- | --- | --- |
| vector `0xac0000bc` (misc group in front, `r6 == -1`) | every Katana title measured, 13 literals each | `cdfs_redir_enable()` |
| vector `0xac0000c0` (the driver alone) | Windows CE's `COREDLL.DLL` | `cdfs_redir_enable()`, since 09-26; saved and restored with `0xbc` |
| the driver body `0x8c0010f0` itself | Windows CE's GD driver: 23 literals in Sega Rally 2's `0WINCEOS.BIN`, called with `r6 = 0`, `r7` = index | **the host** rewrites the literals to `_gd_bios_entry` (`dispatch::gd_body_patches`), as isoldr's `gdc_syscall_patch()` does |

The third is patched from the host, not by overwriting the BIOS's copy of the
driver here, because `gd_spin_down_drive()` needs the real driver at the next
boot and `cdfs_saved` survives only as long as the image. None of the four
Katana test titles carries `0x8c0010f0` or `0x8c0000c0`, so neither change
reaches them.

**What a disc read may not land on** is derived from the linker (09-26):
`[dcload_base, end)` and `[_hiram_start, _hiram_end)`, compared in P1. It used
to be the constants `0x0c004000..0x0c010000` — the stock base's image and
stack — which protected nothing of a relocated loader and refused reads into
IP.BIN, the guest VBR and a low title's own stack (§14.11). The Maple DMA buffer
is deliberately not in the list: it is written only while a `MAPL` command runs,
never under a title.

**Invariants, each paid for:**

1. **Never transmit between building a command in `pkt_buf` and sending it.**
   Commands dispatched from inside `bb->loop()` are past that point, so
   answering `SBIQ` during a wait is fine. Corollary: emit any trace *before*
   building the command, or the trace's `write()` overwrites it.
2. **Never start a second transfer while one waits.** `bin_info` and `pkt_buf`
   are single. `g_gd_in_transfer` is non-zero across the two GD waits
   (`ReadSectors`, `GetTOC`) and `cdda_service()` declines while it is set.
   Without it, a title's interrupt handler calling `GetDrvStat` started an
   audio fetch inside a disc read's wait: the fetch replaced the read's window
   and cleared its deadline, and the title froze forever with the loader still
   answering (Snow Surfers, 2026-09-11/12; signature
   `g_gd_idx_counts[ExecServer]` advancing one-for-one with
   `g_cdfs_sync_reentered`, or a host retrying a LoadBinary that is never
   echoed).
3. **No function live across a yield may take the address of a local**: a
   parked frame is restored onto whatever `r15` the next ExecServer has.
4. **Every wait needs a millisecond deadline on TMU2** (`fine_deadline_*`,
   `GD_READ_DEADLINE_TICKS` 250 ms for disc reads). The seconds timeout counts
   whole seconds on the PMCR (2 s fires at 3 s on hardware; never under a
   flycast without the local PMCR patch), `RTL_IDLE_POLL_LIMIT` only counts
   polls with no frame, and both are disarmed when `timeout_loop` is cleared
   under a wait. Arm, call, then clear — never clear someone else's.
   **Measure with `tmu2_since()`** (`adapter.c`), never `start - TMU2_COUNT`:
   a KOS title owns TMU2 (Pck/4, reloaded every second by `timer_ms_enable()`),
   and the raw subtraction fired every deadline in progress at each reload
   (2026-10-02). `gd_deadline_timer_start()` leaves any Pck/4 timer with a
   period of 0.96 s or more alone, so it no longer reprograms KOS's clock.
   Intervals measured on TMU2 must stay under that period (838 ms at most
   today); `cdda.c`'s service gap and the hook's marks still subtract raw.

**Stuck-lock watchdog.** `gd_lock_watchdog()` (from GetDrvStat/GetCmdStat)
releases the GD lock if it is held while the server is parked and
`g_gd_lock_gen` has not moved for 250 ms — a state no code path produces. It
has never fired (`g_gd_lock_stuck` 0); the freezes that motivated it were the
nesting in invariant 2. `g_gd_lock_owner`/`_stuck_owner` say who held it.

**Constants** (`cdfs_syscalls.c`, override with `-D`):

| Constant | Value | Note |
| --- | --- | --- |
| `GD_EMU_ASYNC` | `8` | 16 KB per host request = the BBA RX ring; DreamShell's SA preset. |
| `GD_BULK_SECTORS` | `0` | isoldr reads ≥100-sector requests in one shot; **do not**: into an 11-frame RX ring that lost 840 packets in one run. |
| `GD_YIELD_BETWEEN_CHUNKS` | `0` | With the yield, Sonic Adventure died on delivery of chunk 1. |
| `GD_CDDA_BETWEEN_CHUNKS` | `1` | Feed CD-DA between chunks. Must stay on with invariant 2, or the ring runs dry during level loads (measured `g_cdda_room_min` 0). |
| `GD_READ_DEADLINE_TICKS` | `3125000` | 250 ms at Pck/4. **The primary recovery**, not a backstop: since `send_sectors` the host waits for nothing, so nothing else notices an answer that never arrives. Was 1.2 s, sized to outwait a host that gave up at ~0.7 s — a premise that no longer exists. |
| `GD_READ_RETRIES` | `4` | re-requests before failing a read. |
| `GD_READ_RETRIES_MMU` | `20` | the same with the MMU on (Windows CE, 5 s): CE's driver gives a failed read up for good. |
| `GD_SYSCALL_TIMEOUT_SECONDS` | `6` | coarse seconds backstop (fires at 7 s). |
| `GD_DRAIN_ITERS` | `0` | pre-request RX drain; measured useless at 256 and 50000, kept so nobody re-tests it blind. |
| `GD_SERVICE_ITERS` | `256` | used by `GD_SERVICE_EVERY_SYSCALL` (§4.3). |
| `GD_TRACE_CALLER` / `GD_TRACE_DEST_FROM` | `0` / `0xffffffff` | caller PC/SP tracing; expensive. |
| `GD_LOCK_STUCK_TICKS` | `3125000` | 250 ms, the watchdog threshold. |

**Known gaps:**

- ~~TMU2 is not started at boot~~ — **fixed 2026-09-20, and it had teeth.**
  `gd_deadline_timer_start()` (`cdfs_syscalls.c`, called from `main()`) now
  starts it; `cdda.c` calls the same function instead of keeping its own copy,
  and it stays idempotent because restarting it mid-read would break that
  read's deadline. Before this, only `cdda.c` and `setup_machine()`
  (`ISOLDR_SETUP_MACHINE=1`) started it, so **for a title that streams its
  music as data rather than as CD-DA — Crazy Taxi — neither the read deadline
  nor the lock watchdog could ever expire.** One lost chunk fell through to the
  coarse PMCR backstop and froze the game for **7.0 s** (`GD_SYSCALL_TIMEOUT_
  SECONDS` 6, which fires at 7). It was invisible until the host stopped
  repairing losses on its own: the lesson is that a bound nothing has ever been
  seen to fire may simply be unable to.
- **No `g2_lock()` around CPU reads of the BBA.** `cdda.c` locks G2 for the
  AICA; the BBA ring is polled unlocked. One freeze dump showed AICA reads at
  zero together with `g_rx_hdr_defer` +6147 and resyncs, which fits a G2 burst
  collision. Before adding one, know that on Sonic Adventure under flycast
  (2026-08-09) no title G2 DMA was ever in flight at `rtl_bb_loop()` entry,
  and that a first attempt which *waited* on the DMA-busy bits rebooted the
  machine — observe before acting.
- `CMD_INIT`, `CMD_REQ_MODE` and `CMD_SET_MODE` are force-completed with
  **nothing written back**. No Katana title has minded; an OS driver that reads
  its mode structure back gets stale memory. Count first
  (`g_gd_cmd_counts[24]`, `[30]`, `[31]`) before spending bytes on it.
- **Streams are served since 2026-09-27** (`data_stream()`; before, they were
  force-completed with nothing delivered). PIO pieces are chained through the
  title's `SetPioCallback` callback, called from the server; **DMA pieces
  after the first need the G1 DMA-end interrupt, which nothing raises** -- a
  DMA stream completes only if its first piece is the whole of it. Windows CE
  is steered to PIO by the host (§4.15). No Katana test title has been run
  with this. `GETTOC2` does not model low/high density areas.
- **Reads into a translated address are staged** (`gd_stage_big`, 8 KB in
  `.gdstage` above `_end` -- the loader's own stack region, dead while a title
  runs, and used only under the MMU because in the LOW family it is a Katana
  title's stack; `gd_stage`, 6 KB in `.hiram`, for the TOC and a stream under
  a Katana title; and `gd_is_virtual()`: P0 with MMUCR.AT set): received there and
  copied with `memcpy.S`. The host cannot write such an address: every copy in
  `memfuncs.c` stores in the source's segment (§8). With the MMU off nothing
  changes.

### 4.6 The footprint rule

**A retail title uses the BIOS work area as a stack, and that is where dcload
lives.** Sonic Adventure enters GD syscalls with `SP = 0x8c00b9d0`, growing
down. When its stack reached `bb` (the adapter pointer, last in BSS), dcload's
next `bb->loop()` jumped through garbage to address 0 (flycast logs
`REIOS: Booting up`, §14.14). Fixed then by shrinking the image: the LoadBinary
map (`BIN_INFO_MAP_SIZE` 11656 → 256, host `MAX_XFER` in step, §16), the Maple
DMA buffer and the packet buffers out of BSS, and no 64-bit division (§14.15).
`docs/sonic-adventure-investigation.md` has the full record.

**The hard bound is not the stack's depth, it is `0x8c00c000`.** A Katana
title's crt0 — the code `0x8c010000` jumps to — **fills
`0x8c00c000`–`0x8c00f400` with the word `"SEGA"`** as its very first loop,
before it calls anything. Found 2026-09-17 in the boot binaries of Snow
Surfers, Crazy Taxi, Jet Set Radio, ChuChu Rocket!, Power Stone and Sonic
Adventure — six of six, so treat it as the rule. **A low loader whose `_end` is
above `0x8c00c000` therefore has its `.data` and `.bss` overwritten before the
title's first GD syscall, whatever the margin under its stack.** Measured the
same day under flycast: Snow Surfers at the stock base (`_end = 0x8c00cb48`)
failed Katana's `gdFsInit` and called `syBtExit()` → BIOS misc syscall 1 → back
to the BIOS menu, 180 ms after `EXEC` (flycast logs `SYS_MISC 1`, then
`REIOS: Booting up`); on the two error codes it does not exit for (-12, -14) it
spins forever instead, which is the "frozen on executing" ending. It had passed
the stack test comfortably: `g_gd_sp_min` 0x8c00e460, 6424 B over `_end`.
isoldr does not meet this because its image is 13 KB and ends at `0x8c007400`,
which is why 593 DreamShell presets can say `0x8c004000`.

**Where that stands now (2026-09-27): over the bound.** The defaults gave
`_end = 0x8c00c000` on 09-26, the image's last byte just under the painted
range, with no margin left. The Windows CE work (§4.15: staging, streams,
the exchange mask and stack switch, retries, CD-DA level follow and catch-up)
put the tree at `_end = 0x8c00c434`, and the interrupt hook (§4.15, phase 1)
at `_end = 0x8c00c7ec`, and the asynchronous reads (phase 3) at **`_end =
0x8c00ce78`, 3704 B over** (2026-09-28, with `WITH_GD_SPINDOWN=0` and `WITH_PMCR_CMD=0`). The HIGH family has a bound of its own:
`.gdstage` must end under `_stack` (`base+0xbc00` since 2026-09-28; it was
`+0xb000`, and the 3 KB moved are what paid for §4.16). Phase 3 spent
`GD_STAGE_BIG_SECTORS` 5 → 4 to fit (the stage is now 8 KB), which leaves
`_end` up to `base+0x9c00` -- **~2 KB left** after the G2 DMA work (`_end` =
`base+0x9310`, default flags; before it, 352 B; `base+0x9634` since `MARK` and
the idle listen, 2026-09-30: ~1.4 KB left). 9n spent `WITH_GD_SPINDOWN` on the Katana hook and 9o `WITH_PMCR_CMD` on the BBA RX interrupt, both now 0 by default. **The deployed
`loaders/` is the current build since 2026-10-02** (md5 `4ce716ce…`: the
three Shenmue II fixes, `MARK`, the idle listen, the G2 DMA suspend, the
per-frame RX channel of §4.16, the P2 purge of §8, `g_gd_kos` and the
disc-read door of §4.5, KOS's masked waits and `DC25`, `tmu2_since()` and the
G2 suspend under KOS (§4.16): `_end` = `base+0x9b20`, `.gdstage` ending 208 B
under `_stack`; it needs
the host's `layout()` of 2026-09-29; the older `0x8c00c000` build is gone), which
loses the low base to painted titles as described below. The user's decision
(2026-09-21) is that this is acceptable where it has to happen: the host
already refuses a low base whose image reaches a range the title paints
(`low_loader_painted_by_title`), so a taller build costs the low family for
Katana titles, not correctness -- but say so when it happens, and
`WITH_GD_SPINDOWN=0` (160 B) is the first flag to spend. It is still above Sonic Adventure's
SP (`0x8c00b9d0`). The smallest build without LTO (`0x8c00b200`) leaves 2000 B
under SA's stack and the smallest with it (`0x8c00a8c0`) 4368 B, against the
4096 B the host requires. The host places a title by these rules (§4.11, §16): it refuses a
low base whose image reaches a constant-range fill it finds in the title, or
whose stack margin is short, and a title the preset database does not know is
searched from `0x8ce00000`. Measured 2026-08-29: at a margin of 2444 B the
title corrupted the loader.

Rules when adding anything DC-side:

- Check `_end` in `dcload.map` after any change that adds state. Under
  `0x8c00c000` is the only footprint a retail title leaves alone at a low base.
- Put large buffers in `.hiram` (`HIRAM_BUF`), not BSS.
- `bb` is near the end of BSS — one of the first things a descending stack
  reaches. Nobody has ordered BSS deliberately yet.
- **The guard is `g_gd_sp_min`** (lowest SP any GD syscall was entered with)
  and `g_gd_sp_in_image` (entries already inside `[_dcload_base, _end)`),
  latched by `gd_note_caller()` at no network cost. `g_gd_sp_min - _end` is the
  margin, readable while the title is still healthy. A `ReadSector` whose size
  is not a sector multiple is a request built from overwritten state.

### 4.7 Handoff to a game, and reachability

- **`go.S` hands `SR = 0x60000101`** (MD=1, RB=1, BL=0, IMASK=0): the title can
  take interrupts and exceptions immediately. The old `0x500000f0` (BL=1,
  IMASK=15) blocked both. isoldr actually hands `0x700000f0` (`ISOLDR_SR`).
  RB=1 switches register banks, so the entry address is moved out of `r4`
  first.
- **Caches off at handoff** (`CCR = 0x0808`), written from P2 with the settling
  window. A deliberate divergence from isoldr (`0x0909`): dcload fills the
  title's buffers with CPU stores that the title reads back.
- The VBR and SP handed over are `0x8c00f400` whatever the loader base.
- **`announce_presence()` (net.c)** sends a gratuitous ARP from the main loop.
  Without it a static `DREAMCAST_IP` is unreachable: the host must ARP, flycast's
  BBA bridge opens its capture only after the guest transmits, and Windows
  silently discards datagrams to an `Unreachable` neighbour. DHCP hid this.

### 4.8 RX ring rules (`rtl8139.c`)

Each rule was a real defect in the stock tree:

1. **Program `RT_INTRMASK`**, or the RX status bits are never observable.
2. **Keep an ungated `RxBufEmpty` fallback**, or frames that never re-assert
   RxOK stay invisible.
3. **Never publish `CAPR` out of range on wrap.** `0x7ff0` tells the chip the
   ring is drained, and it discards the queue.
4. **Check header plausibility.** The status word is written last; an early
   read gives the previous occupant's length and desynchronises the ring.
5. **Overflow is back-pressure, not a fault.** Drain first; re-initialise only
   if the ring refuses to empty.

And **every hardware wait is bounded** (`RTL_LINK_SPIN_LIMIT` on the PHY
waits): an unbounded link-change spin once dropped the poll loop from ~45000
iterations per 0.3 s to one.

**Read the GAPS DMA window (`0x81848000`) 32 bits at a time only**
(`memcpy_32bit`). `SH4_aligned_memcpy` reads it with `fmov.d` (64-bit accesses):
the data comes back correct and the SH4 freezes seconds later.

### 4.9 Warm start — chainloading dcload from dcload

A cold `rtl_bb_init()` powers GAPS down and up, clears the 32 KB SRAM, resets
the chip and restarts auto-negotiation, which takes seconds and can loop on
link-change events. So:

- **`rtl_warm_usable()`** runs in `rtl_bb_detect()` **before** the "GAPS off"
  write and checks that the chip is configured exactly as dcload leaves it
  (GAPS, `RXBUF`, `TXADDR`s, `RXCONFIG` minus accept bits, `TXCONFIG`,
  `INTRMASK`, `CHIPCMD`, link + auto-negotiation). A reset or powered-on chip
  cannot pass.
- **`rtl_warm_adopt()`** re-reads the MAC, jumps `CAPR` to `CBR` to **discard
  the backlog** (the likely frame there is a retransmitted `EXEC`, which would
  reload forever), resets `cur_tx`, clears stale status, re-enables RX.
- `rtl_bb_loop()` must not clear `rtl_link_up` on a warm start.
- **The IP travels in the adapter's SRAM**, at GAPS offset `0x5000` (magic + IP
  + complement), written by `rtl_handoff_save()` just before `go()` and read
  into `g_warm_ip`. RAM cannot carry it (1st_read zero-fills, crt0 zeroes BSS).
  The DHCP lease is not carried. The screen shows `(Warm Start)`.
- BBA only; the LAN Adapter always takes the cold path.

Commands are matched on the MAC and `our_ip` is taken from the packet
(`cmd_loadbin()`), which is why the host can reach a DC without a lease.

### 4.10 DHCP: a reply is not ours just because it is a reply

Only bites on a real LAN:

1. **Ports** (`process_udp()`): the DHCP parser runs only for 67 → 68.
2. **xid and chaddr** (`handle_dhcp_reply()`): the xid we sent is kept in
   `dhcp_my_xid`. Before, another machine's broadcast OFFER was adopted, and
   our own ACK then failed the xid test until the retry counter expired.

A rejected reply returns -1 so the wait continues. `g_dhcp_replies` /
`g_dhcp_not_ours` show whether this happens (readable with `SBIN` during the
DHCP wait). A NAK still restarts `dhcp_go()` recursively, bounded by
`DHCP_NAK_NEST_MAX = 5`.

### 4.11 The base is per-game, and the host moves the loader

DreamShell's presets give a per-title `memory` address:

| address | presets | |
| --- | --- | --- |
| `0x8c004000` | 593 | stock base; what homebrew needs |
| `0x8c001100` | 196 | `_MIN_GINSU` — **not supported** |
| `0x8cfe8000` | 119 | `_HIGH` (Sonic Adventure 2) |
| `0x8c000100` | 80 | `_MIN` — **not supported** |
| `0x8ce00000` | 16 | `ISOLDR_DEFAULT_ADDR` |

**Bases below `0x8c004000` are unsupported**: the image would overwrite the
BIOS syscall area, and `video.s` jumps through the font pointer at
`0x8c0000b4` on every string drawn, including during the upload that destroys
it. Supporting them means dropping the on-screen display for those bases. The
host places such titles elsewhere (§16).

`DCLOAD_BASE` is a build variable; the host picks a base and chainloads a
loader there before uploading the title. Things this must not break:

1. **`0x8c004000` stays the default and what the CD boots** (magic and
   trampoline addresses are literals in every KOS program).
2. **The state handed to the title does not follow the loader**: VBR/SP
   `0x8c00f400` for every base, as isoldr does. Each loader ELF carries
   `exception.bin` as a `.guestvbr` section at that address.
3. **Two layout families.** LOW (base < `0x8c010000`): stock layout, only
   ORIGIN moves. HIGH: relative to the base — stack top `+0xbc00` (`+0xb000`
   before 2026-09-28; the host's `loaders::layout()` mirrors it, and it stays
   1 KB under `.hiram` so that no symbol value is both, the relocator
   classifies words by value), `.hiram` `+0xc000` (12 KB), Maple DMA `+0xf000`,
   span `0x10000`.
4. **A low loader's buffers are at `0x8cfe8000`/`0x8cfe9000`**, so chainloading
   directly to a `0x8cfe8000` base writes over the running loader's packet
   buffers: the transfer "succeeds" and the new loader is deaf. The host always
   hops through `0x8ce00000` (§14.17).
5. **A preset's address was chosen for isoldr (13 KB); we reserve `0x10000`.**
   Sonic Adventure 2's preset `0x8cfe8000` puts the loader's stack where the
   title's Maple DMA list lives (`0x0cff0000`, written by hardware): black
   screen, nothing logged. `0x8cef8000` works. The host now scans the title for
   such constants before choosing (§16).

**The loader set is one relocatable image.** `make loaders` links
`loaders/dcload-relocatable.elf` at `0x8ce00000` with `DCLOAD_EMIT_RELOCS=1`
(`LOADER_BASES` is empty; list bases there to get pre-linked ELFs back). It
does not deploy: copy the ELF to the host's `loaders/` and prove the copy
(§14.19). **One set is kept there**; an A/B is one build away into a directory
of its own, passed with `--loader-dir` -- flycast needs
`DREAMCAST_IP=192.168.1.130 CDDA_TICKS_X8192=580480` (§3.5, §4.13), and the
fourteen sets accumulated during the CD-DA investigation were deleted on
2026-09-20. The host relocates the image to any base in either family:

- The image only has `R_SH_DIR32` relocations. Each is classified by **the value
  of the symbol it names** (image, `_stack`, `.hiram`, `_maple_dma_buffer`) and
  gets that region's delta — four deltas, equal inside one family. Classify by
  symbol value, not section (`_dcload_base` is filed in `.hiram` by `ld`) and
  add the delta to the word (P2 aliases like `0xace00000` keep their bits).
- **`.guestvbr` has no relocations**: `exception.S` names the jump table as
  `DCLOAD_BASE + …` literals (six words). The host patches them by content and
  re-scans; any other reference to the loader there fails the relocation.
- Verified: relocation reproduces a native link byte for byte at
  `0x8c004000`, `0x8ce00000`, `0x8cef8000`, `0x8cfe8000`.
- **Every address C code uses must be a linker symbol, never a `-D` number** —
  a `-D` folds into a literal pool with no relocation. Only `exception.S` still
  uses `DCLOAD_BASE` as a `-D`.
- A LOW target's image must fit under `0x8c00f400` minus 800 B, which rules out
  bases much above `0x8c008000`. **Since 2026-09-30 the set does not fit even at
  `0x8c004000`** (image and `.gdstage` 576 B past the VBR, with `MARK` and the
  idle listen): `LoaderSet::can_provide` relocates for real to answer a low
  base, and the host leaves the low family when it cannot. Getting it back
  costs `GD_STAGE_BIG_SECTORS` 3 (Windows CE's staged reads, 4 sectors a chunk
  today) or one of the two flags.

`DCLOAD_BASE`/`DCLOAD_STACK`/`DCLOAD_HIRAM`/`DCLOAD_MAPLE` reach the linker
script, the C sources and `exception.S` from the Makefile only; C code uses
`_dcload_base` rather than restating the base (§14.11).

### 4.12 A title can switch the adapter off

**Sonic Adventure 2 powers the BBA's GAPS bridge down** during `main`, with the
same two writes `rtl_bb_detect()` uses (`0xa1001414 ← 0`,
`0xa1001418 ← 0x5a14a500`). It probes the four G2 slot windows for `"GAPS"`
and parks what it finds. dcload is then deaf: black screen, no packets, no
exception, title running. Not reproducible under flycast.

**The fix is on the host**: `gaps_probe_patches()` changes the `"GAPS"`
comparison constant in the title (found by content: a 4-aligned `"GAPS"` within
4 KB of a slot-window literal), so the probe finds nothing and the title takes
its no-expansion-device path. On by default; `--no-gaps-guard` disables it.

A DC-side recovery does not work: the RTL8139 is powered off, and after a full
cold re-init `rtl_bb_tx()` never returns (its two hardware waits are
unbounded — bound them first if this is revisited). isoldr has nothing to
borrow: its network backend is non-functional and its working transports are
not on G2.

Lesson: when every instrument goes quiet at once and the title is visibly
alive, suspect the transport's power, not its logic.

### 4.13 CD-DA: the loader is the drive

A GD-ROM stores its music as audio tracks, played with `CMD_PLAY_TRACKS` /
`CMD_PLAY_SECTORS`. `cdda.c` answers those itself: it fetches the audio from
the host and plays it on AICA channels 62/63. **The header of `cdda.c` is the
design description**; this section is the summary, the rules and the status.
History: `docs/cdda-crackle-investigation.md` (the first, integrator engine,
to 2026-09-05) and `docs/cdda-double-buffer-investigation.md` (this engine,
2026-09-06 to 09-19).

#### How it works

- **Ring and lead.** Each channel loops over one ring in sound RAM, written by
  a single write head (`cd.write_pos`) kept a fixed distance -- the **lead** --
  ahead of the AICA. The play position is not read from the AICA (its monitor
  select is shared with the title's sound driver): **TMU1** (Pck/16), started
  in the key-on critical section with `TCOR = end_tm` (one loop), stands in for
  it. `cdda_service()` fetches only what the lead is short of, so the fetches
  come one every 53 ms, at the rate the audio is consumed.
  **Why not isoldr's double buffer**: isoldr fills half a ring at a time
  because it reads from the drive, while here every sub-fetch is a UDP round
  trip the title is frozen for (~3 ms). Filling on the half boundary meant 13
  of them inside ~216 ms and then 477 ms of nothing, which cost Snow Surfers
  4-5 dropped frames once a second (2026-09-20). The same work paced is ~3 ms
  every third frame, and the margins stop oscillating.
- **Who calls it**: the top of the GD server loop, `gdGdcGetDrvStat` (every
  frame, before the lock), and `cdda_service_between_chunks()` inside a long
  disc read. Never an interrupt. It declines while `g_gd_in_transfer` is set
  (§4.5 invariant 2) or a title's G2 DMA writes sound RAM (ADST, as isoldr
  does), and is not re-entrant (`busy`).
- **Format: 4-bit Yamaha ADPCM by default** (`DC24`): the host encodes and
  sends the left block then the right. A quarter of PCM's bytes on the wire
  and on G2; the PCM build made the title lag. ADPCM is differential: **every
  key-on restarts the host's encoder** (bit 31 of the request, from
  `cd.restart`), and every byte the AICA plays must be the continuation of the
  one before -- a wrong or replayed block is heard for up to seconds. The host
  never emits a nibble the AICA's decoders disagree on (§16).
  `CDDA_ADPCM=0` selects 16-bit PCM (`DC23`).
- **Geometry** (ADPCM defaults):

  | | |
  | --- | --- |
  | sub-fetch | 4 sectors = 2352 frames = 53 ms, one round trip, into `cdda_pcm` (`.hiram`) |
  | ring | 26 sub-fetches = 61152 samples/channel = **1.39 s** (LEA is 16 bits: at most 65536), 60 KB of sound RAM for both |
  | lead | 20 sub-fetches; less the trigger's lag, **893 ms** of audio ahead of the AICA and 493 ms of played ring behind the write head |
  | per service call | at most 2 sub-fetches: the catch-up rate, not the steady one (one call in three does one) |
  | PCM | 3-sector sub-fetch, 1.04 s ring, 179 KB of sound RAM |

- **Start** (`cdda_prime()`, used by PLAY, RELEASE, SEEK, the theft repair and
  the overrun mute): key off, lay the quiet floor over the whole ring, and arm
  the fill. The services lay the lead like any other audio and key on only when
  it is whole (`cdda_prime_step()`), or sooner if the range ran out first.
- **Silence, never garbage.** If the lead falls within one sub-fetch (53 ms) of
  nothing, after the service has fetched what it could, the channels are keyed
  off and the stream restarts at what was last heard (`cdda_current_lba()`, not
  the write head a second ahead of it): a silence, not a splice
  (`g_cdda_mutes`). The lead is a difference modulo one loop, so it cannot see
  itself gone past zero; a **service gap** over `CDDA_GAP_LIMIT_TICKS` (838 ms,
  the audio the lead holds) says so on TMU2 instead and restarts too.
  **Key-off silences because register 20 carries RR = `0x1f`**; RR = 0 holds
  the level (it did until 2026-09-16).
- **Clock**: `end_tm` comes from `TICKS_X8192` (isoldr's VA1 constant × 16;
  `CDDA_TICKS_X8192=580480` on flycast, whose clocks are exact -- 4535 × 128)
  and the host's trim: a ppm scale in the `size` field of every audio
  ReturnValue, applied at the next TMU1 reload, ignored outside ±1.5 %. The
  test console needs ~184 ppm.
- **Phase**: the fill trigger reads the model `CDDA_LAG_SHIFT` late (an eighth
  of a loop, 173 ms: the early margin). It comes off the lead and goes onto the
  played ring behind the write head, which is what the model being ahead of the
  AICA would eat into. What a title eats by not calling the GD driver (305 ms
  once in Snow Surfers) comes off the lead's 893 ms and is won back at two
  sub-fetches a call. Everything that measures uses `cdda_true_elapsed()` (lag
  added back; during the key-on head start, the time since key-on); only the
  fill trigger uses the lagged model.
- **Fetch integrity.** Audio answers come without acknowledgement round trips,
  so a late answer can meet a later request, naming the same buffer and size.
  `cdda_fetch()`:
  1. **closes the LoadBinary window before each request**, so completion is
     judged on this answer's own window. Until 2026-09-19 the previous
     answer's window, already complete, stayed installed: when an answer's
     LoadBinary and parts were lost and only its ReturnValue arrived, the fetch
     passed and pushed the previous sub-fetch again -- the glitch heard for
     weeks with every counter clean (`g_cdda_retv_nodata` now);
  2. accepts only a ReturnValue whose `address` is the LBA it asked for
     (`g_cdda_wrong_lba`), waiting a little longer if the window completed
     before the echo arrived;
  3. keeps the "door" shut: `cmd_loadbin()` refuses any LoadBinary into the
     staging buffer except the one awaited (`g_bin_stage_*`,
     `g_cdda_stale_lbin`);
  4. suppresses the LoadBinary echo (it would collide with the burst) and ends
     the wait on the last PartBinary (`bin_complete_escape`);
  5. on failure, closes the window and drains for 10 ms.

  Deadline **20 ms** per sub-fetch (normal: ~3 ms). The host drops answers it
  took longer than `CDDA_GIVE_UP` (15 ms) to produce, and answers re-asks from
  a cache of the last 48 requests' bytes.
- **Channels**: TL (byte 41) is **attenuation** (0 = full). The send level
  (DISDL) mirrors the game's CD input level (`0x2040`/`0x2044`), clamped to
  `CDDA_DISDL`; a zero master volume is raised. Every 13 sub-fetches a watchdog
  re-reads the channels: pan/level drift is rewritten in place, a structural
  change confirmed twice in a row is a theft (`g_cdda_ch_stolen`) and restarts
  the stream. An all-zero control word is a failed read, not a theft, and a
  check that does not read back clean right after key-on disables the
  watchdog for that key-on.
- **Sound RAM**: the rings sit under `CDDA_RING_TOP` = `0x150000` (block 20).
  The top of RAM, isoldr's place, reaches into Snow Surfers' samples at our
  size; its `.MLT` sound banks end at `0x13e0c0`.
- **Idle listening**: a service with nothing to fill listens to the network
  for at most 1 ms, at most every 20 ms, so `--diag` is answered while a title
  runs without freezing it on every GD call.
- **TOC**: fetched once per session with `DC22`, area 2 (whole disc).
- **Position**: `cdda_current_lba()` reports the FAD being heard (play
  position vs. write head), for `REQ_STAT`/`GETSCD`.

#### Rules

1. Never service while `g_gd_in_transfer` is set or a title's G2 DMA into
   sound RAM is in flight; from an interrupt, only through the hook's tick
   (`cdda_service_tick()`, §4.15): GD lock free, one sub-fetch, no listening.
2. Every key-on restarts the ADPCM encoder; any re-key goes through
   `cdda_prime()` (re-keying onto the existing ring decodes it from a reset
   decoder: full-scale noise).
3. The host's `CDDA_GIVE_UP` must stay below `CDDA_FETCH_DEADLINE_TICKS`.
4. Key on only over a whole lead; key off before the lead runs out. A byte the
   decoder plays out of sequence is not a click.
5. A fetch is complete only on its own window: close it before the request.
6. Measure with the true model; trigger with the lagged one. The lead is
   modulo one loop, so anything that could outlast it is judged on TMU2.
7. All AICA access by the CPU inside `g2_lock()`, with a FIFO wait at most
   every eight 32-bit stores (bounded at ~2 ms). `g2_lock()` first waits for
   the ring's own DMA (`g2dma_quiesce()`, §4.16), and the ring is written by
   DMA in ADPCM: the unaligned edges (at most 28 bytes a side) by the CPU, the
   32-byte-aligned body on channels 2 (left) and 3 (right), left running when
   `cdda_push()` returns. The staging buffer is `STAGE_BYTES` (`FETCH_BYTES` +
   64), 32-aligned, the left block received at `cdda_pcm + a` (`a` = the left
   destination modulo 32) and the right one moved up to 31 bytes on to be
   congruent to its own. `cdda_prime()` no longer floors the whole ring (61 KB
   of CPU writes, ~9 ms of frozen title); the drain floors one cell, by DMA.
   `CDDA_ADPCM=0` keeps the old CPU writes.
8. AICA reads from the SH4 sometimes return `0x00000000`: an implausible zero
   is a failed read.
9. No variable divisors (libgcc's divider costs ~1 KB, §14.15).
10. **Changing the geometry invalidates things outside `cdda.c`**: the host's
    trim thresholds (`CDDA_TRIM_SEG_S`, `CDDA_TRIM_GAP_MAX`) and its
    `CddaClock` tests assume a stream paced one sub-fetch at a time; the
    encoder cache (`RECENT` = 48) must cover the whole ring (26).

#### Build flags

| Flag | Default | What it does |
| --- | --- | --- |
| `WITH_CDDA` | `1` | the engine (5152 B of `_end`) |
| `CDDA_ADPCM` | `1` | 0 = 16-bit PCM |
| `CDDA_RING_FETCHES` | `26` | ring size, as large as LEA allows |
| `CDDA_LEAD_FETCHES` | `20` | audio kept ahead of the AICA; the rest of the ring is the margin the other way |
| `CDDA_FETCHES_PER_SERVICE` | `2` | sub-fetches per service call, plus one per sub-fetch of audio the gap since the last call consumed, up to the lead (2026-09-27: Windows CE calls the driver hundreds of ms apart) |
| `CDDA_LAG_SHIFT` | `3` | how late the fill trigger reads the model, as a shift of one loop |
| `CDDA_TICKS_X8192` | `578960` | the model's clock ratio (Pck/16 ticks per 8192 samples); flycast's is exactly `580480`, which the flycast sets use |
| `CDDA_SERVICE_DRAIN_ITERS` | `256` | idle listening window (≤ 1 ms, at most every 20 ms); 0 removes it |
| `CDDA_RING_TOP` | `0x150000` | top of the rings in sound RAM |
| `CDDA_DISDL` | `0xf` | send-level ceiling, ~3 dB a step |
| `GD_CDDA_BETWEEN_CHUNKS` | `1` | feed CD-DA during disc reads (`cdfs_syscalls.c`) |

Constants in `cdda.c`: `CDDA_FETCH_DEADLINE_TICKS` 20 ms,
`CDDA_DRAIN_DEADLINE_TICKS` 10 ms, `CDDA_TOC_DEADLINE_TICKS` 500 ms,
`CDDA_MUTE_GUARD` one sub-fetch (53 ms), `CDDA_GAP_LIMIT_TICKS` 838 ms,
`CDDA_CH_BAD_LIMIT` 2, `CDDA_CHECK_FETCHES` 13.

The instruments of the 2026-09 investigation -- store read-back of every word,
clobber probe, sound-RAM activity map, register watch, position probe
(MSLC/CA), TMU1 check, half-gap statistics, test tone -- were removed on
2026-09-19 once the cause was found (the loader went from 3029 to ~1430 lines
of `cdda.c`, and `_end` down by 3204 B). Git history has the code; the loader
set built with them was deleted with the other A/Bs on 2026-09-20.

#### Counters (read with `--diag` or `scripts/dc-counters.py`)

| Question | Counters |
| --- | --- |
| Is it healthy? | `g_cdda_plays`, `g_cdda_fetches` (**sub-fetches**, ~19/s), `g_cdda_fetch_fails` (sub-fetches, each asked again), `g_cdda_mutes` (a silence instead of garbage), `g_cdda_room_min` (least lead ever seen, TMU1 ticks, 3125 ≈ 1 ms; ~893 ms is healthy), `g_cdda_svc_gap_max` (longest time without a service, TMU2 ticks: the title not calling the GD driver) |
| Right answers? | `g_cdda_wrong_lba`, `g_cdda_retv_nodata` (our LBA came back without our data: a lost answer, caught), `g_cdda_stale_lbin` |
| Channels | `g_cdda_ch_stolen` |
| Clock | `g_cdda_end_tm`, `g_cdda_scale_ppm` (1000000 = no trim yet) |
| Misc | `g_cdda_toc_fails`, `g_cdda_last_lba` |

TMU1 is Pck/16 (3125 ticks/ms) and TMU2 Pck/4 (12500): `--diag` prints both in
milliseconds. `room_min` is computed modulo one loop and cannot see a lead
gone past zero; the service-gap limit on TMU2 can. When the counters are clean and a
glitch is heard, record the output and align it with the exact offline decode
(Status): that is what found the last one.

#### Status (2026-09-20)

Tested on one console (BBA) and under flycast, with Snow Surfers PAL and the
loader relocated.

- **The glitch heard for weeks with clean counters is found and fixed.** A
  flycast recording, aligned against the host's stream decoded offline with
  flycast's `DecodeADPCM`, matched to −25..−35 dB with no lost sample until
  25.653 s, the first slot of a half, which held the last sub-fetch of the
  half before, in both ears; the right ear then played the right bytes from a
  wrong decoder state for seconds. The fetch had received only its
  ReturnValue (Fetch integrity, item 1). With the fix, flycast plays the intro
  without a glitch. **Not yet measured on the console.**
- **The right ear of a short sub-fetch** (the last of a range that is not a
  multiple of 4 sectors) was read from the middle of the staging buffer, where
  the host does not put it: stale bytes and a desynchronised right decoder at
  every loop of a repeating track. Found while simplifying, fixed the same day.
- **Repeating tracks did not loop when their length was odd** (6 of Snow
  Surfers' 14 audio tracks): ADPCM fetches go in pairs of sectors, the lone
  final sector rounded down to 0, and 0 read as the end of the range before the
  repeat was reached, so the level's music played once and went silent. The
  lone sector is now skipped and the repeat applies (checked by running the
  function itself over odd and even ranges, against the old code). On the host,
  a play range running past the end of a track's file (the next track's
  undumped pregap: 150 sectors after Snow Surfers' track 18) was an error the
  loader re-asked forever; it is silence now (GDI and CDI). **Not yet measured
  on hardware.**
- **The music cost 4-5 dropped frames once a second** in Snow Surfers, once
  looping worked and a level played its track for more than one pass. It was
  the schedule, not a fault: the double buffer fetched a whole half on its
  boundary, 13 round trips of ~3 ms inside ~216 ms (7 consecutive frames at
  ~6 ms each), then 477 ms idle. The engine now keeps a fixed lead and fetches
  one sub-fetch every 53 ms -- one service call in three, ~3 ms -- with the
  margins constant instead of oscillating (893 ms of audio ahead, 493 ms of
  played ring behind) . Checked by compiling the real `cdda.c` against a
  simulated AICA and clocks on the PC (**how**: copy `cdda.c`, redirect the
  hardware macros -- `SNDREG32`, `AICA_RAM`, the `TMU_*` registers,
  `aica_dma_busy`, `G2_*` -- at their `#define` lines to arrays and variables,
  stub `bb->loop()` into a host that answers the LBA in `pkt_buf` after a
  chosen delay, and drive a virtual Pck/4 clock; nothing in the logic is
  re-typed, and reading TMU2 must cost a tick or the key-off spin never ends):
  in a 60 s run,
  at most 1 fetch per call in the steady state, lead 891..1064 ms, no mute;
  with a 700 ms service stall every 5 s, lead down to 279 ms and still no mute;
  with one fetch in seven failing, lead 872 ms; a 1 s stall trips the new TMU2
  gap limit, as designed. Track ends land within 1.3 ms of the audio's length.
  **Not yet measured on hardware.**
- Standing from before: the overrun mute, the asynchronous prime and RR =
  `0x1f` (2026-09-16); the host's clamp-safe encoder and re-asks answered from
  its history before any disc read; flycast's exact clock constant; the true
  model during the key-on head start.
- The log of every measurement from 2026-09-12 to 09-19 is in
  `docs/cdda-double-buffer-investigation.md`.

Open questions and limits:

- No phase servo. Long sessions without a PLAY/seek rely on the trim, whose
  estimator the host had to be re-taught for a paced stream (§16): it used to
  find its window boundaries in the idle tail between halves, and there is no
  longer one.
- A title that stops calling the GD driver stops the music (isoldr too).
- The game's CD input level is followed by the channel watchdog (every
  `CDDA_CHECK_FETCHES` sub-fetches, 2026-09-27); 0 means full only until the
  game has set a level, and mutes after two zero reads in a row.
- An ADPCM range's odd final sector is skipped (~13 ms, once per pass). A play
  range shorter than the lead keys on early and is untested on hardware, and
  one shorter than the host's history (~2.5 s) would be answered from it on the
  loop back.

#### Lessons from this engine

- **While the fill is triggered by a buffer boundary, the margin and the
  burstiness are the same quantity.** With two halves the fill may start only
  when the AICA leaves a half and must finish before it comes back, so every
  millisecond of margin has to be fetched ahead of time, in a burst. A write
  head with a target lead separates them: the margin is the lead, the rate is
  the audio rate, and both are constant. The ring never changed size.
- A counter panel cannot report a schedule. Every CD-DA counter was clean
  while the title dropped 4-5 frames a second; what named it was the period the
  player heard, 693 ms, which was the half.
- A probe that baselines on its own read-back measures persistence, not
  arrival: compare with the source.
- Output-side registers (TL, send levels, mixer) are past anything the loader
  can read back; check a field's meaning against KOS's `arm/aica.c` or isoldr,
  not its name (§14.21).
- Measure drift only over windows aligned to halves: a hand measurement on
  unaligned windows once reported 1.16 % against a real 184 ppm.
- A warning that fires in every session cannot be told apart from a fault.
- Counters and the listener disagreeing means an instrument is blind somewhere.
  Beware the converse: the session where `g_cdda_stale` matched the glitches
  heard was read as proof the set was complete, and the next session glitched
  with `stale` at 0. The event that was missing had **no counter at all**, and
  the code that caused it said `/* Start anyway */` — a comment is not a
  measurement.
- A model cannot audit itself, and a model that wraps cannot see a whole period
  of lateness. Three counters read "healthy" for weeks over an event that
  replays 693 ms of audio, because all three asked the same wrapping clock. The
  fix is a clock the model is not derived from (TMU2), not a better threshold.
- In a differential format, a hole is not a hole: the decoder carries it
  forward. Muting before it and restarting clean is the only bounded outcome,
  and a counter for "the AICA entered a partial half" is not a counter for
  what was heard.
- A completion test must be about this transfer. A window left over from the
  previous one, with the same address and size, is already complete, and the
  one packet that did arrive (the ReturnValue, echoing the right LBA) was
  enough to push stale bytes that every probe then confirmed.
- When every counter is clean, record the output. Aligned against an exact
  offline decode, a recording localises an ADPCM fault to the sub-fetch and
  names the bytes that were played; no instrument on the console could.
- A key-off is only as good as the release rate behind it. Read every field a
  register write zeroes as a decision (§14.21).
- A model shared by two machines must agree with every model of the other
  end, not the one a comment picked. The encoder was pinned against the crate
  and MAME, and the note that Sega's encoder clamps was recorded and set aside;
  flycast, which this project already reads, clamps too. A defect in what is
  sent is invisible to every counter on the receiving side: simulate the codec
  offline, on the disc's own audio, before instrumenting the console further.
- Nothing one-off and lazy may happen inside a syscall the console is timing.
  The host's deflate index cost 140 ms on the first read of a track, which is
  the whole fetch budget several times over; it now happens on a thread before
  the title asks for music.

### 4.14 The drive is stopped at boot

`gd_spin_down_drive()` (`cdfs_syscalls.c`, `WITH_GD_SPINDOWN=1`, called from
`main()`) puts the real GD-ROM in `<STANDBY>`. Nothing in a session reads the
disc again — a title's GD syscalls are answered from the host, CD-DA comes off
the network, and an uploaded homebrew never touches the drive — so the disc the
BIOS spun up to boot `1st_read.bin` would otherwise keep turning.

What it is actually worth: **the drive's firmware already does this after
180 s**. The SPI mode page's Standby Time defaults to `B4h` seconds of
`<PAUSE>` before the unit goes to `<STANDBY>` (`Cdif131e.txt`, "Standby Time
(Byte 4-5)", whose own note is that a large value hurts MTBF). So this buys
three minutes of rotation per boot, immediately and audibly, and makes the
stopped state deterministic rather than something the next command postpones.
It is **reversible**: a title run without a disc image, or a KOS program
calling `cdrom_init()`, spins the drive back up, paying that read's spin-up.

Three things it depends on:

1. **It calls the real BIOS driver**, through the syscall vector at
   `0xac0000bc` — so it must run after `cdfs_redir_save()`/`cdfs_redir_disable()`
   have put the BIOS back on that vector, and it is the only place in the
   loader that does. The ABI is the one `cdfs_redir.s` decodes for a title:
   r7 = function index, r6 = 0.
2. **The BIOS driver is a coroutine too** (§4.5). `ReqCmd` only queues the
   STOP; it progresses while we call `ExecServer`, so the loop is the driver's
   scheduler and not a poll of the drive. Leaving early leaves the command
   queued in a driver nobody will run again. `InitSystem` is the **fallback**
   and not the first call: `ReqCmd`/`ExecServer`/`GetCmdStat` are non-blocking
   by construction, `InitSystem` is the one that may bring hardware up, and
   the BIOS has just read the disc with this driver.
3. **Bounded on a spin count** (`GD_SPINDOWN_SPIN_LIMIT`), not on TMU2, which
   is not running that early — and a TMU2 deadline would cost more footprint
   than the whole function is worth (§4.6: it already spent 160 of the 216 B
   that were under `0x8c00c000`). What one `ExecServer` costs has not been
   measured, so the bound is on iterations and not on time. A drive that will
   not answer is left spinning rather than spun on.

**Off by default since 2026-09-28** (`WITH_GD_SPINDOWN=0`): its 160 bytes
paid for the Katana interrupt hook under the HIGH bound (§4.6).

`g_gd_spindown` says how it went: the final command status + 2 (**4** =
COMPLETED, the healthy answer; 1 FAILED, 2 IDLE, 5 STREAMING), plus 7 "the
driver would not take the command" and 8 "it never finished". 0 means the call
never ran — `WITH_GD_SPINDOWN=0`, or an empty syscall vector.

**Not yet measured on hardware.** The check that needs no instrument is that
the drive goes quiet a second or two after the loader's screen appears.

### 4.15 Windows CE titles

Established 2026-09-20..27 on Sega Rally 2 PAL (GDI), which now boots, plays
and saves under the loader; the history is `docs/wince-investigation.md`.
**Booting a CE title boots an operating system**: the disc carries `NK.EXE`,
`COREDLL.DLL`, `GWES.EXE`, `FILESYS.EXE`, ~250 files and the game;
`0WINCEOS.BIN` is the ROM image that starts it and then loads the rest off the
disc through CE's own GD driver (`wsegacd.dll`).

**Boot** (host, `src/wince.rs` and `boot::WinCe`; automatic, `--no-wince` to
disable):

- The first 2048-byte sector of `0WINCEOS.BIN` is dropped and the rest loaded
  at `0x8c010000`, as isoldr does (parity, not proof).
- The title is entered through the disc's own second bootstrap
  (`--boot-ipbin`), which hands over SR `0x700000f0` and zeroes
  `0x8c00fc00..0x8c010000` -- the two places `go.S` differs from a real boot.
  IP.BIN is stock; CE reads its header at `0x8c008000` at run time.

**Memory.** CE's ROMHDR gives its kernel `0x8c143000..0x8cef0000`, allocated
top-down, and driver globals `0x8cef0000..0x8d000000`. The host places the
loader at `0x8cee0000` (HIGH layout) and lowers `ulRAMEnd` to it in the
upload. **Nothing in the loader may write outside its own span while a title
runs** -- the old post-mortem block at `0x8cf0c000` did, into CE's driver
globals, on every read (§11). Inside the span, three regions are dead while a
title runs and are reused for it:

| Region | Before `EXEC` | Under a CE title |
| --- | --- | --- |
| `.gdstage` (above `_end`, under `_stack`) | the loader's own stack | `gd_stage_big`, 4 sectors |
| Maple DMA page (4 KB) | `MAPL` commands | the network exchange's stack |
| `gd_stage` (`.hiram`, 3 sectors) | -- | the TOC; streams under a Katana title |

`.gdstage` is used only under the MMU: in the LOW family that range is a
Katana title's stack.

**The GD driver.**

- CE calls the BIOS driver body `0x8c0010f0` directly (§4.5, third door); the
  host points those calls at `_gd_bios_entry`. This is what makes CE servable
  over the network at all.
- **CE runs with the MMU on.** DMAREAD's destination is a physical page frame
  (`gdGdcReqCmd()` makes it P1). Every other buffer is a **virtual** address
  only the title's translation reaches (`gd_is_virtual()`: P0 with MMUCR.AT):
  such a read is received into a stage and copied out with `memcpy.S`; never
  through `memfuncs.c`, whose copies store in the source's segment (§8), and
  never from the host.
- **Streams** (`PIOREAD_STREAM_EX` 39 and friends) are served by
  `data_stream()`: PIO pieces are chained through the title's callback. DMA
  stream pieces would need the G1 DMA-end interrupt, which nothing raises, so
  the host turns the branch that picks DMA for an aligned read into a branch
  to PIO (`0x8c099f74` in Sega Rally 2; §7h of the investigation).
- **Status words matter.** A PROCESSING status with status[3] = `WAIT_IRQ`
  makes CE sleep on the GD interrupt; the retry path therefore clears it under
  the MMU so CE polls. A failed read is fatal to CE's driver, so under the MMU
  a read gets `GD_READ_RETRIES_MMU` (20) retries.

**A preemptive OS calls us.** A Katana title's interrupt handlers run on top
of a GD syscall and return; CE's timer interrupt enters its scheduler, which
can run other threads for a whole quantum while the loader is half way
through a network exchange -- the answer then overflows the ring and the
deadline runs out while the loader is not running at all. Masking interrupts
is not enough: CE calls the driver on a thread stack at a virtual address, and
a TLB miss or an uncommitted stack page enters CE's kernel, which sets IMASK 0.
So every network exchange of the GD path (`ReadSectors`, `GetTOC`,
`cdda_fetch`) goes through `gd_exchange()`: under the MMU (and under a KOS
title, `g_gd_kos`, §16) it masks interrupts
(`bb_irq_hold()`, `adapter.h`) and then runs on the Maple page
(`gd_on_loader_stack`, `cdfs_redir.s`) -- in that order, because it is one
stack for every thread. The adapter loops mask too, and every exchange ends
with reception off (after a deadline it used to stay on, and the LAN filled
the ring for the next attempt). Katana titles see only that `bb->stop()`,
which a successful answer's `cmd_retval()` already did.

**Performance.** A real drive takes ~0.4 s over a 604 KB read with the CPU
free, and CE's menu keeps drawing through it. Served synchronously the read
was ~0.1 s of CPU the loader kept, and the menu stopped for that. Yielding
between chunks with nobody working meanwhile made it worse (a 5 ms sleep per
chunk: 7s). A round trip costs ~1.5 ms fixed plus ~0.17 ms a sector. Hence
the asynchronous reads of phase 3 (below): the thread sleeps and the hook's
tick moves the chunk.

**CD-DA: CE never calls `GetDrvStat`.** The music is fed only when CE runs the
GD server (ExecServer), hundreds of ms apart. `cdda_fill()` therefore budgets
the sub-fetches the gap consumed plus two, up to the whole lead (§4.13).
Gaps close to the lead (704 ms measured against 893) still cost audible
glitches; the fix is the same interrupt.

**The interrupt hook** (2026-09-27, `irq.c`, `irq_hook.S`, `WITH_IRQ_HOOK`).
`docs/wince-investigation.md` §9. Phase 1 (installed, counted) **measured
under flycast and on the console** (~600 interrupts a second, a tick ~1 µs).
Phase 2 (the tick feeds CD-DA) built, **not yet run**.

- **Two entries are recognised** (2026-09-28): Windows CE's (below) and the
  Katana library's, which copies six nops then `mov.l r0,@-r15 ; mov.l
  @(disp,pc),r0 ; jmp @r0 ; mov.l r1,@-r15` to `VBR+0x100/+0x400/+0x600`
  (Crazy Taxi's crt0 sets VBR to our `0x8c00f400` and copies it in, as
  Sonic Adventure does). Over three nops nothing needs rebuilding: `irq_out`
  gives back r0, r1 and r15 (the title's r0 parked just under its SP, popped
  in the delay slot) and resumes at `+0x606` (`g_irq_nop_entry` 1). The words
  under the vector may be zero or nop padding -- our `exception.bin` pads with
  nops. From the tick, network code runs on the loader's stack
  (`gd_in_irq()`: SR.BL), and the tick keeps off the network while CD-DA is
  (`cdda_busy`: a Katana `GetDrvStat` services it before taking the GD lock).
  It looks at a read on the wire at most every 0.5 ms, and each look takes what the ring holds and leaves (`GA_POLL_ITERS` 2, 9r): the RX interrupt brings it back for the rest.
- **The BBA's RX interrupt** (Katana, 2026-09-28, 9o). The title's own
  interrupts come bunched (~790 a second on Crazy Taxi), and a chunk back
  after ~2.5 ms waited up to 10 ms more. The chip already raises its line
  (GAPS `0x1414` = 1, `RT_INTRMASK` = the RX bits); `irq_rx_arm()` routes
  Holly EXT bit 3 (KOS's `ASIC_EVT_EXP_PCI`) to a level while a chunk is on
  the wire: the highest of IML6/IML4/IML2 (INTEVT `0x320`/`0x360`/`0x3a0`)
  whose three masks the title left at zero when the hook went in, else IML6,
  **shared** -- Crazy Taxi uses IML4, and a first version that took only a
  free IML4 never armed (9p). The tick acknowledges the chip (`rtl_irq_ack()`:
  the RX status bits; the loop still finds the frames through its RxBufEmpty
  net) and looks at the read at once; `irq_entry` then returns with `rte`
  itself (`g_irq_swallow`) unless something of the title's is pending on that
  level (IST & its IML masks), in which case the title's handler runs and
  finds our bit clear. **Only an entry with the chip's bit pending is ever
  swallowed**: 9p named IML2 and IML6 the wrong way round, took Crazy Taxi's
  own IML6 interrupts for the chip's, and swallowed them for good -- the
  title froze (9q). `g_irq_rx` counts them, `g_irq_rx_evt` names the level,
  `g_irq_iml[9]` keeps the masks the title had (IML2/4/6 NRM, EXT, ERR).
- Every SH4 interrupt (CE's TMU0 tick, VBlank...) enters at `VBR+0x600`. The
  loader copies a 30-byte template over the **title's live table** (`stc vbr`)
  from each GD syscall under the MMU (`irq_hook_check()` in ReqCmd and
  GetCmdStat): `+0x5e8` trampoline and its two literals (our stack, our
  entry), `+0x600` `nop; bra; nop`. `irq_entry` saves everything, the FPU
  included, calls `irq_tick()` on the top of the Maple page, and resumes at
  `+0x606` with CE's three replaced instructions rebuilt (`r6 = @(40,r7)`,
  `r0 = *(VBR+0x68c)`, `r1 = r6`) and `r15` back from SGR.
- **Only Windows CE's exact entry is patched**: `+0x5e8..+0x600` zero and
  `567a d022 6163` at `+0x600` (Sega Rally 2, VBR `0x8c012110`). Anything
  else is refused and counted once per VBR (`g_irq_refused`,
  `g_irq_refused_vbr`); the GD path then works as without the hook. A table
  that loses the hook gets it back (`g_irq_rehooks`). This is what the first
  hook (removed 2026-08-07, `docs/loader-comparison.md` 2.3) lacked.
- Rules the entry keeps, each a reset if broken (SR.BL is 1 throughout):
  **r15 is replaced before any push** (CE's thread stacks are virtual, and a
  TLB miss under BL resets); **FD is cleared and FPSCR, FPUL and both FP banks
  saved** (GCC spills to FP registers, and CE switches the FPU lazily); the
  pair stores need r15 8-aligned (22 longs pushed before them: keep it even);
  C reached from `irq_tick()` touches P1/P2 only and never waits.
- **Two stacks, never one**: the hook's is the Maple page's first KB, the
  exchange's (`gd_on_loader_stack()`) the page top. Sharing it corrupted an
  exchange CE had preempted (`docs/wince-investigation.md` 9b).
- **An exchange must not enter CE's kernel.** CE runs its threads with SR.FD
  set (lazy FPU); the exchange's first FPU instruction (`fmov.d` in
  `SH4_aligned_memcpy`, GCC's FP spills) raised an exception CE handled on
  our stack with IMASK 0, where it could preempt. `gd_on_loader_stack()` now
  clears FD and saves the FP registers around fn (`fpu_push`/`fpu_pop`,
  `cdfs_redir.s`), as the hook does.
- Counters: `g_irq_hooked`, `g_irq_vbr`, `g_irq_entries` (every interrupt),
  `g_irq_ticks`, `g_irq_tick_max` (TMU2), `g_irq_tick_sum` (all ticks added: the CPU the hook takes; `--diag` shows its growth in ms), `g_irq_evt_last` (INTEVT).

**Phase 2: the tick feeds CD-DA.** At most every 5 ms (TMU2), and only while
the GD lock is free -- every network use of the GD path (reads, TOC, CD-DA
from the server) holds it, and no syscall starts while the tick runs, so a
free lock means nobody is half way through `pkt_buf`, `bin_info` or the ring.
`cdda_service_tick()`: one sub-fetch per call, no listening window (it would
run the network path on the hook's 1 KB stack); the exchange itself switches
to `gd_on_loader_stack()`'s. SR.BL stays set throughout, so the title's
interrupts wait for a sub-fetch (~3 ms, 20 ms at worst): phase 4 moves that
wait out of the interrupt.

**Phase 3: asynchronous disc reads** (`data_transfer_async()`,
`cdfs_syscalls.c`; PIOREAD/DMAREAD of more than one sector, CE with the hook
in, BBA only). **The tick moves the read, not the thread**: CE's GD thread
wakes only ~10 times a second while the menu draws, and a first version in
which the thread copied and posted each chunk managed 72 KB/s (wince 9f). So:
the thread starts the read and yields (`WAIT_INTERNAL`); the tick (GD lock
free) drains the ring in passes of `GA_POLL_ITERS` loop turns, gives each
4-sector chunk its verdict (window whole = done, 250 ms = failed, a
ReturnValue over a hole = stale), **copies it into the title's buffer and
posts the next**, at the network's pace. The buffer is virtual and the tick
cannot take a TLB miss (BL set), so on each wake the thread translates the
next `GA_XLAT_PAGES` (32) 4 KB pages **by CE's own page tables**
(`ga_walk()`: TTB, section, MemBlock, entry = PTEL + 1 -- what CE's TLB
refill reads, and what flycast's `USE_WINCE_HACK` reads), checks each with a
byte inverted through the virtual address and read back uncached at the
physical one, and purges the buffer's lines through the virtual address,
into `ga_xpa[]` (`.hiram`); the tick writes through P1 and writes its lines
back. A UTLB probe did this until 9j and could not work under flycast
(below). Between two chunks the tick feeds CD-DA (`cdda_service_tick()`), as
the synchronous loop does: `irq_tick()` services it only when no read is on
the wire, and a load is seconds of chunks back to back (9k). A chunk is done with its ReturnValue,
not merely a whole window (a late one met the next CD-DA fetch as a wrong
LBA). A chunk beyond the
translated pages waits for the thread, which copies it the old way. DMAREAD
(physical) needs no translation. Counters `g_ga_posts`, `g_ga_irq_done`
(chunks the tick finished, copy included), `g_ga_xlat_miss`, `g_ga_sync`, `g_ga_wakes` (thread resumes with a chunk on the wire)
(reads finished synchronously: when nothing translates, the read goes back
to the old loop rather than trickle at the thread's ~20 wakes a second).
Built, **not yet run**.

Also not done: `CMD_REQ_MODE`/`SET_MODE`/`INIT` answered for real (§4.5 known
gaps; measure first).

**Debugging CE.**

- **flycast is built with `FAST_MMU`** (`core/build.h`): it serves
  translations from its own 65536-entry cache, so an access can succeed
  without the page's entry being in the 64-entry UTLB, and a UTLB probe
  misses pages a real SH4 would have had to reload. Translate by CE's page
  tables instead (`ga_walk()`, 9j): flycast resolves CE's misses with the
  same walk (`USE_WINCE_HACK`, `core/hw/sh4/modules/wince.h`).

- Under flycast, the GDB stub stops the emulation on every MMU exception while
  a client is attached (`debugger::debugTrap`), and every CE API call is one
  (a jump to a trap address like `0xfffffd3b`). One attach halts flycast for
  good: read everything in that attach (`scripts/dc-integrity.py --elf
  <relocated ELF>` for the loader's integrity, a dump of the loader span for
  its counters), then restart flycast.
- A console recording of the music, aligned against the disc's track, names
  what the AICA played (the ring replaying itself was found that way).
- In a dump, `pc` is SPC, and an exception taken in a delay slot sets SPC to
  the **branch** before it: a `pc` on a `bra` means the fault is at `pc+2`.

### 4.16 G2 DMA: the CPU stays free while the bus moves the bytes

Established 2026-09-28/29 (`docs/g2-dma-investigation.md`). Everything the
loader used to move over G2 with the CPU -- the BBA's RX ring, read a word at a
time (~94 us a frame, the CPU waiting on the bus), and the CD-DA rings, written
the same way -- now goes by DMA where a title is running, so the title executes
meanwhile. **The bus is the limit, not the CPU**: measured on the console, 1536
bytes from the BBA take 99 us by CPU and 94 us by DMA; 2368 bytes to the AICA
336 us and 314 us. What is gained is CPU, not transfer time.

- **Channels.** 0 is the title's sound driver, 1 the BBA's (KOS's choice; the RX
  DMA), 2 and 3 the CD-DA ring's left and right. All four reach the AICA and the
  BBA's SRAM with no wrong word, also with the CPU writing the AICA meanwhile
  (`G2DMA_BENCH=1`, the torture round; positive control included). Registers:
  `g2dma.h`. `G2APRO` is written on each start; TSEL 4 (CPU trigger, suspend
  honoured).
- **Rule: the CPU never uses G2 over the loader's own DMA.** Every entry point
  that does calls `g2dma_quiesce()` (CD-DA channels) and, in `rtl8139.c`,
  `rx_settle(1)`, first: `g2_lock()`, `cdda_fetch()`, `rtl_bb_tx/start/stop/
  irq_ack/loop`, `la_bb_tx/loop`. Bounded (~2 ms), and a channel that will not
  end is aborted (`g_g2dma_timeouts`, must stay 0).
- **CD-DA** (§4.13 rule 7): edges by CPU, body by DMA on 2 and 3, left running.
- **THE TITLE'S G2 DMA IS SUSPENDED WHILE THE TICK RUNS** (`g2dma_hold()` /
  `g2dma_release()`, nested; `cdda.c`'s `g2_lock()` uses them too), as KOS's
  `g2_lock()` does around every G2 access. Measured 2026-09-30/10-01 on Sonic
  Adventure 2: with the asynchronous Katana reads the tick read the BBA's ring
  while the title's sound driver moved its banks to the AICA, and the title
  waited forever right after loading `SDRV`/`SMLT`/`SMPB` -- on the console
  only. Channels with a transfer of the loader's running are left alone, and
  `g2dma_start()` lifts the suspend of the channel it takes. The synchronous GD
  path suspends **under KOS only** (`gd_exchange()`, `g_gd_kos`, 2026-10-02):
  the GTA III port failed one stream read five attempts in a row with the host
  answering each, then asserted on EIO; KOS itself suspends G2 DMA around every
  CPU access, and its interrupts are masked for the exchange anyway. Suspect,
  not proven: `--diag` sees nothing under KOS between reads. Katana titles'
  synchronous path still does not suspend (a 250 ms read deadline would stall
  their sound): never seen to matter.
- **A CHANNEL THE TITLE USES IS NEVER THE RX DMA'S.** A Katana title arms the
  end bits of all four channels on its IML4 (`0x7f000`), and its handler takes
  the end of a transfer of ours on a channel it drives for the end of its own.
  Crazy Taxi 2 does not use channel 1 and ignores ours; Sonic Adventure 2 does,
  and went to a black screen before its menu in every build that received
  frames by DMA there. `g2dma_pick()` chooses per frame, 3 then 2 then 1, a
  channel neither busy nor the title's (`g_g2dma_foreign`, bit n = channel n):
  seen busy or with its end bit up while nothing of ours is on it, or with a
  RAM address (STAR) outside `.hiram` -- the trace a finished transfer leaves,
  which the first two tests miss. `g2dma_forget()` clears an earlier loader's
  traces at EXEC. None free: that frame goes by CPU. **Measured on the console
  2026-10-01**: Sonic Adventure 2, Crazy Taxi 2 and Shenmue II all run with it
  (`loaders/` md5 `b94bc113…`); channels 2 and 3 are also CD-DA's, whose own transfers do not yet
  avoid a title's channel. The loader clears only its own channels' end bits
  (`g2dma_mine`).
- **RX under the interrupt hook, Katana titles only** (`rx_iml` set: the BBA's
  own interrupt level exists): a long frame whose first 64 bytes -- read by the
  CPU, 4 us -- are a PBIN for us (`rx_is_pbin()`) is copied by DMA into
  `raw_current_pkt` (`rx_dma_start()`), the ring not advanced, and the tick
  returns to the title. The frame is processed (`process_pkt`, `rx_advance`)
  when the DMA is over, by `rx_settle()`. Anything else, and everything outside
  the tick or under Windows CE (no RX interrupt to wake it), is copied as
  before. Processing a PBIN never transmits, so it can be finished from any
  context; `rx_settle(1)` (wait) is in every entry point above, `rx_settle(0)`
  (leave a running DMA alone) at the top of `rtl_bb_loop()` and `rtl_bb_rx()`.
- **THE END OF OUR DMA REACHES US ON THE TITLE'S LEVEL.** Katana titles arm
  bits 12..18 of IML4's NRM mask (Crazy Taxi: `0x7f000`, `g_irq_iml`), Ext1's DMA
  end (bit 16) among them, so the interrupt is taken as IML4 (evt `0x360`) and
  never as our IML2 (`0x3a0`). It used to be dropped by the tick's "a read was
  looked at a moment ago" limit (`IRQ_READ_PERIOD`), the frame waited for the
  next interrupt of the title's (~1.6 ms, one frame per entry) and a load ran at
  ~0.7 MiB/s with the tick never finishing a chunk. `irq_tick()` now takes a
  finished DMA (bit 16 up, at any level) as a reason to look now. Result on
  Crazy Taxi 2, load to the menu: 71 chunks of 71 finished by the tick, latency
  DMA to processing ~81 us, tick CPU 0.75 ms a chunk (1.5 before), `g_irq_tick_max`
  0.3 ms (1.7), ~1.5 MiB/s (0.7 without the fix, ~2 with the CPU copy), no lag.
- **A fetch inside the tick takes its frames by CPU** (2026-09-29, Aqua GT, the
  first title to combine CD-DA and the Katana hook). `irq_tick()` sets
  `g_rx_dma_tick` for its whole length, and `cdda_fetch()` waits inside
  `bb->loop()`: the loop stopped at the first PBIN (DMA started, `escape_loop`),
  the fetch judged a window barely begun, and 5 fetches in 6 failed, each up to
  20 ms with SR.BL set (`g_cdda_fetch_fails` 11387 for 2246 fetches,
  `g_cdda_retv_nodata` and `g_pbin_rejected` rising together, `g_cdda_room_min`
  47 ms). `cdda_service_tick()` now clears `g_rx_dma_tick` around the service.
  `g_ga_irq_done` 0 there is not a fault: Aqua GT polls ExecServer ~2600/s and
  the thread finishes each chunk first. **Not yet measured on the console.**
- **What did not work**, so nobody repeats it: acknowledging the chip's RX
  status before the DMA and leaving its interrupt armed (its line rises mid-DMA,
  the tick waits for the end: 4.7 % of the CPU against 1.1 %, same throughput);
  a synchronous RX DMA (94 us against 99 us: nothing to gain, only the wait
  changes hands); reading the whole ring in one DMA (no buffer for it: `.hiram`
  has ~500 B left).
- **The Internet checksum is fast** (`packet.c`, `csum_sum16()`): 32-bit reads,
  the two halves added apart, no carry test per word, one fold at the end --
  the old loop was ~18 us over a 1440-byte payload. Checked bit for bit against
  the old loops on the PC (38 426 cases: every length 0..1600, four alignments,
  all-0 and all-`0xff` included).
- **What is left** in the tick is `process_pkt`: mostly the RAM-to-RAM copy of
  `cmd_partbin()` and its purge (est. 13 us a frame of ~50). A zero copy -- DMA
  of the payload to its destination, the checksum then read from there, so the
  frame is written before it is verified -- would gain ~10-15 us a frame:
  ~0.15 ms of a chunk of ~8 ms, invisible in play. Not done; it needs the LOW
  `.gdstage` assert relaxed (the CD image has 240 B left) and the host's
  relocation bound with it. The NIC cannot verify the checksum: the RTL8139
  offload exists only on the C+ (descriptor mode), and it already discards
  frames with a bad Ethernet CRC (RXCONFIG accepts no errors).
- **The legacy 1024-byte payload mode (dc-tool < 2.0.0) was removed from the
  loader** (2026-09-28: `DCTOOL_MAJOR < 2` branches in `commands.c` and
  `syscalls.c`) for its ~220 bytes. `dc-tool -l` no longer works against this
  loader; dc-tool 2.x and the Rust host are unaffected.
- **Counters**: `g_g2dma_timeouts` (must stay 0) and `g_rx_dma_frames`. Read
  `g_ga_irq_done` against `g_ga_posts`: that is the tick finishing chunks. The
  investigation's others (latency, causes of each entry, the title's IMASK)
  were removed once they had answered; the git history has them.
- **flycast completes a G2 DMA the moment it starts** (`aica_if.cpp` copies
  everything at once and defers only the end interrupt and the start bit): a
  missing wait is invisible there, and so is the title's mask carrying our
  event. The console is the judge.

## 5. 1st_read bootstrap (`target-src/1st_read`)

`loader.s` + `disable.s`, linked at `-Ttext=0x8c010000`, `objcopy`'d, then
**scrambled** with KOS's `scramble` (a retail Dreamcast refuses an unscrambled
binary). `loader.s` masks interrupts, disables the cache, zero-fills
`0x8c004000`–`0x8c010000`, copies `exception.bin` to `0xac00f400` and
`dcload.bin` to `0xac004000`, then jumps. Sizes are link-time symbol
differences; addresses come from `make -C ../dcload print-base` /
`print-guest-vbr` / `print-zero-end`.

## 6. Generated files / gotchas

- Every translation unit in `target-src/dcload/` and `example-src/` emits a
  `*.asm` listing (`-Wa,-adghlmns=$*.asm`). Build output; `make clean` removes
  them. `DCLOAD_LTO=1` suppresses them.
- `target-src/dcload/dcload.x` is generated from `dcload.x.in`; `loaders/` is
  `make loaders` output.
- `.gitignore` covers `*.o *.bin *.lzo *.srec *.exe *.elf *.asm *.map` and the
  example programs, at the top level only.
- Protocol structs are **wire format**: `host-src/tool/commands.h` and
  `target-src/dcload/syscalls.h` must match byte for byte.
- The "dcload is loaded" sentinel is `0xdeadbeef` at `0x8c004004`
  (`example-src/dcload-syscalls.c`).

## 7. PC host tool (`host-src/tool`)

### 7.1 What it produces

`dc-tool-ip`, linked against `libelf` (default) or sh-elf `libbfd` +
`libiberty` (+ `libsframe`, `libintl`) under MinGW; `-lz` where needed;
`-lws2_32 -lwsock32 -liconv` on MinGW. It does not serve `DC23`/`DC24`.

| File | Role |
| --- | --- |
| `dc-tool.c` | the CLI, with a bundled BSD `getopt` fallback |
| `syscalls.c/.h` | per-syscall handlers mirroring the DC side |
| `dc-io.h` | transport primitives and `send_cmd` |
| `commands.h` | wire-format `command_t` and `CMD_*` IDs |
| `dcload-types.h` | the DC's `dirent`/`stat` layouts |
| `config.h` | `PACKAGE_VERSION` ← `DCLOAD_VERSION` |
| `utils.c/.h` | logging, IP helpers, `exception_struct_t` |
| `shim.c`, `unlink.c` | MinGW-only helpers |

### 7.2 Command line

Options `x:u:d:a:s:t:i:nlqhrgf`, plus `m:c:` outside MinGW.

| Flag | Arg | Meaning |
| --- | --- | --- |
| `-x` | file | upload and execute (default `0x0c010000`) |
| `-u` | file | upload only |
| `-d` | file | download to file |
| `-a` | address | override upload address |
| `-s` | size | override download size |
| `-t` | `ip[:port]` | target; port defaults to `53535` |
| `-n` | — | do not attach console / fileserver |
| `-q` | — | do not clear screen before download |
| `-m` | path | map `/pc/` to path |
| `-c` | path | chroot to path (root needed) |
| `-i` | isofile | CDFS redirection from this image |
| `-r` | — | reset the DC (while dcload is in control) |
| `-g` | — | GDB server on TCP `:2159` |
| `-l` | — | legacy 1024-byte payload (refused by the loader since 2026-09-28, §4.16) |
| `-f` | — | no FIFO delays: faster, more loss |
| `-h` | — | usage |

One of `-x`/`-u`/`-d`/`-r` per run; `-m` and `-c` are exclusive.
`prepare_comms()` sends `CMD_VERSION` with `(major<<16)|(minor<<8)|patch`,
detects legacy (31313) vs v2 (53535), and identifies the adapter.

### 7.3 Networking constants

UDP **53535** (v2), legacy **31313**. Payload 1452 B (1494 with headers); `-l`
forces 1024 B and caps a transfer at 11 MB. GDB stub TCP **:2159**.
`PACKET_TIMEOUT = 250000` µs. The delay knobs are chosen from the adapter.

## 8. Protocol reference

`commands.h` on both sides is canonical. The 4-byte `command_t` ID is the only
string; `address` and `size` are big-endian, followed by `size` bytes of data.
Target-side `command_t` is `packed, aligned(4)`; host-side only `packed`.

| ID | Name | Use |
| --- | --- | --- |
| `EXEC` | `CMD_EXECUTE` | execute |
| `LBIN` | `CMD_LOADBIN` | begin receiving binary |
| `PBIN` | `CMD_PARTBIN` | part of a binary |
| `DBIN` | `CMD_DONEBIN` | end of binary |
| `SBIN` / `SBIQ` | `CMD_SENDBIN` / `CMD_SENDBINQ` | read DC memory (quiet variant) |
| `VERS` | `CMD_VERSION` | version handshake |
| `RETV` | `CMD_RETVAL` | return value |
| `RBOT` | `CMD_REBOOT` | reboot |
| `MAPL` | — | host-to-DC Maple packet |
| `PMCR` | — | performance-counter control |
| `EXPT` | `CMD_EXCEPTION` | DC-to-host exception dump |

`syscalls.h` (both ends) defines `DC00`–`DC22` (exit, fstat, write, read, open,
close, creat, link, unlink, chdir, chmod, lseek, time, stat, utime, bad,
opendir, closedir, readdir, cdfsread, gdbpacket, rewinddir, cdfstoc), plus
`DD02` (legacy write) and:

- **`DC23` `CMD_CDDAREAD`** (DC→host): raw 2352-byte audio sectors. value0 =
  LBA, value1 = destination, value2 = bytes.
- **`MARK`** (host→DC, 2026-09-30, `cmd_mark`): `address` a 64 KB-aligned
  start, `size` a multiple of 64 KB (≤ 16 MB), bit 31 clear = **paint** (one
  word every 256 B, through P2, holding its P1 address `^ 0x5a3cc3a5`), set =
  **check** (each sampled line purged, then read through P2; a block stops at
  its first changed word). Reply `MARK` at the same address: `size` 0 after a
  paint, the bitmap's byte count after a check (bit i = block i changed), or
  `0xffffffff` if the range touches the loader's image, stack, `.hiram` or
  Maple page. The host paints before `EXEC` and checks a few blocks a second
  while the title runs (`marks.rs`, §16).
- **`DC24` `CMD_CDDAREAD_ADPCM`**: the same audio as 4-bit ADPCM, left block
  then right. value2 = frames (= bytes), **bit 31 = restart the encoder**.

Both are answered by LoadBinary/PartBinary into value1 and a **ReturnValue
whose `address` is the LBA served and whose `size` is the clock trim in ppm**
(§4.13). `cmd_retval()` latches `size` in `syscall_retsize`; every other
ReturnValue sends 0 there.

`DC25` `CMD_CONSOLE` (DC→host, 2026-10-02) is console text from a running
KOS title: value0 = fd (1 or 2), then the bytes. **It has no answer**; a lost
datagram is a lost line. Sent only under dcload-ip-rs (`g_gd_kos`).

`LBIN`/`PBIN`/`DBIN` also carry disc sectors to a running title (§4.5).

**Known bug, half fixed (measured 2026-08-20 with `dcload-ip-rs
selftest-readback`):** writing with `PBIN` to a **P2** address (`0xac…`) and
reading it back with `SBIQ` at the same P2 address returns, from byte 8 on, the
previous transfer's bytes. Either direction alone, and the physical window
`0x0c…`, are fine (32/32). The suspect is `SH4_aligned_memcpy` in
`cmd_partbin`/`cmd_sendbinq`. Hosts should address RAM through `0x0c…`.
**Probable mechanism (2026-09-26, not re-tested):** every copy in `memfuncs.c`
stores at `src + memdiff(dst, src)`, and `memdiff()` masks both addresses to
29 bits — so the stores go to the destination's physical address **in the
source's segment**. A P2 destination fed from a P1 packet buffer is written
cached, and `cmd_loadbin` did not purge a P2 destination. **That half is fixed
(2026-10-02, measured on the console):** GTA II reads its file headers by PIO
into P2 buffers (`0xac37xxxx`), and with its P1 in copy-back it read stale RAM
and hung on the loading screen right after the `GBST` header, every counter
clean. `cmd_partbin` now purges every RAM destination through the P1 alias the
copy stored into. The `SBIQ` half (a P2 source copied into a P1 packet buffer
through P2) is untouched and the selftest was not re-run. The same rule sends
a Windows CE virtual address to area 2 (`docs/wince-investigation.md` §7f):
**no `memfuncs.c` copy may target an address the title's MMU translates.**

**A `DBIN` names its range.** `cmd_donebin()` answers with the first missing
part of the LoadBinary window (or `0, 0` when complete); `cmd_sendbinq()` ends a
memory read with a `DBIN` carrying the address and size it served. Without that
distinction a counter read during a transfer swallowed the transfer's `DBIN`
and credited a read with holes as complete. Hosts match on the ID only, so only
the filter that needs it sees the fields.

**`MAPL` carries the loader's own argument block**, not a Maple frame: one byte
each of port (0-3), unit (0 = the controller, 1-5 = its sub-units), Maple
command and **payload length in LONGWORDS**, then that many longwords
(`cmd_maple`). The reply is a `MAPL` whose `size` is the number of bytes copied
out of the Maple receive buffer and whose data is the raw response frame --
response code, destination, source, length in longwords, then the data. A
response code is **signed**: -1 is "nothing at that address", -4 is "busy, ask
again" (retried 64 times inside `cmd_maple`, then handed to the host). dc-tool
never sends one; the Rust host does, for the VM2 game ID below.

**VM2 / VMUPro game ID.** A VM2, a VMUPro and the Maple adapters that answer
like them (`"VM2 by Dreamware"`, `"8BITMODS VMUPro "`, `"USB RP2040 EMU  "`,
`"Pico2Maple USBBT"`, in the 40-byte `extended` field an `ALLINFO` response
carries after the standard 112-byte device info) select a game's saves when
they are told the product number of the title that is starting. openMenu does
it from the console; here it is **entirely host-side** (`src/vm2.rs` in the Rust
host, §16), over `MAPL`: `ALLINFO` (command 2) to units 1 and 2 of each port to
find them, then **Maple command 33** with a payload of the memory-card function
code big-endian (`00 00 00 02`), 12 bytes of product number and optionally 128
bytes of title -- 4 or 36 longwords. Nothing was added to the loader for it,
because it needed none of the bytes §4.6 has left.

**Three defects made that passthrough unusable or unsafe, and were fixed on
2026-09-20 when the first payload longer than one longword appeared:**

1. `maple_docmd()` handed `datalen` -- LONGWORDS -- to `SH4_aligned_memcpy`,
   which counts BYTES, so three quarters of every payload stayed behind and a
   12-character ID arrived as 3. The line it replaced in 2025 had it right
   (`memcpy(sendbuf, data, datalen << 2)`, still there in a comment).
2. `cmd_maple()` read the response back through `to_p1(res)`. The Maple DMA
   writes that buffer and the operand cache does not snoop DMA, so the first
   read left clean lines resident and **every later `MAPL` returned the first
   one's response** -- four ports would all report the same device. The same
   applied in the other direction: the write-back covered the block holding the
   three control words, which are written through P2, so a resident line pushed
   the *previous* command's port and frame header back over them.

3. `cmd_maple()` sized the reply with `res[3]`, the device's own length field,
   read through a `char` -- which is **signed** on sh-elf. A device claiming 128
   or more longwords (the protocol allows 255) made the length negative, and
   `SH4_aligned_memcpy` takes it unsigned: a four-gigabyte copy out of a 1 KB
   buffer, from a byte the device chooses. Cast to `unsigned char`.

The first two are gone by moving the payload copy and the response read to P2.
**A loader older than this cannot serve a VM2**, and there is no feature bit to
test for -- the host chainloads its own loader for every disc image, so keeping
`loaders/` deployed is what keeps the two in step (§14.19).

**A cycle that writes nothing is not a device that says nothing.** Measured the
same day on a console with a VMUPro in port A: the session's first two `MAPL`
commands both answered `response 0, 255 longwords`, the loader served 1024
bytes of it, and every command after them worked. `maple_docmd()` never cleared
the receive buffer, so an untouched buffer was served as a Maple response --
stale RAM, indistinguishable from an answer. Three things now:

- the response header is stamped with `MAPLE_NO_REPLY` (`0xeeeeeeee`, a
  negative response code) before every cycle, so an untouched buffer is
  **nameable**, and distinct from the `-1` the controller itself writes when a
  device does not answer in time;
- a cycle whose sentinel survives is run again, up to `MAPLE_DMA_TRIES` (3),
  re-arming the DMA list pointer each time because the controller consumes it.
  This answers a measured condition, not a guessed cause; `g_maple_dma_empty`
  counts how often it was needed, so the cause stays visible;
- `maple_wait_dma()` is **bounded** (`MAPLE_DMA_SPIN_LIMIT`, `g_maple_dma_timeouts`).
  It span forever on a bit the Maple controller owns, inside the command loop:
  a device that wedged the bus took the loader with it, silently (§4.8).

**The busy bit is not a completion signal, and treating it as one damaged the
bus.** Reported on hardware 2026-09-20: after a scan the console could no
longer see its VMUs at all -- not from the BIOS, not from a game -- until the
controller was physically unplugged. `MAPLE(0x18)` is read immediately after
the trigger is written to it, so it may not be set yet; the wait then returns
at once with the buffer untouched, and the caller either reads stale RAM or
(worse, once the retry above existed) **starts a second Maple cycle on top of
the first**. KOS never faces this because it takes the DMA completion interrupt
and gates the next burst on `dma_in_progress` (`maple_irq.c`). This driver
polls, so it waits for the **answer** instead, which is a positive signal and
always arrives: when nothing is at the address the controller writes -1 itself
when its 50000-tick timeout expires (`MAPLE_ANSWER_SPIN_LIMIT`). A cycle is
only ever re-triggered from a controller that has been confirmed idle.

**The whole receive buffer is cleared before each cycle**, as KOS does in
`maple_frame_init()` (`memset(frame->recv_buf, 0, 1024)`). A device that answers
with a short frame leaves everything past it as it was, and the caller reads
that as part of the answer. Not with `memset_zeroes_64bit()`, which forces its
destination to P1: dirty cache lines over a buffer the DMA writes are the
defect above in reverse.

## 9. Example programs (`example-src`)

A template for DC programs using dcload syscalls: `crt0.S`,
`startup_support.c`, `dcload-syscall.s` (trampoline via `0x8c004008`),
`dcload-syscall.h`, `dcload-syscalls.c/.h` (POSIX-ish wrappers guarded on the
magic), `dc3.x` / `dc4.x`. Demos: `console-test.c` (file server),
`exception-test.c` (dump path), `gethostinfo.c`. Run with `dc-tool -x
console-test`.

## 10. CD / disc image recipes

Not part of the root `make`; `cd` into them.

- **`make-cd/`** — CD-R with `wodim` + `genisoimage`, `IP.BIN` via KOS `makeip`,
  recorder `dev=0,0,0`.
- **`make-cdi/`** — `mkdcdisc -n dctoolip -B 1st_read.bin -N -m` →
  `dctoolip.cdi`. `-B` takes an already-scrambled binary (ours is); `-b`
  would scramble it again.

## 11. Debugging and instruments

**Ports.** dcload's GDB stub: TCP `:2159` (`dc-tool -g -x prog.elf`,
`target remote :2159`). Emulator GDB: TCP `:3263` (`.gdbinit`,
`.vscode/launch.json`; `arch sh4`, `endian little` first). A faulting program
writes `dcload_exception_dump.bin` (`exception_struct_t`).

**flycast as target**: `docs/flycast-debug-loop.md` +
`scripts/flycast-debug-loop.sh`. Needs flycast built with
`-DENABLE_GDB_SERVER=ON`; it starts suspended (release with
`scripts/flycast-resume.py`). Anything touching `:3263` must `detach`, or the
guest stays halted. flycast answers only `Z0` breakpoints (no watchpoints),
and its SH4 UBC does not compare: trap writes host-side
(`dc-catch-vector-write.sh`) or patch flycast.

**On real hardware** the loader answers `SBIQ` from inside `bb->loop()`, which
is how every counter is read there. While a title runs, dcload looks at the
network only during GD waits, audio fetches and the CD-DA idle window.

| Script | What it answers |
| --- | --- |
| `dc-counters.py <ip>` | the counters on hardware, via `SBIQ`. Checks the base (from VERS) **and** 256 bytes of code against the ELF before decoding. For a host-relocated loader, produce the matching ELF with `dcload-ip-rs relocate loaders/dcload-relocatable.elf <base> -o <file>` and pass `--elf`. `--repeat N --interval S` prints deltas; `--raw addr len` dumps. Needs `sh-elf-nm` (source environ.sh). |
| `dc-peek.py <symbol\|addr>` | read guest memory under flycast; `--base`/`--elf` after a relocation. Never hard-code a counter address. |
| `dc-sample.py`, `dc-track.py` | poll counters; `dc-track` re-attaches per sample (a held GDB connection freezes the guest). |
| `dc-freeze.py`, `dc-pc.py` | snapshot during a freeze; where the guest executes. |
| `dc-screen.py <out.png>` | the displayed framebuffer (0.28 s a frame; `--scale`, `--repeat`, `--probe` for CRC/luma). Re-attaches per frame. |
| `dc-regs.py` | where a write was aimed (QACR, C2DSTAT, DMAC, TA pointers). `DAR2 = 0` is normal. |
| `dc-integrity.py` | did the title overwrite the loader (sections vs ELF or a baseline). |
| `dc-irqwatch.py`, `dc-trap.py` | interrupts; `Z0` breakpoint with a self-test (the BIOS ROM ignores flycast's patch). |
| `dc-catch-vector-write.sh` | host-side hardware watchpoint (`cdb.exe`) with a positive control: no `SELFTEST-OK` means no result. |
| `flycast-counters.sh` | reads the `dcdiag` flycast patch — **absent from the current flycast tree**; re-apply before use. |
| `sa-repeat.sh`, `sa-gdi-control.sh`, `flycast-transition.sh` | Sonic Adventure session drivers; `sa-gdi-control.sh` boots the GDI in flycast without dcload (§14.18). |
| `make-preset-db.py` | generates the host's `game-presets.tsv`. |

**Always-compiled counters** (outside CD-DA, which is §4.13):

- Footprint: `g_gd_sp_min`, `g_gd_sp_in_image` (§4.6).
- GD: `g_gd_idx_counts[]` (per syscall index; `[2]` ExecServer and `[4]`
  GetDrvStat run once per frame in Sonic Adventure, `[3]` InitSystem must be
  1 — a free check of the array alignment),
  `g_gd_cmd_counts[]` (per GD command, counted before the lock),
  `g_gd_park_longs`, `g_cdfs_sync_chunks`, `g_cdfs_sync_reentered`,
  `g_gd_spindown` (how the boot-time drive stop went, §4.14),
  `g_cdfs_read_retries`/`_fails`/`_holes`, `g_gd_in_transfer`, and the lock
  group (`g_gd_lock_stuck`/`_stuck_owner`/`_stuck_ticks`/`g_gd_lock_owner`/
  `_gen`). **`_fails` and `_holes` are different ends of the link**: fails means
  the host never answered, holes means it answered and the answer was short.
  Since the host stopped acknowledging on the sector path (§4.5, §16),
  `_holes` is the only place a lost packet in a disc read shows up.
- Transfers: `g_lbin_count`, `g_lbin_noecho`, `g_bin_data_done`,
  `g_dbin_count`, `g_dbin_incomplete`, `g_pbin_ok`/`_rejected`/`_clamped`,
  `g_last_load_addr`/`_size`, `g_last_reject_*`.
- RX: `g_rx_frames`, `g_rx_polls`, `g_rx_wraps`, `g_rx_overflow`, `g_rx_reinit`,
  `g_rx_linkchange`, `g_rx_link_giveup`, `g_rx_hdr_defer`, `g_rx_resync`,
  `g_rx_missed` (the chip's own drop tally), `g_rx_last_capr`/`_cbr`.
- Timeouts: `g_fine_timeouts` (TMU2 deadline exits) and `g_idle_polls_max` are
  **different exits** — reading the second as "no timeout fired" hid a 3 s
  freeze. `g_pmcr_backwards`.
- DHCP / warm start: `g_dhcp_replies`, `g_dhcp_not_ours`, `g_warm_start`,
  `g_warm_ip`.
- Maple: `g_maple_dma_empty` (cycles that wrote nothing and were re-run) and
  `g_maple_dma_timeouts` (the bounded DMA wait gave up) — §8.
- **No post-mortem block any more** (removed 2026-09-27). It sat at the fixed
  address `0x8cf0c000`, outside every loader placement, and `ReadSectors()`
  wrote nine words there -- two of them read-modify-write -- on every disc
  read, into whatever the title kept at that address. Nothing read it back.
  Under Windows CE that address is inside the driver globals
  (`0x8cef0000..0x8d000000`, §4.15). **The loader writes nothing outside its
  own footprint while a title runs**; an instrument that needs to must justify
  the address against the title (`GUEST_TICK_BLOCK` is the one such address,
  off by default).

Signatures worth knowing: `ExecServer` advancing one-for-one with
`g_cdfs_sync_reentered` = the server never comes back (§4.5); `g_rx_polls` per
`g_cdfs_sync_chunks` = how hard the loader spins per chunk, no clock needed.

**Checking the data, not the accounting.** Counters say bytes arrived, not that
they are right. The Rust host's read-back verification (`DCLOAD_VERIFY_READS=1`)
re-reads delivered ranges with `SBIQ` (`docs/read-back-verification.md`).

**Method.** An instrument inside the blast radius cannot report on the blast;
an instrument that costs a round trip changes the timing it measures; a null
result means nothing until the instrument has been shown to detect something
(positive control); observe before acting, because a fix that crashes destroys
its own measurement; and count on both sides of a boundary (a guest counter
against a capture on the wire) to tell lost TX from lost RX.

## 12. CI and branching

- **Primary CI is GitLab** (`.gitlab-ci.yml`): stage 1 clones KOS and tarballs
  `scramble`; stages 2–3 build in
  `segadreamcast/toolchains:binutils-2.34-gcc-4.7.4-newlib-2.0.0-gdb-9.1` and
  the gcc-9.3.0/newlib-3.3.0 image (`make && make install`).
- GitHub Actions only mirrors `master` to `gitlab.com/kallistios/dcload-ip.git`.
- Default branch `master`.

## 13. Cross-platform quirks

- **MinGW**: `MINGW` (original) / `MINGW64` (MSYS2); `WITH_BFD` forced to 1;
  `shim.c` for original MinGW only.
- **Cygwin**: `WINDOWS` + `CYGWIN`, like MinGW.
- **macOS**: `libelf` from `/opt/homebrew`, `-DMACOS`.
- **BSD**: `BSD := 1`; install `libelf` yourself.

## 14. Pitfalls

1. **Not sourcing KOS `environ.sh`** — "command not found".
2. **Building `1st_read` before `dcload`** — the `.incbin`s fail.
3. **Building `make-cd/` or `make-cdi/` from the root** — not in `SUBDIRS`.
4. **Editing a `*.asm` file** — it is a regenerated listing.
5. **A static IP without the DC speaking first** — §4.7.
6. **Changing a build variable without `make clean`.** The `%.o` rules depend
   on the Makefiles, not on variables given on the command line, so
   `make CDDA_ADPCM=0` after a default build only relinks old objects — a mixed
   binary that links fine. The same holds for `DREAMCAST_IP`. `make loaders`
   cleans by itself; anything else: `make clean` first, then check the result
   (`_end`, or `strings -a dcload | grep -E '^[0-9.]+$'` for the IP).
7. **Setting `VERSION` anywhere but `Makefile.cfg`.**
8. **Adding a command or syscall on one side only** — both `commands.h` and both
   `syscalls.h`, byte-identical layouts.
9. **Trusting a guard that no longer guards.** `scripts/check-resident-invariant.sh`
   checks a `_resident_end` split that no longer exists and
   `scripts/frontier_fuzz.c` fuzzes accounting removed with `ABIN`; both
   report success. The host's CD-DA trim tests passed on a geometry the console
   no longer had. Prove a check can fail before trusting it.
   **A timeout is a check too**, and the read deadline could not fire for two
   months because nothing started TMU2 unless a title played CD-DA (§4.5). It
   went unnoticed while the host repaired every loss on its own; the first
   session that relied on it froze the game for 7 s. `g_gd_lock_stuck` has
   "never fired" for the same reason — read that as unproven, not as healthy.
   **And a bound guards the loop it is in, not the function.**
   `receive_data()`'s first loop carries a carefully argued budget ("what keeps
   a silent console bounded"); the repair loop right below it had none, so a
   console answering nothing became an unbounded `SendBinQ` flood. This host is
   single-threaded, so while it spun there it served no disc reads at all and
   the title froze permanently — a worse outcome, from an instrument, than the
   fault it was measuring.
10. **Testing the old image.** `make` does not regenerate the CDI, and
    regenerating does not deploy it. Run `mkdcdisc`, copy, then prove the
    deployed image contains the new `1st_read.bin` **by content** (search for
    its first 64 bytes), never by timestamp or size.
11. **Hard-coding an address the linker owns.** A GD-vector tripwire compared
    against a literal; an unrelated change moved the target and the machine
    rebooted at `EXEC` with no trace. Compare against the symbol.
12. **Adding DC-side state without checking `_end`** — §4.6.
13. **Leaving an instrument on.** `GD_TRACE`/`GD_TRACE_CALLER` can stop a title
    booting. **And an instrument on a failure path is inside the blast
    radius.** The failed-chunk trace used to be `gd_trace_always`, so it fired
    exactly when the link had just failed to deliver something — and a
    `write()` is not one packet: it transmits, waits, and the host then fetches
    the written bytes back with `SendBinQ` (`fs.rs`, `download_data`). A disc
    read that could not be answered therefore produced an instrument read that
    could not be answered either, and on 2026-09-20 that pair wedged the host
    in an unbounded re-request loop and froze the title for good. It is
    `gd_trace` now; `g_cdfs_read_retries`/`_fails`/`_holes` say the same thing
    at no network cost.
14. **`REIOS: Booting up` in `flycast.log` is not a reset** — the SH4 executed
    address 0, e.g. a null jump. Check flycast's `[BBA-DIAG]` line in
    `Do_Exception` (verify the string is in the binary). **Unless `SYS_MISC 1`
    precedes it**, in which case it is the title itself asking the BIOS for the
    menu (Katana `syBtExit()`), and the disc boots again — after which every
    counter read comes from *that* build (§14.19: a rebooted CD loader answered
    `g_gd_sp_min` with `_global_bg_color`, and the host reported the title's
    stack inside the loader).
15. **Dividing a `long long`** (or by a variable) — `__udivdi3` and friends add
    ~840 B, `__udivsi3_i4i` ~1060 B, silently. Use `PMCR_Delta_Seconds()` or
    32-bit/constant divisors; `grep libgcc target-src/dcload/dcload.map`.
16. **A large buffer in BSS** — use `HIRAM_BUF`. `.hiram` is zeroed by crt0 but
    not by 1st_read's zero-fill, and a title's allocator could claim it.
17. **Chainloading a loader over the running one's packet buffers** — §4.11
    item 4. The transfer succeeds and the new loader is deaf.
    `loaders::live_footprint()` on the host knows the ranges.
18. **Believing a black screen is yours.** Sonic Adventure renders a near-black
    frame while running normally, identical with no dcload at all
    (`scripts/sa-gdi-control.sh`). Run that control first.
19. **Reading counters against a different build.** Two builds at the same base
    put the same counter at different addresses, and every value reads back
    believable and wrong. Deployed loader sets and `target-src/dcload/loaders/`
    are separate artefacts (`make loaders` does not deploy). `dc-counters.py`
    compares code bytes for this reason. Likewise, compare the md5 of two A/B
    sets before testing: identical md5s have caught a flag that never reached
    the compiler, twice.
20. **Servicing from an interrupt without an owner test.** A CD-DA fetch
    transmits, and an interrupt can land while `pkt_buf` or `bin_info` is in
    use (§4.5). The hook's tick (§4.15) services only while the GD lock is
    free, which every network use of the GD path holds. It also re-verifies
    the title's VBR on every GD syscall (isoldr's `exception_vbr_ok()`), and
    patches only Windows CE's entry: Sonic Adventure writes its own `+0x600`.
21. **Reading a hardware field by the name in the comment.** AICA TL is
    attenuation; writing it as a volume silenced CD-DA while every counter was
    healthy. Check fields against a working driver (KOS `arm/aica.c`).
22. **Assuming an interrupt you armed is the one that fires.** A title's own
    masks may carry the same event on a higher level: the end of our BBA DMA
    came as the title's IML4, not our IML2 (§4.16). Read the title's masks
    (`g_irq_iml`) before choosing where to listen, and count the event at any
    level before concluding it does not fire.

## 15. Where to look first

- **Wire protocol** → `host-src/tool/commands.h`, `host-src/tool/syscalls.h`,
  `target-src/dcload/commands.{c,h}`, `target-src/dcload/syscalls.{c,h}`.
- **Throughput and latency** → `Makefile.cfg` FIFO delays, `GD_EMU_ASYNC`, and
  the host's pacing (§16). A disc read blocks the title for its whole duration,
  so milliseconds per chunk matter more than KB/s. Upload loss was congestion,
  proven by `RT_RXMISSED` (887 drops in one upload before windowed flow
  control).
- **A title that stutters while it runs** → the per-chunk cost, not the total
  bandwidth. A title that streams reads *during* gameplay pays that cost inside
  a frame it has already half spent, so what the player feels is a block of
  dropped frames, not a slowdown (§16, Crazy Taxi). Measure it from the host's
  `debug!` timestamps for consecutive `ReadSector` requests — they are the
  chunks of one read — and against `g_cdfs_sync_chunks`; then decide between
  the fixed per-chunk cost and the schedule. **`--diag` is inside what it
  measures**: its `SBIQ` reads are answered from within the read's own wait, and
  they show up as `Received non-DBIN packets while waiting for DoneBinary
  response`, as `g_dbin_incomplete`, and once as a 521 ms stall. Take one run
  without it.
- **Base address / memory map** → `target-src/dcload/Makefile` (the layout
  table) first, then `dcload.x.in`, `target-src/1st_read/`, `dcload-crt0.s`,
  `go.S`, `exception.S`, `commands.c` (`_dcload_base`), `maple.c`, `hiram.h`,
  and the host's `src/loaders.rs`. **Do not add a literal.**
- **GD read path** → headers of `cdfs_syscalls.c` and `cdfs_redir.s`, then
  `commands.c` (`cmd_partbin`/`cmd_donebin`, `bin_info`).
- **CD-DA** → the header of `cdda.c`, then §4.13.
- **Anything that runs while a game runs** → `_end` and §4.6.
- **A new example program** → copy one, add it to `example-src/Makefile`.
- **A new on-screen field** → `video.s`, `dcload.c`.
- **A new Maple or PMCR command** → README only; `dc-tool` forwards verbatim.
- **A new KOS** → check `utils/scramble` and the CI image tags.

## 16. The Rust host server

`dcload-ip-rs`, at `/mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/`, serves
GD-ROM and CD-DA emulation (mainly `src/dispatch.rs`) and has **its own
`AGENTS.md`**: read it for anything about the host. `dc-tool-ip` remains the
reference for the basic protocol. **Never run `cargo` in the user's `target/`**
(it breaks their Windows build); use `CARGO_TARGET_DIR=<scratchpad>`.

What it does that concerns the DC side:

- **Chooses the loader base** before uploading a title: identifies the disc
  (IP.BIN md5, then title), looks up DreamShell's preset
  (`game-presets.tsv`), and rejects a base the title names in its code
  (`literals_in_loader_footprint`), writes into (`game-memory.tsv`, a 64 KB
  bitmap learned from served reads, merged by OR), whose stack comes too
  close (`g_gd_sp_min` vs `_end`, `LOW_BASE_MIN_MARGIN` 4096) or **whose image
  is inside a range the title fills from constant bounds**
  (`constant_range_fills` — the Katana `"SEGA"` paint of `0x8c00c000`–
  `0x8c00f400`, §4.6; it rules out the low family for every retail title with a
  CD-DA build). Unsupported low presets go to the biggest free hole; a title the
  database does not know is searched from `0x8ce00000`.
  `dcload-ip-rs identify <image>` prints the decision offline, on a `startup`
  line beside the `stack` one.
- **Learns what a title writes** (`src/marks.rs`, 2026-09-30): before `EXEC`
  it has the loader paint every 64 KB block above the title's image that is
  neither known used nor the loader's (`MARK`, §8), then checks them, 16
  blocks a second; a changed block goes into `game-memory.tsv` like a read.
  Not for Windows CE titles; `--no-marks` turns it off. The loader's own span
  is never observed: a write there is learned by the next session, with the
  loader elsewhere. Found because Shenmue II overwrote a loader at 0x8cfd0000
  in a block no read had touched.
- **Two placement rules of 2026-09-30.** A region the title names (a
  64 KB-aligned word anywhere in its image, `dispatch::region_starts`) that
  starts inside a high loader's span or less than one block under it rules
  that base out (`region_reaching_loader`: Shenmue II's 0x8cfc0000 under
  0x8cfd0000). And a high preset refused by the constant scan is taken as a
  **window**: the loader goes against the lowest address the title names above
  the preset (`preset_window_base`) -- 0x8cfe0000 for Shenmue II and Crazy
  Taxi, under their Maple DMA list at 0x8cff0000.
- **Relocates and chainloads** `loaders/dcload-relocatable.elf` (§4.11), always
  via `SCRATCH_BASE` `0x8ce00000`, and replaces a running loader when its image
  differs from the file even at the same base.
- **Patches titles** before upload: the GAPS guard (§4.12), `--vga` (forces the
  PDTRA cable read; `auto` uses the cable dcload reports), PPF patches
  (`patches/`, checked against the PPF blockcheck).
- **KOS binaries** (found by KOS's dcload probe words, `is_kos_binary`,
  2026-10-02): `g_gd_kos` is set under a disc image, and the dcload magic at
  `0x8c004004` is then pointed at the live loader (its `base+8` syscall
  pointer), so KOS's console (`dbgio` `fs_dcload`) and `/pc` reach the host --
  as it is without a disc image. A Katana title, or a KOS one the loader could
  not be told about, gets the magic cleared. **Safe only because `g_gd_kos`
  masks interrupts across every adapter wait** (`bb_irq_hold()`, as under the
  MMU): KOS is preemptive, and a thread's printf entering the write syscall in
  the middle of another thread's disc read would share `pkt_buf` and take the
  read's ReturnValue; KOS masks its own dcload syscalls. **Console writes
  (fd 1 and 2) then go as `DC25`** (`CMD_CONSOLE`, `console_write()` in
  `syscalls.c`): the text in the datagram, no SendBinQ, no ReturnValue, no
  wait. The acknowledged `DC02` stopped KOS for good after its first line on
  the console -- its wait has no deadline, so one lost frame is a hung title.
  Gated on `g_gd_kos`, which only this host sets: dc-tool-ip does not know
  `DC25`. And the
  title's top of RAM is lowered under the loader (`kos_mem_top_patches`:
  `arch_stack_16m`/`_32m`, which give `_arch_mem_top`; KOS's heap otherwise
  grows to `0x8cff0000`, through a high loader and a low one's buffers).
- **`--diag`**: the counter panel (`d` toggles, `w` writes `dcload-diag.txt`;
  the header shows the measured sample interval, not the requested one).
  `stackwatch` reads `g_gd_sp_min` every 10 s in every session.
- **Tells a VM2/VMUPro which game is starting** (`src/vm2.rs`), over `MAPL`,
  in the idle seconds before `EXEC` and with a disc image only -- the product
  number comes from IP.BIN. `--no-vm2` turns it off. It costs the loader
  nothing, and §8 has the protocol and the `MAPL` defects it uncovered.
  **It enumerates the way KOS does**: `DEVINFO` to a port's unit 0, then the
  occupied slots out of that answer's **sender byte** (bit 5 = the port's own
  peripheral, bits 0..4 = one per slot), then only those units. A slot the
  controller does not report is never addressed and a silent port is left
  alone -- on a console with two controllers, probing all eight slots blind
  was twelve transactions aimed at nothing.

**Contracts the DC side must keep in step with:**

- `MAX_XFER = 256 × CHUNK_SIZE` ↔ `BIN_INFO_MAP_SIZE` 256 (`commands.c`).
- **VERS**: after the version string's NUL, 4-byte fields read *forward*: the
  linked base, then the cable (0 VGA, 2 RGB, 3 composite). Append new fields at
  the end; older loaders send fewer.
- **Layout**: `loaders::live_footprint()`, `HIRAM_RESERVED` (`0x3000`),
  `LOADER_SPAN` (`0x10000`) mirror the Makefile layout table; the relocator
  assumes only `R_SH_DIR32`, linker-symbol addresses, and `.guestvbr`
  references only through the jump table (§4.11). It repeats the link's two
  stack ASSERTs: every image section (`.gdstage` included) under `_stack`,
  and 800 B from the **`_end` symbol** -- until 2026-09-27 it measured those
  800 B from the top of `.gdstage`, and refused a build that linked.
- **Counter names** are read from the ELF by name (`src/diag.rs`,
  `src/stackwatch.rs`, `scripts/dc-counters.py`); renaming one breaks them.
  So are **`_gd_stage`** and **`_gd_stage_big`** (the ranges a disc read may
  land on inside the loader, `loader_stage` in `main.rs`),
  **`_g_gd_kos`** (`cdfs_syscalls.c`), which the host sets to 1 before `EXEC`
  for a KOS binary (found by KOS's dcload probe words, `is_kos_binary`): a
  read then starts without the first yield, as isoldr does for KOS -- KOS
  sleeps on PROCESSING until the G1 DMA-end interrupt or its vblank handler --
  and a one-sector read takes the retrying path (KOS never retries: one lost
  request was EIO and an assertion in the GTA III port);
  and **`_gd_bios_entry`** (`cdfs_redir.s`), the address the host writes
  over a title's direct calls to the BIOS GD driver (§4.5); a loader without it
  leaves those calls on the real drive, and the host says so.
- Disc reads are served **one at a time**, to completion, inside the request
  handler, by `send_sectors()` — **no LoadBinary echo, no DoneBinary probe**
  (§4.5). Everything else still uses `send_data()`, which verifies the echo and
  whose recovery resends a run from the address `DoneBinary` reports, doubling
  up to 64 parts (resending a part is harmless); `send_sectors` falls back to it
  above `MAX_XFER`, since the loader's check speaks for one window only.
  **This is a two-sided contract with no feature bit**: a loader that does not
  test `bin_window_complete()` accepts the ReturnValue as proof and would hand
  the title short data in silence. What keeps them in step is that the host
  chainloads its own loader from `loaders/` for every disc image, so that
  directory is redeployed with any change here (§14.19).
  **Its LoadBinary must name exactly the destination the request gave**: the
  loader refuses any other one while a read waits (§4.5, the disc-read door).
- **`send_sectors` pauses once after the LoadBinary, before the first part.**
  `cmd_loadbin` zeroes the part map and purges the cache over the whole
  destination range — 512 cache blocks for a 16 KB chunk — and waiting for the
  echo used to cover that work by accident. That is the one thing the echo did
  for the *loader* rather than for the host. Dropped without replacing it, the
  first parts arrive while the loader is still purging; the ring overflows, and
  an overflow does not cost one part but the **whole answer** (§4.8 rule 5
  drains or re-initialises). Measured on Crazy Taxi, 2026-09-20: reads that
  completed only after 7.0 s, which was the loader waiting out a deadline for
  an answer that no longer existed.
- **Measured 2026-09-20, Crazy Taxi**, which streams both its music (73 sectors
  from a sequential LBA every ~3.5 s) and its city (3–8 sectors every ~60 ms)
  **while rendering**, unlike Sonic Adventure which reads on loading screens.
  A 16 KB chunk froze the title for ~5 ms: 1.3 ms of wire, 1.8 ms of deliberate
  pause, ~1.9 ms for the two acknowledgement round trips. Ten chunks is a 58 ms
  stall, several times a second — ~77 ms of frozen title per second, delivered
  in blocks of 18 to 58 ms, which is what a player feels as micro-freezes
  rather than as slowdown. The pause and the round trips were 74 % of it, and
  every loss counter (`g_rx_missed`, `g_rx_overflow`, `g_pbin_rejected`,
  `g_cdfs_read_retries`) was 0 across 4000 chunks — the acknowledgements were
  protecting against nothing on that link.
- **`runtime_pacing()` is 6 packets / 600 µs** (was 10 / 1800, which for a
  12-packet chunk meant exactly one 1.8 ms pause). Six packets is ~9 KB in
  front of a 16 KB ring, so it is the safer of the two as well as the cheaper.
  It is not zero because outrunning the ring does not drop a frame, it desyncs
  CAPR from CBR and the loader receives nothing further — a wedge that does not
  recover. `DCLOAD_RT_BURST` / `DCLOAD_RT_DELAY_US` tune it without a rebuild.
  **Do not shorten it further**, measured 2026-09-20 over 2302 chunks:
  `g_rx_missed` 0 (the chip never dropped a frame for want of space, so the
  pacing is adequate) but `g_rx_overflow` 3 and `g_rx_resync` 2 — the ring is
  at its back-pressure limit already.
- **What the residual loss actually is, and it is not congestion.** That same
  session: 2 failed chunks in 2302 (0.09 %), `g_cdfs_read_holes` **0** — no
  chunk has ever arrived short, so what the title is handed is right — and
  `g_cdfs_read_fails` 2 = `g_fine_timeouts` 2, i.e. both were the 250 ms
  deadline with nothing arriving at all, and both retries then succeeded. They
  line up with `g_rx_resync` 2: a resync is the status-word race of §4.8
  rule 4, and it **discards the queue**, taking the answer with it. A shorter
  pause cannot buy that off. What would is ending the wait the moment a resync
  happens under `g_gd_in_transfer`, instead of sitting out the deadline —
  250 ms down to ~2 ms — but that is the RX path, which is where the
  unrecoverable wedges live, for two events per session.
- **No lazy work on a syscall path.** A zipped track's deflate index takes
  ~130 ms per 17 MiB to build, and until 2026-09-13 the audio tracks' were
  built inside the first CD-DA read of each track, which put a hole in the
  music (§4.13). `zip::warm_track_indexes` now builds them on a thread as soon
  as the `.gdi` is opened, filling the same process-wide index cache the read
  path consults; `Gdi::audio_track_files()` says which tracks, and the data
  tracks are left alone because identifying the disc indexes them anyway.
- **Audio** (`send_audio`) skips the echo and DoneBinary round trips (they were
  2.4 ms of frozen title per fetch), sends the ReturnValue with `address` = LBA
  and `size` = trim ppm, drops any answer that took ≥ `CDDA_GIVE_UP` (15 ms) to
  produce — **must stay below the loader's 20 ms deadline** — and probes for
  loss once every 256 fetches. The ADPCM encoder answers re-asks from its last
  `RECENT` = 48 requests (`lost_replay` must stay 0) **before reading the disc
  again** (`Stream::replay`), and never emits a nibble
  that decodes differently with and without a clamp on its contribution
  (`pick_nibble`, §4.13 Status).
- **The trim estimator's thresholds** (`CDDA_TRIM_SEG_S` 2 s,
  `CDDA_TRIM_GAP_MAX` 1.0 s, `CDDA_TRIM_SEG_SANE` 20 %, window sanity 3 %,
  result gate 1.5 %) assume a **paced** stream, one sub-fetch every ~53 ms
  (§4.13 rule 10). Until 2026-09-20 they looked for the idle tail between two
  halves, which a paced loader never produces: the estimator would have banked
  nothing and never trimmed.
- **Pacing on the runtime path is a spin, never `thread::sleep`**: dcload
  answers a read synchronously, so every host-side wait freezes the title.
  Windows rounds sleeps up to the timer tick; removing three sleeps per chunk
  took Sonic Adventure from 45.6 ms to 1.5 ms of freeze per 16 KB chunk.
- CD-DA reads are excluded from `game-memory.tsv` (they land in the loader's
  staging buffer).
- **`MAPL`'s argument block** (port, unit, command, length in LONGWORDS) and the
  signed response code are the interface `src/vm2.rs` is written against; the
  reply's `size` is how many bytes `cmd_maple` copied, and is the only length to
  trust.

## 17. Sources of truth

Code first. Then:

| File | For |
| --- | --- |
| `Makefile.cfg`, `Makefile.hostdetect` | toolchain, tunables, version, host flags |
| `target-src/dcload/Makefile` | build flags, base address and layout |
| `target-src/dcload/dcload.x.in` + `dcload.map` | memory map and `_end` |
| `target-src/dcload/cdda.c` (header) | CD-DA design |
| `target-src/dcload/cdfs_syscalls.c`, `cdfs_redir.s` | GD emulation |
| `target-src/dcload/go.S` | the handoff |
| `target-src/1st_read/loader.s` | bootstrap |
| `host-src/tool/dc-tool.c` | CLI and protocol behaviour |
| `README.md`, `CHANGES` | user-facing behaviour (versions lag) |
| `Cdif131e.txt` | SPI / GD-ROM command spec |

`docs/` — history and references, not descriptions of the tree:

| Document | For |
| --- | --- |
| `loader-comparison.md` | isoldr vs dc-virtcd vs dcload on the same problems. Consult before inventing a mechanism; its dcload column predates the current tree. |
| `sonic-adventure-investigation.md` | 2026-08-07→10: every cause eliminated, and the footprint root cause. |
| `cdda-crackle-investigation.md` | 2026-08-29→09-05: the first CD-DA engine (integrator). Transport and AICA measurements still hold. |
| `cdda-double-buffer-investigation.md` | 2026-09-06→09-12: the current engine brought up on hardware, with a table of what was later superseded. |
| `wince-investigation.md` | 2026-09-20→27: Sega Rally 2 from black screen to playable -- the three GD doors, virtual buffers, streams, CE's RAM, preemption inside a network exchange, CD-DA under CE, and what the interrupt hook must solve next. |
| `g2-dma-investigation.md` | 2026-09-28/29: G2 DMA for the CD-DA rings and the BBA's RX; the bench, each console measurement and why the RX wake-up failed until the IML4 finding (§4.16). |
| `flycast-debug-loop.md` | the emulator-as-target workflow. |
| `read-back-verification.md` | what read-back verification proves and does not. |
| `dreamshell-presets/` | 6168 archived DreamShell presets. |
| `game-presets.tsv` | presets folded one row per game (generated by `scripts/make-preset-db.py`; the host reads its copy in its own project root, beside `game-memory.tsv`). |
