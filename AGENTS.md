# dcload-ip — Agent Notes

> The operating manual for AI coding agents working in this repository. It
> describes **the code that is present**; a measurement is kept only where it
> justifies a non-obvious choice. Investigation logs live in `docs/` (§17); the
> long narrative this file carried until 2026-10-03 is in its git history.
>
> If this document conflicts with the code, the code wins. Section numbers are
> cited from source comments and from the Rust host ("AGENTS.md 4.6"), so keep
> them stable, including the numbering of §14.

## 1. What this project is

`dcload-ip` is a **Sega Dreamcast network loader**. Two halves talk over UDP:

1. A **Dreamcast-side** program (`dcload`, at `0x8c004000` by default, copied
   there by the 1st_read bootstrap — §5). It implements ARP, ICMP, UDP and an
   optional DHCP client on the BBA (`HIT-0400`) or LAN Adapter (`HIT-0300`),
   and serves commands to upload (`LBIN`/`PBIN`/`DBIN`), execute (`EXEC`),
   read memory (`SBIN`/`SBIQ`), proxy GDB, pass Maple packets (`MAPL`). For a
   launched title it also **emulates the GD-ROM drive** (§4.5), including
   **CD-DA playback** (§4.13), serving sectors from a disc image on the PC.
2. A **PC-side** program. `dc-tool-ip` (C, in this repo) is the reference
   tool. GD-ROM and CD-DA emulation is served by a separate Rust host,
   `dcload-ip-rs` (§16).

Fork of KallistiOS' `dcload-ip`, overhauled by Moopthehedgehog, maintained by
Mickaël Cardoso (SiZiOUS) and contributors. License **GPLv2** (`COPYING`).

Version `2.0.4`, set in `Makefile.cfg` **only** (both Makefiles pass it as
`-DDCLOAD_VERSION`); keep `README.md` and `CHANGES` in step with it.

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
- `DREAMCAST_IP = 0.0.0.0` — `0.x.x.x` selects DHCP; a static address needs
  `announce_presence()` (§4.7). `scripts/flycast-debug-loop.sh` builds with
  `DREAMCAST_IP=192.168.1.130` and records it in `target-src/dcload/.built-ip`.
  **`make clean` after changing it** (§14.6). **A loader set must be built with
  the same IP as the CD image**: a chainloaded loader inherits the IP only
  through the BBA's SRAM (§4.9), which flycast does not have.

## 4. The Dreamcast binary (`target-src/dcload`)

### 4.1 Artifacts

- `dcload` — ELF linked with `dcload.x`; `dcload.bin` ~36 KB with defaults.
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
| `packet.c/.h`, `bswap.h` | packet builder/parser, byte order, the Internet checksum (`csum_sum16()`). |
| `net.c/.h` | ARP/ICMP/UDP glue, `announce_presence()`. |
| `adapter.c/.h` | adapter interface (`bb`), the loader's clock (`clk_now()`/`clk_since()`, §4.5) and the fine deadline, `bb_irq_hold()`. |
| `hiram.h` | `HIRAM_BUF`: put a large buffer in `.hiram` instead of BSS. |
| `rtl8139.c/.h` | BBA driver, RX ring (§4.8), warm start (§4.9), RX by G2 DMA (§4.16). |
| `lan_adapter.c/.h` | LAN Adapter driver (`WITH_LAN_ADAPTER`). |
| `dhcp.c/.h` | DHCP client (§4.10). |
| `perfctr.c/.h` | SH4 performance counters; counter 1 times the DHCP lease and adapter timeouts. |
| `memfuncs.c/.h`, `memcpy.S`, `memcmp.c` | aligned mem* fast paths (§8 on segments). |
| `maple.c/.h` | Maple bus driver; its DMA buffer is outside the image (§4.4). |
| `cdda.c/.h` | CD-DA playback (§4.13). The header of `cdda.c` is the design description. |
| `g2dma.c/.h` | the four G2 DMA channels by polling, `g2dma_quiesce()`, `g2dma_hold()`, `g2dma_pick()` (§4.16). Built with `WITH_CDDA`. |
| `g2bench.c` | `G2DMA_BENCH=1` only (§4.16). |
| `cdfs.h`, `cdfs_redir.s`, `cdfs_syscalls.c` | GD-ROM emulation (§4.5), asynchronous reads, the boot-time drive spin-down (§4.14). Both carry explanatory headers. |
| `syscalls.c/.h` | host syscalls `DC00`–`DC25` (§8). |
| `commands.c/.h` | the command dispatcher and the LoadBinary window (`bin_info`). |
| `exception.S` | exception display **and** the VBR table handed to the title. |
| `irq.c/.h`, `irq_hook.S` | the interrupt hook in a title's own vector table (§4.15, `WITH_IRQ_HOOK`). |

### 4.3 Build flags

All in `target-src/dcload/Makefile`, which triggers a rebuild when edited.
GD constants are in §4.5, CD-DA flags in §4.13. Several defaults differ between
the **LOW** family (the CD build) and the **HIGH** family (the set the host
chainloads, §4.11), because the LOW build is short of bytes and the host always
replaces it before a disc image.

**Feature and size knobs.** Footprint is correctness (§4.6). Costs are
approximate changes of `_end`, measured when each flag was introduced.

| Flag | Default | Effect | Cost |
| --- | --- | --- | --- |
| `PKT_BUFS_IN_HIRAM` | `1` | packet buffers and the CD-DA staging buffer in `.hiram` instead of BSS | saves 5440 B |
| `WITH_CDDA` | `1` | CD-DA engine (§4.13) and `g2dma.c` | ~5.2 KB |
| `WITH_LAN_ADAPTER` | `1` | HIT-0300 driver; 0 = BBA only | ~2 KB |
| `WITH_MAPLE` | `1` | serve `MAPL` | ~650 B |
| `WITH_PMCR_CMD` | `0` | serve `PMCR` (`perfctr.c` stays); no host sends it | ~800 B |
| `WITH_GD_SPINDOWN` | `0` | stop the real drive at boot (§4.14) | 160 B |
| `WITH_IRQ_HOOK` | `1` | the interrupt hook (§4.15) | ~800 B |
| `GD_ASYNC_KATANA` | `1` | the hook in Katana titles too, and their disc reads through the asynchronous engine (§4.5); needs `WITH_IRQ_HOOK` | ~150 B |
| `GA_KATANA_READS` | `1` | with `GD_ASYNC_KATANA`, 0 keeps the hook (CD-DA from the tick, idle listen) but serves Katana reads synchronously | -- |
| `WITH_MARK_CMD` | `1` HIGH, `0` LOW | serve `MARK` (§8) | 432 B |
| `IRQ_IDLE_LISTEN` | `1` HIGH, `0` LOW | the hook's tick listens to the network (≤ 1 ms every 50 ms) when nothing else owns it, so `--diag`, the stack watch and `MARK` are answered while a title does not read its disc | ~220 B |
| `RTL_RX_DMA` | `1` HIGH, `0` LOW | the BBA's RX frames by G2 DMA from the tick (§4.16); 0 = every frame by CPU | ~150 B |
| `GD_STAGE_BIG_SECTORS` | `4` HIGH, `3` LOW, `2` with the bench | size of `.gdstage` (§4.15) | 2 KB a sector |
| `DCLOAD_GC_SECTIONS` | `1` | `--gc-sections`; also reveals dead code | saves 328 B |
| `DCLOAD_LTO` | `0` | `-flto`. **Off on purpose**: it changes the depth of the C frame the GD coroutine parks (§4.5 — check `g_gd_park_longs` on hardware) and suppresses the `*.asm` listings | saves ~2.4 KB |

**Diagnostic and experimental flags** (all default 0):

| Flag | What it does |
| --- | --- |
| `GD_TRACE` | Trace GD requests to the host console. Each event is a UDP round trip, which perturbs timing enough to stop a title booting. |
| `GD_SERVICE_EVERY_SYSCALL` | Poll the network at the top of ReqCmd/GetCmdStat/GetDrvStat. **Sonic Adventure dies with it** (mechanism unknown). |
| `GUEST_TICK` (+`_PERIOD`, `_WATCHDOG`, `_BLOCK`) | TMU0 sampler of the guest PC, block at `0x8cff8000` (survives a reset + CD reboot); read with `scripts/dc-ticks.py`. |
| `ISOLDR_HANDOFF` = `ISOLDR_SR` + `ISOLDR_REGS` + `ISOLDR_SETUP_MACHINE` | isoldr's machine state at launch. **`ISOLDR_REGS` breaks Sonic Adventure**; the other two were never judged alone. |
| `GUEST_CACHES_ON` | Hand the title CCR `0x0909` instead of `0x0808`. |
| `GUEST_IRQ_MASKED` | Hand SR with IMASK=15. Diagnostic only (go.S). |
| `DCLOAD_CLEAR_IPBIN`, `DCLOAD_ZERO_GAME_RAM` | Clear the IP.BIN region / the title's RAM at start-up, as a real boot would. Faithfulness only. |
| `G2DMA_BENCH` | At boot, measure which G2 DMA channels reach the BBA's SRAM and the AICA, with a torture round; results in `g_bench[]` and on screen. ~1.2 KB: never in a shipped set. |
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
| `0x8c00d638` | `_end` with default flags (2026-10-03). Code and BSS are all below it. Over the `0x8c00c000` bound (§4.6). |
| `_end`..`_stack` | the loader's stack; also `.gdstage` (NOLOAD, `GD_STAGE_BIG_SECTORS` × 2 KB), used only under a Windows CE title (§4.15). |
| `0x8c00f400` | `_stack` (LOW layout), **the VBR handed to the title**, the link address of `exception`, and the BIOS VBR. Only `_stack` moves with the base. |
| `0x8c010000` | the title's load address; `exception.bin` ends just before it. |
| `0x8cfe8000` | Maple DMA buffer (2 KB), outside the image (`_maple_dma_buffer`). |
| `0x8cfe9000` | `.hiram`, 12 KB reserved (NOLOAD, zeroed by crt0): packet buffers, CD-DA staging buffer, `gd_stage`, `ga_xpa[]`. |

Link-time asserts (`dcload.x.in`): `.hiram` ends at or under the Maple buffer,
`(_stack - _end) > 800`, `.gdstage` ends at or under `_stack`, `_end` within
`ram`.

The HIGH layout is in §4.11.

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
- **Synchronous reads** (`data_transfer_emu_async`) go in `GD_EMU_ASYNC`-sector
  chunks, one host round trip each, **without yielding to the title between
  chunks** (only before retrying a failed one), feeding CD-DA between chunks
  (`GD_CDDA_BETWEEN_CHUNKS`).
- **Asynchronous reads** (`data_transfer_async()`, the `ga_*` functions): the
  read is posted, the title runs, and **the hook's tick** (§4.15) collects each
  chunk and posts the next. Used under Windows CE with the hook in, and under
  Katana titles with `GD_ASYNC_KATANA` (BBA only), for reads of more than one
  sector. With the MMU off the destination is physical and the host writes it
  directly, `GA_PHYS_SECTORS` (6) a chunk — what the RX ring holds undrained;
  a virtual destination is staged and copied (§4.15). A chunk is done with its
  ReturnValue, not merely a whole window. Counters: `g_ga_posts`,
  `g_ga_irq_done` (chunks the tick finished), `g_ga_xlat_miss`, `g_ga_sync`,
  `g_ga_wakes`.
- **KOS titles** (`g_gd_kos`, set by the host before `EXEC`, §16): a read
  starts without the first yield, as isoldr does for KOS (KOS sleeps on
  PROCESSING until its G1 DMA-end or vblank handler), and a one-sector read
  takes the retrying path (KOS never retries: one failure is EIO).
- `gdGdcGetCmdStat` reports progress (`COMPLETED` consumed once, then `IDLE`;
  `req_count` never 0 or 1). `gdGdcGetDrvStat` reports PLAYING while a read or
  CD-DA is live and calls `cdda_service()` **before** taking the GD lock.
- `CMD_REQ_STAT` and `CMD_GETSCD` report the CD-DA position while music plays.
  Unmodelled commands are force-completed, never failed (a title that gets
  FAILED for a routine command tends to give up).
- **Streams** (`data_stream()`): PIO pieces are chained through the title's
  `SetPioCallback` callback, called from the server. **DMA pieces after the
  first need the G1 DMA-end interrupt, which nothing raises**: a DMA stream
  completes only if its first piece is the whole of it (Windows CE is steered
  to PIO by the host, §4.15). `GETTOC2` does not model low/high density areas.

**Judging a chunk.** The host sends the LoadBinary, the parts and the
ReturnValue without waiting for an echo or a DoneBinary probe (`send_sectors`,
§16), so the loader decides:

1. `ReadSectors` calls `bin_window_close()` **before** building the request, so
   completion is judged on this chunk's own window, and tests
   `bin_window_complete()` when the ReturnValue arrives. A hole fails the chunk
   (`g_cdfs_read_holes`) and it is asked again. **A short chunk must never be
   reported COMPLETED**; the title would run the bytes.
2. **The door** (`g_bin_read_want`): while a read waits, `cmd_loadbin` refuses a
   LoadBinary for any other destination without touching the window
   (`g_gd_stale_lbin`). A late answer to an earlier read would otherwise replace
   the window, fill it and complete the waiting read with nothing delivered.
3. **The ReturnValue names the read** (`g_retval_want`): the host sends
   `0x40000000 | LBA` (`GD_READ_TAG`, `commands.h`) as its address.
   `cmd_retval()` takes one naming another LBA for nothing — closes the window
   it filled, does not end the wait — because translated reads all land in
   `gd_stage_big` and KOS reads into the same cache blocks again, so the door
   alone cannot tell two answers apart. 0 (an older host) is accepted.
4. A ReturnValue over a hole may be a late answer to the same read: the wait
   continues to its own deadline (`g_cdfs_read_stale`); its parts are the same
   bytes for the same place, so they count.
5. `bin_echo_suppress(1)` for the wait: an echo would put one of our frames on
   the wire inside the host's burst.

**Three doors to the BIOS GD driver**, and a title served from the host must not
get through any of them to the real drive:

| Door | Who uses it | How it is held |
| --- | --- | --- |
| vector `0xac0000bc` (misc group in front, `r6 == -1`) | Katana titles | `cdfs_redir_enable()` |
| vector `0xac0000c0` (the driver alone) | Windows CE's `COREDLL.DLL` | `cdfs_redir_enable()`, saved and restored with `0xbc` |
| the driver body `0x8c0010f0` itself | Windows CE's GD driver (literals in `0WINCEOS.BIN`, called with `r6 = 0`, `r7` = index) | **the host** rewrites the literals to `_gd_bios_entry` (`dispatch::gd_body_patches`), as isoldr's `gdc_syscall_patch()` does |

The third is patched from the host rather than by overwriting the BIOS's copy
of the driver, because `gd_spin_down_drive()` needs the real driver at the next
boot.

**What a disc read may not land on** is derived from the linker:
`[dcload_base, end)` and `[_hiram_start, _hiram_end)`, compared in P1. The Maple
DMA buffer is deliberately not in the list: it is written only while a `MAPL`
command runs, never under a title.

**Invariants, each paid for:**

1. **Never transmit between building a command in `pkt_buf` and sending it.**
   Commands dispatched from inside `bb->loop()` are past that point, so
   answering `SBIQ` during a wait is fine. Corollary: emit any trace *before*
   building the command, or the trace's `write()` overwrites it.
2. **Never start a second transfer while one waits.** `bin_info` and `pkt_buf`
   are single. `g_gd_in_transfer` is non-zero across the GD waits and
   `cdda_service()` declines while it is set. Without it, a title's interrupt
   handler calling `GetDrvStat` started an audio fetch inside a disc read's wait
   and the title froze forever (Snow Surfers). Signature: `g_gd_idx_counts[2]`
   (ExecServer) advancing one-for-one with `g_cdfs_sync_reentered`, or a host
   retrying a LoadBinary that is never echoed.
3. **No function live across a yield may take the address of a local**: a
   parked frame is restored onto whatever `r15` the next ExecServer has.
4. **Every wait needs a millisecond deadline on the loader's clock**
   (`fine_deadline_*`, `GD_READ_DEADLINE_TICKS` 250 ms for disc reads). The
   seconds timeout counts whole seconds on the same counter (2 s fires at 3 s),
   `RTL_IDLE_POLL_LIMIT` only counts polls with no frame, and both are disarmed
   when `timeout_loop` is cleared under a wait. Arm, call, then clear — never
   clear someone else's.
   **The clock is performance counter 1, never a TMU** (`clk_now()`,
   `clk_since()`, `adapter.h`): started by `main()` counting CPU cycles,
   `>> 4` = Pck/4 ticks, so every `*_TICKS` constant and every millisecond the
   host prints keep their unit; it wraps every 21.4 s. A title owns its TMUs:
   KOS reloads TMU2 every second. Under Sonic Adventure 2, measured on TMU2,
   asynchronous reads were judged 250 ms late one frame after their request —
   re-asked every 16.7 ms, the ADX music starved — while synchronous reads
   (inside one syscall) never were: the title disturbs TMU2 between two looks
   (how is still to be read off `g_gaf_tmu2`, `GA_FAIL_PROBE`). The loader neither
   programs nor reads TMU2. KOS clears this counter once in its init, before
   any read; `g_pmcr_backwards` counts a later restart. **flycast reads 0
   from it without the local `sh4_mmr.cpp` patch** (§11): every deadline is
   then dead there.

**The GD lock's C sections run masked.** `gd_take()` raises IMASK to 15
before taking the lock and `gd_give()` gives the caller's IMASK back (only
IMASK: T and the rest of SR stay). ReqCmd, GetCmdStat, GetDrvStat and
ChangeDataType hold it for a few instructions, no network, nothing waited for.
Unmasked, a title's handler that landed inside one and polled the driver until
it stopped answering BUSY waited on a holder that could not resume: Shenmue II
froze before its menu (2026-10-03: GetDrvStat held it 266 ms while the driver
kept answering BUSY). The server's own hold (`es_enter`) stays interruptible:
it waits on the network, and BUSY is the BIOS's answer then too. Anything
added between `gd_take()` and `gd_give()` must stay short and must not wait;
`gdGdcReset()` and the watchdog release with `gd_release()`, which leaves SR
alone.

**Stuck-lock watchdog.** `gd_lock_watchdog()` (from GetDrvStat/GetCmdStat)
releases the GD lock if it is held while the server is parked,
`g_gd_lock_gen` has not moved for `GD_LOCK_STUCK_TICKS`, **and no C syscall is
inside** (`g_gd_lock_owner` 0). Its one recorded release was wrong: the
Shenmue II hold above, which it handed to the polling handler while the
interrupted call later released the lock a second time. Leaving such a hold
(`g_gd_lock_held_long`) froze the title and silenced the loader, whose tick
needs the lock free. On TMU2, which Shenmue II stops for its RTC calibration,
the watchdog seldom reached 250 ms: the same freeze, intermittent and deaf
(2026-10-02). With the mask, `g_gd_lock_held_long` must stay 0.
`_stuck_owner` / `_stuck_ticks` describe the last of either. `GA_FAIL_PROBE`
builds add `g_gd_busy_c` (BUSY from a C section: 0), `g_gd_busy_srv`, and the
last caller told BUSY (`g_gd_busy_pr`, `g_gd_busy_sr`: IMASK > 0 means it
called from an interrupt).

**Constants** (`cdfs_syscalls.c`, override with `-D`):

| Constant | Value | Note |
| --- | --- | --- |
| `GD_EMU_ASYNC` | `8` | 16 KB per host request = the BBA RX ring. |
| `GD_BULK_SECTORS` | `0` | isoldr reads ≥100-sector requests in one shot; **do not**: into an 11-frame RX ring that lost 840 packets in one run. |
| `GD_YIELD_BETWEEN_CHUNKS` | `0` | (synchronous path) with the yield, Sonic Adventure died on delivery of chunk 1 — measured while SA's stack still ran through the loader (§4.6), so unconfirmed today. |
| `GD_CDDA_BETWEEN_CHUNKS` | `1` | Must stay on with invariant 2, or the CD-DA ring runs dry during level loads. |
| `GD_READ_DEADLINE_TICKS` | `3125000` | 250 ms at Pck/4. **The primary recovery**: the host waits for nothing, so nothing else notices an answer that never arrives. |
| `GD_READ_RETRIES` / `_MMU` | `4` / `20` | re-requests before failing a read; Windows CE's driver gives a failed read up for good, hence 20 under the MMU. |
| `GD_SYSCALL_TIMEOUT_SECONDS` | `6` | coarse seconds backstop (fires at 7 s). |
| `GD_DRAIN_ITERS` | `0` | pre-request RX drain; measured useless at 256 and 50000. |
| `GD_SERVICE_ITERS` | `256` | used by `GD_SERVICE_EVERY_SYSCALL` (§4.3). |
| `GD_TRACE_CALLER` / `GD_TRACE_DEST_FROM` | `0` / `0xffffffff` | caller PC/SP tracing; expensive. |
| `GD_LOCK_STUCK_TICKS` | `3125000` | 250 ms, the watchdog threshold. |
| `GD_STAGE_SECTORS` / `GD_STAGE_BIG_SECTORS` | `3` / `4` | `gd_stage` (`.hiram`) and `gd_stage_big` (`.gdstage`), §4.15. |
| `GA_PHYS_SECTORS`, `GA_POLL_ITERS`, `GA_XLAT_PAGES` | `6`, `2`, `32` | asynchronous reads: sectors per physical chunk, loop turns per look, 4 KB pages translated ahead (§4.15). |

**Known gaps:**

- **No `g2_lock()` around CPU reads of the BBA on the synchronous Katana path**
  (a 250 ms read deadline would stall the title's sound). Under the tick and
  under KOS the title's G2 DMA is suspended (§4.16).
- `CMD_INIT`, `CMD_REQ_MODE` and `CMD_SET_MODE` are force-completed with
  **nothing written back**. Count first (`g_gd_cmd_counts[24]`, `[30]`, `[31]`)
  before spending bytes on it.
- **Reads into a translated address are staged** (`gd_is_virtual()`: P0 with
  MMUCR.AT set): received in `gd_stage_big` (in `.gdstage`, used only under the
  MMU because in the LOW family it is a Katana title's stack) or `gd_stage`
  (`.hiram`; the TOC, and a stream under a Katana title), and copied with
  `memcpy.S`. The host cannot write such an address and the loader may not take
  a TLB miss.

### 4.6 The footprint rule

**A retail title uses the BIOS work area as a stack, and that is where a LOW
dcload lives.** Sonic Adventure enters GD syscalls with `SP = 0x8c00b9d0`,
growing down; when its stack reached `bb` (the adapter pointer, near the end of
BSS), dcload's next `bb->loop()` jumped through garbage to address 0.
`docs/sonic-adventure-investigation.md` has the record.

**The hard bound is `0x8c00c000`.** A Katana title's crt0 **fills
`0x8c00c000`–`0x8c00f400` with the word `"SEGA"`** as its very first loop,
before any GD syscall (six retail titles of six checked). A LOW loader whose
`_end` is above `0x8c00c000` has its `.data` and `.bss` overwritten whatever
its stack margin; the title then fails `gdFsInit` and exits to the BIOS menu
(flycast logs `SYS_MISC 1`, then `REIOS: Booting up`) or spins forever.
isoldr does not meet this because its image ends at `0x8c007400`.

**Where it stands (2026-10-03):**

- **LOW (the CD build): `_end = 0x8c00d638`, 5.6 KB over the bound.** The CD
  loader cannot host a retail Katana title at its own base. That is accepted
  (user decision, 2026-09-21): the host chainloads its own HIGH set before any
  disc image and refuses a low base whose image reaches a range the title
  paints (`low_loader_painted_by_title`). Say so when a change makes it worse.
  `.gdstage` ends 1472 B under `_stack`.
- **HIGH (the host's set): `_end = base+0x9b2c`, `.gdstage` ends 192 B under
  `_stack` (`base+0xbc00`).** This is the binding budget. The next bytes come
  from `GD_STAGE_BIG_SECTORS` (2 KB a sector, costs Windows CE's staged reads)
  or from a feature flag of §4.3.

Rules when adding anything DC-side:

- Check `_end` and `__gdstage_end` in `dcload.map` for **both** families
  (`make config` / `DCLOAD_BASE=0x8ce00000` for HIGH) after any change that
  adds state.
- Put large buffers in `.hiram` (`HIRAM_BUF`), not BSS. `.hiram` is nearly full.
- `bb` is near the end of BSS — one of the first things a descending stack
  reaches. Nobody has ordered BSS deliberately yet.
- **The guard is `g_gd_sp_min`** (lowest SP any GD syscall was entered with)
  and `g_gd_sp_in_image` (entries already inside `[_dcload_base, _end)`),
  latched by `gd_note_caller()` at no network cost. `g_gd_sp_min - _end` is the
  margin. A `ReadSector` whose size is not a sector multiple is a request built
  from overwritten state.

### 4.7 Handoff to a game, and reachability

- **`go.S` hands `SR = 0x60000101`** (MD=1, RB=1, BL=0, IMASK=0): the title can
  take interrupts and exceptions immediately. isoldr hands `0x700000f0`
  (`ISOLDR_SR`). RB=1 switches register banks, so the entry address is moved out
  of `r4` first.
- **Caches off at handoff** (`CCR = 0x0808`), written from P2 with the settling
  window. A deliberate divergence from isoldr (`0x0909`): dcload fills the
  title's buffers with CPU stores that the title reads back.
- The VBR and SP handed over are `0x8c00f400` whatever the loader base.
- **`announce_presence()` (net.c)** sends a gratuitous ARP from the main loop.
  Without it a static `DREAMCAST_IP` is unreachable: flycast's BBA bridge opens
  its capture only after the guest transmits, and Windows silently discards
  datagrams to an `Unreachable` neighbour. DHCP hid this.

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

And **every hardware wait is bounded** (`RTL_LINK_SPIN_LIMIT`,
`RTL_TX_WAIT_TICKS`): an unbounded link-change spin once dropped the poll loop
from ~45000 iterations per 0.3 s to one.

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
- **The IP travels in the adapter's SRAM**, at GAPS offset `0x5000` (magic
  `'DCWS'` + IP + complement), written by `rtl_handoff_save()` just before
  `go()` and read into `g_warm_ip`. RAM cannot carry it (1st_read zero-fills,
  crt0 zeroes BSS). The DHCP lease is not carried. The screen shows
  `(Warm Start)`.
- BBA only; the LAN Adapter always takes the cold path.

Commands are matched on the MAC and `our_ip` is taken from the packet
(`cmd_loadbin()`), which is why the host can reach a DC without a lease.

### 4.10 DHCP: a reply is not ours just because it is a reply

Only bites on a real LAN:

1. **Ports** (`process_udp()`): the DHCP parser runs only for 67 → 68.
2. **xid and chaddr** (`handle_dhcp_reply()`): the xid we sent is kept in
   `dhcp_my_xid`; another machine's broadcast OFFER is not adopted.

A rejected reply returns -1 so the wait continues. `g_dhcp_replies` /
`g_dhcp_not_ours` show whether this happens (readable with `SBIN` during the
DHCP wait). A NAK restarts `dhcp_go()` recursively, bounded by
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
BIOS syscall area, and `video.s` jumps through the font pointer at `0x8c0000b4`
on every string drawn, including during the upload that destroys it.
Supporting them means dropping the on-screen display for those bases. The host
places such titles elsewhere (§16).

`DCLOAD_BASE` is a build variable; the host picks a base and chainloads a
loader there before uploading the title. Things this must not break:

1. **`0x8c004000` stays the default and what the CD boots** (magic and
   trampoline addresses are literals in every KOS program).
2. **The state handed to the title does not follow the loader**: VBR/SP
   `0x8c00f400` for every base, as isoldr does. Each loader ELF carries
   `exception.bin` as a `.guestvbr` section at that address.
3. **Two layout families.** LOW (base < `0x8c010000`): stock layout, only
   ORIGIN moves. HIGH: relative to the base — image at `+0`, stack top
   `+0xbc00` (1 KB under `.hiram`, so that no symbol value is both: the
   relocator classifies words by value), `.hiram` `+0xc000` (12 KB), Maple DMA
   `+0xf000`, span `0x10000`. The host's `loaders::layout()` mirrors this.
4. **A low loader's buffers are at `0x8cfe8000`/`0x8cfe9000`**, so chainloading
   directly to a `0x8cfe8000` base writes over the running loader's packet
   buffers: the transfer "succeeds" and the new loader is deaf. The host always
   hops through `0x8ce00000` (§14.17).
5. **A preset's address was chosen for isoldr (13 KB); we reserve `0x10000`.**
   Sonic Adventure 2's preset `0x8cfe8000` puts the loader's stack where the
   title's Maple DMA list lives (`0x0cff0000`): black screen. The host scans the
   title for such constants before choosing (§16).

**The loader set is one relocatable image.** `make loaders` links
`loaders/dcload-relocatable.elf` at `0x8ce00000` with `DCLOAD_EMIT_RELOCS=1`
(`LOADER_BASES` is empty; list bases there to get pre-linked ELFs back). It
does not deploy: copy the ELF to the host's `loaders/` and prove the copy by
md5 (§14.19). **One set is kept there**; an A/B is a build into a directory of
its own, passed with `--loader-dir` — a flycast set needs
`DREAMCAST_IP=192.168.1.130 CDDA_TICKS_X8192=580480` (§3.5, §4.13). The host
relocates the image to any base in either family:

- The image only has `R_SH_DIR32` relocations. Each is classified by **the value
  of the symbol it names** (image, `_stack`, `.hiram`, `_maple_dma_buffer`) and
  gets that region's delta — four deltas, equal inside one family. Classify by
  symbol value, not section (`_dcload_base` is filed in `.hiram` by `ld`) and
  add the delta to the word (P2 aliases like `0xace00000` keep their bits).
- **`.guestvbr` has no relocations**: `exception.S` names the jump table as
  `DCLOAD_BASE + …` literals (six words). The host patches them by content and
  re-scans; any other reference to the loader there fails the relocation.
- Relocation reproduces a native link byte for byte at `0x8c004000`,
  `0x8ce00000`, `0x8cef8000`, `0x8cfe8000`.
- **Every address C code uses must be a linker symbol, never a `-D` number** —
  a `-D` folds into a literal pool with no relocation. Only `exception.S` uses
  `DCLOAD_BASE` as a `-D`; C code uses `_dcload_base` (§14.11).
- A LOW target's image must fit under `0x8c00f400`. **The HIGH set does not fit
  at `0x8c004000`** (image and `.gdstage` ~1.9 KB past the VBR):
  `LoaderSet::can_provide` relocates for real to answer a low base, and the host
  leaves the low family when it cannot.

`DCLOAD_BASE`/`DCLOAD_STACK`/`DCLOAD_HIRAM`/`DCLOAD_MAPLE` reach the linker
script, the C sources and `exception.S` from the Makefile only.

### 4.12 A title can switch the adapter off

**Sonic Adventure 2 powers the BBA's GAPS bridge down** during `main`, with the
same two writes `rtl_bb_detect()` uses (`0xa1001414 ← 0`,
`0xa1001418 ← 0x5a14a500`), after probing the four G2 slot windows for
`"GAPS"`. dcload is then deaf: black screen, no packets, title running. Not
reproducible under flycast.

**The fix is on the host**: `gaps_probe_patches()` changes the `"GAPS"`
comparison constant in the title (found by content), so the probe finds nothing
and the title takes its no-expansion-device path. On by default;
`--no-gaps-guard` disables it.

A DC-side recovery does not work: after a full cold re-init `rtl_bb_tx()` never
returned. Lesson: when every instrument goes quiet at once and the title is
visibly alive, suspect the transport's power, not its logic.

### 4.13 CD-DA: the loader is the drive

A GD-ROM stores its music as audio tracks, played with `CMD_PLAY_TRACKS` /
`CMD_PLAY_SECTORS`. `cdda.c` answers those itself: it fetches the audio from
the host and plays it on AICA channels 62/63. **The header of `cdda.c` is the
design description**; this section is the summary and the rules. History:
`docs/cdda-crackle-investigation.md`, `docs/cdda-double-buffer-investigation.md`.

#### How it works

- **Ring and lead.** Each channel loops over one ring in sound RAM, written by a
  single write head kept a fixed distance — the **lead** — ahead of the AICA.
  The play position is not read from the AICA (its monitor select is shared with
  the title's sound driver): **TMU1** (Pck/16), started in the key-on critical
  section with `TCOR = end_tm` (one loop), stands in for it. `cdda_service()`
  fetches only what the lead is short of, so fetches come one every 53 ms, at
  the rate audio is consumed. (A double buffer filled on half boundaries, as
  isoldr does from a drive, meant bursts of round trips the title is frozen for:
  4-5 dropped frames a second in Snow Surfers.)
- **Who calls it**: the top of the GD server loop, `gdGdcGetDrvStat` (before the
  lock), `cdda_service_between_chunks()` inside a long disc read, and the hook's
  tick (`cdda_service_tick()`, §4.15). It declines while `g_gd_in_transfer` is
  set (§4.5 invariant 2) or a title's G2 DMA writes sound RAM (ADST), and is not
  re-entrant (`busy`).
- **Format: 4-bit Yamaha ADPCM by default** (`DC24`): the host encodes and sends
  the left block then the right. **Every key-on restarts the host's encoder**
  (bit 31 of the request, from `cd.restart`), and every byte the AICA plays must
  be the continuation of the one before. The host never emits a nibble the
  AICA's decoders disagree on (§16). `CDDA_ADPCM=0` selects 16-bit PCM (`DC23`).
- **Geometry** (ADPCM defaults):

  | | |
  | --- | --- |
  | sub-fetch | 4 sectors = 2352 frames = 53 ms, one round trip, into `cdda_pcm` (`.hiram`) |
  | ring | 26 sub-fetches = 61152 samples/channel = **1.39 s** (LEA is 16 bits), 60 KB of sound RAM for both |
  | lead | 20 sub-fetches; less the trigger's lag, **893 ms** of audio ahead of the AICA |
  | per service call | `CDDA_FETCHES_PER_SERVICE` plus one per sub-fetch the gap since the last call consumed, up to the lead |
  | PCM | 3-sector sub-fetch, 1.04 s ring, 179 KB of sound RAM |

- **Start** (`cdda_prime()`, used by PLAY, RELEASE, SEEK, the theft repair and
  the overrun mute): key off and arm the fill; the services lay the lead and key
  on only when it is whole (`cdda_prime_step()`), or sooner if the range ran out.
- **Silence, never garbage.** If the lead falls within one sub-fetch of nothing,
  the channels are keyed off and the stream restarts at what was last heard
  (`cdda_current_lba()`): `g_cdda_mutes`. The lead is a difference modulo one
  loop, so it cannot see itself gone past zero; a **service gap** over
  `CDDA_GAP_LIMIT_TICKS` (838 ms) says so on the loader's clock instead. **Key-off silences
  because register 20 carries RR = `0x1f`**; RR = 0 holds the level.
- **Clock**: `end_tm` comes from `CDDA_TICKS_X8192` (isoldr's constant × 16;
  `580480` on flycast, whose clocks are exact) and the host's trim: a ppm scale
  in the `size` field of every audio ReturnValue, applied at the next TMU1
  reload, ignored outside ±1.5 %.
- **Phase**: the fill trigger reads the model `CDDA_LAG_SHIFT` late (an eighth
  of a loop). Everything that measures uses `cdda_true_elapsed()` (lag added
  back); only the fill trigger uses the lagged model.
- **Fetch integrity.** Audio answers come without acknowledgement round trips,
  so `cdda_fetch()`:
  1. closes the LoadBinary window before each request, so completion is judged
     on this answer's own window (`g_cdda_retv_nodata`: our LBA came back
     without our data);
  2. accepts only a ReturnValue whose `address` is the LBA it asked for —
     `cmd_retval()` takes another one for nothing and lets the wait go on
     (`g_retval_want`, `g_cdda_wrong_lba`);
  3. keeps the door shut: `cmd_loadbin()` refuses any LoadBinary into the
     staging buffer except the one awaited (`g_cdda_stale_lbin`);
  4. suppresses the LoadBinary echo and ends the wait on the last PartBinary
     (`bin_complete_escape`);
  5. on failure, closes the window and drains for 10 ms.

  Deadline **20 ms** per sub-fetch (normal ~3 ms). The host drops answers it
  took longer than `CDDA_GIVE_UP` (15 ms) to produce and answers re-asks from a
  cache of its last 48 requests.
- **Channels**: TL (byte 41) is **attenuation** (0 = full). The send level
  (DISDL) mirrors the game's CD input level (`0x2040`/`0x2044`), clamped to
  `CDDA_DISDL`. Every `CDDA_CHECK_FETCHES` sub-fetches a watchdog re-reads the
  channels: level drift is rewritten in place, a structural change confirmed
  twice in a row is a theft (`g_cdda_ch_stolen`) and restarts the stream. An
  all-zero control word is a failed read, not a theft.
- **Sound RAM**: the rings sit under `CDDA_RING_TOP` = `0x150000`. The top of
  RAM, isoldr's place, reaches into Snow Surfers' samples at our size.
- **Idle listening**: a service with nothing to fill listens to the network for
  at most 1 ms, at most every 20 ms, so `--diag` is answered while a title runs.
- **TOC**: fetched once per session with `DC22`, area 2 (whole disc).
- **Repeating ranges**: an ADPCM range's odd final sector is skipped (fetches go
  in sector pairs) so the repeat still applies.

#### Rules

1. Never service while `g_gd_in_transfer` is set or a title's G2 DMA into sound
   RAM is in flight; from an interrupt, only through `cdda_service_tick()`:
   GD lock free, one sub-fetch, no listening.
2. Every key-on restarts the ADPCM encoder; any re-key goes through
   `cdda_prime()` (re-keying onto the existing ring decodes it from a reset
   decoder: full-scale noise).
3. The host's `CDDA_GIVE_UP` must stay below `CDDA_FETCH_DEADLINE_TICKS`.
4. Key on only over a whole lead; key off before the lead runs out.
5. A fetch is complete only on its own window: close it before the request.
6. Measure with the true model; trigger with the lagged one. Anything that could
   outlast one loop is judged on the loader's clock (`clk_since()`).
7. All AICA access by the CPU inside `g2_lock()`, with a FIFO wait at most every
   eight 32-bit stores. `g2_lock()` first waits for the ring's own DMA
   (`g2dma_quiesce()`, §4.16). In ADPCM the ring is written by DMA: unaligned
   edges (≤ 28 bytes a side) by the CPU, the 32-byte-aligned body on G2 channels
   2 (left) and 3 (right), left running when `cdda_push()` returns. The staging
   buffer is `STAGE_BYTES` (`FETCH_BYTES` + 64), 32-aligned, each block placed
   congruent to its destination modulo 32. `CDDA_ADPCM=0` keeps CPU writes.
8. AICA reads from the SH4 sometimes return `0x00000000`: an implausible zero is
   a failed read.
9. No variable divisors (libgcc's divider costs ~1 KB, §14.15).
10. **Changing the geometry invalidates things outside `cdda.c`**: the host's
    trim thresholds and `CddaClock` tests assume a stream paced one sub-fetch at
    a time; the encoder cache (`RECENT` = 48) must cover the whole ring (26).

#### Build flags

| Flag | Default | What it does |
| --- | --- | --- |
| `WITH_CDDA` | `1` | the engine |
| `CDDA_ADPCM` | `1` | 0 = 16-bit PCM |
| `CDDA_RING_FETCHES` | `26` | ring size, as large as LEA allows |
| `CDDA_LEAD_FETCHES` | `20` | audio kept ahead of the AICA |
| `CDDA_FETCHES_PER_SERVICE` | `2` | sub-fetches per service call, plus the catch-up above |
| `CDDA_LAG_SHIFT` | `3` | how late the fill trigger reads the model, as a shift of one loop |
| `CDDA_TICKS_X8192` | `578960` | Pck/16 ticks per 8192 samples; `580480` for flycast |
| `CDDA_SERVICE_DRAIN_ITERS` | `256` | idle listening window; 0 removes it |
| `CDDA_RING_TOP` | `0x150000` | top of the rings in sound RAM |
| `CDDA_DISDL` | `0xf` | send-level ceiling, ~3 dB a step |
| `GD_CDDA_BETWEEN_CHUNKS` | `1` | feed CD-DA during disc reads (`cdfs_syscalls.c`) |

The Makefile leaves most of them empty so `cdda.c`'s default applies; each is
added to `CFLAGS` inside its own `ifneq` — keep it that way.

Constants in `cdda.c`: `CDDA_FETCH_DEADLINE_TICKS` 20 ms,
`CDDA_DRAIN_DEADLINE_TICKS` 10 ms, `CDDA_TOC_DEADLINE_TICKS` 500 ms,
`CDDA_MUTE_GUARD` one sub-fetch, `CDDA_GAP_LIMIT_TICKS` 838 ms,
`CDDA_CH_BAD_LIMIT` 2, `CDDA_CHECK_FETCHES` 13.

#### Counters (read with `--diag` or `scripts/dc-counters.py`)

| Question | Counters |
| --- | --- |
| Is it healthy? | `g_cdda_plays`, `g_cdda_fetches` (sub-fetches, ~19/s), `g_cdda_fetch_fails`, `g_cdda_mutes`, `g_cdda_room_min` (least lead seen, TMU1 ticks, 3125 ≈ 1 ms; ~893 ms is healthy), `g_cdda_svc_gap_max` (longest time without a service, Pck/4 ticks) |
| Right answers? | `g_cdda_wrong_lba`, `g_cdda_retv_nodata`, `g_cdda_stale_lbin` |
| Channels | `g_cdda_ch_stolen` |
| Clock | `g_cdda_end_tm`, `g_cdda_scale_ppm` (1000000 = no trim yet) |
| Misc | `g_cdda_toc_fails`, `g_cdda_last_lba` |

TMU1 is Pck/16 (3125 ticks/ms) and the loader's clock Pck/4 (12500, §4.5):
`--diag` prints both in milliseconds.

#### Open questions and limits

- No phase servo: long sessions without a PLAY/seek rely on the host's trim.
- A title that stops calling the GD driver and has no hook stops the music
  (isoldr too).
- A play range shorter than the lead keys on early and is untested on hardware.

#### Lessons from this engine

- **When every counter is clean and a glitch is heard, record the output** and
  align it with the host's stream decoded offline (flycast's `DecodeADPCM`):
  that localises an ADPCM fault to the sub-fetch. Every counter was clean for
  weeks over a fetch that pushed the previous sub-fetch again.
- A completion test must be about this transfer: a window left over from the
  previous one, same address and size, is already complete.
- A model that wraps cannot see a whole period of lateness: judge it on a clock
  it is not derived from (the loader's clock).
- A counter panel cannot report a schedule: dropped frames came from bursty
  fetching while every counter was healthy.
- In a differential format a hole is carried forward by the decoder: muting
  before it and restarting clean is the only bounded outcome.
- Read every field a register write zeroes as a decision (RR, §14.21).
- A model shared by two machines must agree with every implementation of the
  other end (the encoder had to match flycast's and Sega's clamping decoder).
- Nothing one-off and lazy may happen inside a syscall the console is timing.

### 4.14 The drive is stopped at boot

`gd_spin_down_drive()` (`cdfs_syscalls.c`, `WITH_GD_SPINDOWN=1`, **off by
default** to pay for the hook under the HIGH bound) puts the real GD-ROM in
`<STANDBY>` from `main()`. Nothing in a session reads the disc again. The
drive's firmware already does this after 180 s of `<PAUSE>` (`Cdif131e.txt`,
"Standby Time"), so this buys three minutes of rotation per boot. It is
reversible: a KOS program calling `cdrom_init()` spins the drive back up.

1. **It calls the real BIOS driver** through `0xac0000bc`, so it runs after
   `cdfs_redir_save()`/`cdfs_redir_disable()` have put the BIOS back on that
   vector. ABI: r7 = function index, r6 = 0.
2. **The BIOS driver is a coroutine too**: `ReqCmd` only queues the STOP; it
   progresses while we call `ExecServer`. `InitSystem` is the fallback.
3. **Bounded on a spin count** (`GD_SPINDOWN_SPIN_LIMIT`), in the style of
   the other hardware waits. A drive that will not answer is left spinning.

`g_gd_spindown`: final command status + 2 (**4** = COMPLETED; 1 FAILED, 2 IDLE,
5 STREAMING), 7 "the driver would not take the command", 8 "it never finished",
0 never ran. **Not yet measured on hardware.**

### 4.15 Windows CE titles, and the interrupt hook

Sega Rally 2 PAL boots, plays and saves under the loader; the history is
`docs/wince-investigation.md`. **Booting a CE title boots an operating system**:
`0WINCEOS.BIN` starts it and CE then loads ~250 files off the disc through its
own GD driver.

**Boot** (host, `src/wince.rs`; automatic, `--no-wince` to disable): the first
2048-byte sector of `0WINCEOS.BIN` is dropped and the rest loaded at
`0x8c010000`, as isoldr does; the title is entered through the disc's own
second bootstrap (`--boot-ipbin`).

**Memory.** CE's kernel takes `0x8c143000..0x8cef0000`, its driver globals
`0x8cef0000..0x8d000000`. The host places the loader at `0x8cee0000` (HIGH) and
lowers `ulRAMEnd` to it. **Nothing in the loader may write outside its own span
while a title runs.** Inside the span, three regions are dead while a title runs
and are reused:

| Region | Before `EXEC` | Under a title |
| --- | --- | --- |
| `.gdstage` (above `_end`, under `_stack`) | the loader's own stack | `gd_stage_big`, MMU only |
| Maple DMA page (4 KB) | `MAPL` commands | the hook's stack (first KB) and the network exchange's stack (top) |
| `gd_stage` (`.hiram`, 3 sectors) | -- | the TOC; streams under a Katana title |

**The GD driver under CE.**

- CE calls the BIOS driver body `0x8c0010f0` directly (§4.5, third door).
- **CE runs with the MMU on.** DMAREAD's destination is physical; every other
  buffer is virtual (`gd_is_virtual()`), staged and copied (§4.5).
- **Streams** are PIO-chained; the host turns the branch that picks DMA for an
  aligned read into a branch to PIO.
- **Status words matter.** PROCESSING with status[3] = `CMD_WAIT_IRQ` makes CE sleep
  on the GD interrupt; the retry path clears it under the MMU so CE polls.
- **CE never calls `GetDrvStat`**: CD-DA is fed from ExecServer (hundreds of ms
  apart) and from the hook's tick; `cdda_fill()` budgets the gap's sub-fetches.

**A preemptive OS calls us.** CE's timer interrupt can run other threads for a
whole quantum in the middle of a network exchange, and CE calls the driver on a
virtual thread stack where a TLB miss enters CE's kernel with IMASK 0. So every
network exchange of the GD path (`ReadSectors`, `GetTOC`, `cdda_fetch`) goes
through `gd_exchange()`: under the MMU (and under KOS, `g_gd_kos`) it masks
interrupts (`bb_irq_hold()`) and then runs on the Maple page
(`gd_on_loader_stack`, `cdfs_redir.s`), in that order. `gd_on_loader_stack()`
also clears SR.FD and saves the FP registers (`fpu_push`/`fpu_pop`): CE runs
threads with FD set (lazy FPU), and our first FPU instruction would otherwise
enter CE's kernel on our stack. Every exchange ends with reception off.

**The interrupt hook** (`irq.c`, `irq_hook.S`, `WITH_IRQ_HOOK`). Every SH4
interrupt enters at `VBR+0x600`; the loader patches the title's live table so
`irq_entry` runs `irq_tick()` first, then resumes the title's handler.

- **Two entries are recognised**, re-checked on GD syscalls
  (`irq_hook_check()`; `g_irq_rehooks` when a table lost the hook):
  - **Windows CE's exact entry** (`+0x5e8..+0x600` zero and `567a d022 6163` at
    `+0x600`): a 30-byte template goes over it (`+0x5e8` trampoline and its two
    literals, `+0x600` `nop; bra; nop`), and `irq_entry` resumes at `+0x606`
    with CE's three replaced instructions rebuilt.
  - **The Katana library's entry** (six nops, then `mov.l r0,@-r15 ;
    mov.l @(disp,pc),r0 ; jmp @r0 ; mov.l r1,@-r15`, copied by the title's crt0
    to `VBR+0x100/+0x400/+0x600`): `g_irq_nop_entry` 1, nothing to rebuild.
  - Anything else is refused and counted once per VBR (`g_irq_refused`,
    `g_irq_refused_vbr`); the GD path then works without the hook.
- **Rules the entry keeps, each a reset if broken** (SR.BL is 1 throughout):
  r15 is replaced before any push (a TLB miss under BL resets); FD is cleared
  and FPSCR, FPUL and both FP banks saved; the pair stores need r15 8-aligned
  (22 longs pushed before them: keep it even); C reached from `irq_tick()`
  touches P1/P2 only and never waits on the title.
- **Two stacks, never one**: the hook's (the Maple page's first KB) and the
  exchange's (its top). Sharing one corrupted an exchange CE had preempted.
- **What the tick does**, only while the GD lock is free (every network use of
  the GD path holds it, so a free lock means nobody is half way through
  `pkt_buf`, `bin_info` or the ring):
  - feeds CD-DA at most every 5 ms (`IRQ_CDDA_PERIOD`) — one sub-fetch, no
    listening window, on the exchange's stack;
  - moves asynchronous reads (§4.5): drains the ring in passes of
    `GA_POLL_ITERS`, judges each chunk (window whole + its ReturnValue = done,
    250 ms = failed), copies it into the title's buffer and posts the next,
    feeding CD-DA between chunks; looks at a read at most every 0.5 ms
    (`IRQ_READ_PERIOD`) unless the RX interrupt or a finished RX DMA calls it;
  - with `IRQ_IDLE_LISTEN`, listens to the network when nothing else owns it.
- **Virtual destinations under CE** cannot be written from the tick (it cannot
  take a TLB miss). On each wake the GD thread translates the next
  `GA_XLAT_PAGES` 4 KB pages **by CE's own page tables** (`ga_walk()`: TTB,
  section, MemBlock, entry — what CE's TLB refill and flycast's
  `USE_WINCE_HACK` read), checks each with a byte inverted through the virtual
  address and read back uncached at the physical one, purges the lines, and
  stores the result in `ga_xpa[]`; the tick writes through P1. A chunk beyond
  the translated pages waits for the thread; when nothing translates the read
  goes back to the synchronous loop (`g_ga_sync`). A UTLB probe cannot do this
  under flycast (`FAST_MMU`).
- **The BBA's RX interrupt** (Katana titles): `irq_rx_arm()` routes Holly EXT
  bit 3 to a level while a chunk is on the wire — the highest of IML6/IML4/IML2
  whose masks the title left at zero, else IML6, **shared**. The tick
  acknowledges the chip (`rtl_irq_ack()`), and `irq_entry` swallows the
  interrupt (`g_irq_swallow`) unless something of the title's is pending on that
  level. **Only an entry with the chip's bit pending is ever swallowed** —
  swallowing a title's own interrupt froze Crazy Taxi. `g_irq_rx`,
  `g_irq_rx_evt`, `g_irq_iml[9]` (the title's masks).
- Counters: `g_irq_hooked`, `g_irq_vbr`, `g_irq_entries`, `g_irq_ticks`,
  `g_irq_tick_max`, `g_irq_tick_sum` (the CPU the hook takes), `g_irq_evt_last`.

**Status**: CE (hook, CD-DA from the tick, asynchronous reads) measured on the
console and under flycast; Katana titles with the hook and RX interrupt run on
the console (Crazy Taxi 2, Sonic Adventure 2, Shenmue II). Sonic Adventure (1)
has not been re-measured with asynchronous Katana reads, which yield between
chunks (see `GD_YIELD_BETWEEN_CHUNKS`, §4.5).

**Debugging CE.**

- **flycast is built with `FAST_MMU`**: a UTLB probe misses pages a real SH4
  would have reloaded. Translate by CE's page tables instead.
- Under flycast, the GDB stub stops the emulation on every MMU exception while a
  client is attached, and every CE API call is one. One attach halts flycast for
  good: read everything in that attach (`scripts/dc-integrity.py --elf
  <relocated ELF>`, a dump of the loader span), then restart flycast.
- In a dump, `pc` is SPC, and an exception taken in a delay slot sets SPC to the
  **branch** before it: a `pc` on a `bra` means the fault is at `pc+2`.

### 4.16 G2 DMA: the CPU stays free while the bus moves the bytes

`docs/g2-dma-investigation.md`. The BBA's RX ring and the CD-DA rings go by DMA
where a title is running, so the title executes meanwhile. **The bus is the
limit, not the CPU** (1536 bytes from the BBA: 99 µs by CPU, 94 µs by DMA):
what is gained is CPU, not transfer time.

- **Channels.** 0 is the title's sound driver; the loader uses 1-3 for RX
  (chosen per frame, below) and 2 (left) and 3 (right) for the CD-DA ring.
  Registers: `g2dma.h`. `G2APRO` is written on each start; TSEL 4 (CPU trigger,
  suspend honoured).
- **Rule: the CPU never uses G2 over the loader's own DMA.** Every entry point
  that does calls `g2dma_quiesce()` and, in `rtl8139.c`, `rx_settle(1)` first:
  `g2_lock()`, `cdda_fetch()`, `rtl_bb_tx/start/stop/irq_ack/loop`,
  `la_bb_tx/loop`. Bounded (~2 ms); a channel that will not end is aborted
  (`g_g2dma_timeouts`, must stay 0).
- **The title's G2 DMA is suspended while the tick runs** (`g2dma_hold()` /
  `g2dma_release()`, nested; `cdda.c`'s `g2_lock()` uses them too), as KOS's
  `g2_lock()` does. Without it, Sonic Adventure 2 hung right after loading its
  sound banks, on the console only. Channels with a transfer of the loader's
  running are left alone. The synchronous GD path suspends **under KOS only**
  (`gd_exchange()`).
- **A channel the title uses is never the RX DMA's.** A Katana title arms the
  end bits of all four channels on its IML4 and would take the end of our
  transfer for its own (Sonic Adventure 2: black screen). `g2dma_pick()` chooses
  per frame, 3 then 2 then 1, a channel neither busy nor the title's
  (`g_g2dma_foreign`, bit n = channel n: seen busy or with its end bit up while
  nothing of ours is on it, or with a RAM address outside `.hiram`).
  `g2dma_forget()` clears an earlier loader's traces at EXEC. None free: that
  frame goes by CPU. The loader clears only its own channels' end bits
  (`g2dma_mine`). CD-DA's own transfers on 2 and 3 do not yet avoid a title's
  channel.
- **RX under the hook, Katana titles only** (`rx_iml` set): a long frame whose
  first 64 bytes (read by the CPU) are a PBIN for us (`rx_is_pbin()`) is copied
  by DMA into `raw_current_pkt` (`rx_dma_start()`), the ring not advanced, and
  the tick returns to the title; `rx_settle()` processes it when the DMA is
  over. Anything else, and everything outside the tick or under Windows CE, is
  copied by CPU. `rx_settle(1)` (wait) is in every entry point above,
  `rx_settle(0)` at the top of `rtl_bb_loop()` and `rtl_bb_rx()`.
- **The end of our DMA reaches us on the title's level** (IML4, evt `0x360`,
  never our IML2): `irq_tick()` takes a finished DMA (bit 16 up, at any level)
  as a reason to look now.
- **A fetch inside the tick takes its frames by CPU**: `cdda_service_tick()`
  clears `g_rx_dma_tick` around the service, or the loop stops at the first PBIN
  and the fetch judges a window barely begun.
- **What did not work**: acknowledging the chip's RX status before the DMA and
  leaving its interrupt armed (more CPU, same throughput); a synchronous RX DMA
  (nothing to gain); reading the whole ring in one DMA (no buffer for it).
- **Not done**: DMA of the payload straight to its destination (zero copy,
  ~10-15 µs a frame); it needs bytes the LOW build does not have. The NIC cannot
  verify UDP checksums (offload only on the C+), and already discards frames
  with a bad Ethernet CRC.
- **The legacy 1024-byte payload mode (dc-tool < 2.0.0) was removed from the
  loader** for its bytes: `dc-tool -l` no longer works against it.
- Counters: `g_g2dma_timeouts`, `g_rx_dma_frames`, `g_g2dma_foreign`.
- **flycast completes a G2 DMA the moment it starts**: a missing wait is
  invisible there, and so is a title's mask carrying our event. The console is
  the judge.

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
`-lws2_32 -lwsock32 -liconv` on MinGW. It does not serve `DC23`/`DC24`/`DC25`.

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
| `-l` | — | legacy 1024-byte payload (refused by this loader, §4.16) |
| `-f` | — | no FIFO delays: faster, more loss |
| `-h` | — | usage |

One of `-x`/`-u`/`-d`/`-r` per run; `-m` and `-c` are exclusive.
`prepare_comms()` sends `CMD_VERSION` with `(major<<16)|(minor<<8)|patch`,
detects legacy (31313) vs v2 (53535), and identifies the adapter.

### 7.3 Networking constants

UDP **53535** (v2), legacy **31313**. Payload 1452 B (1494 with headers); `-l`
forces 1024 B. GDB stub TCP **:2159**. `PACKET_TIMEOUT = 250000` µs. The delay
knobs are chosen from the adapter.

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
| `PMCR` | — | performance-counter control (`WITH_PMCR_CMD`, off) |
| `MARK` | — | paint / check witness words (`WITH_MARK_CMD`) |
| `EXPT` | `CMD_EXCEPTION` | DC-to-host exception dump |

`syscalls.h` (both ends) defines `DC00`–`DC22` (exit, fstat, write, read, open,
close, creat, link, unlink, chdir, chmod, lseek, time, stat, utime, bad,
opendir, closedir, readdir, cdfsread, gdbpacket, rewinddir, cdfstoc), plus
`DD02` (legacy write) and:

- **`DC23` `CMD_CDDAREAD`** (DC→host): raw 2352-byte audio sectors. value0 =
  LBA, value1 = destination, value2 = bytes.
- **`DC24` `CMD_CDDAREAD_ADPCM`**: the same audio as 4-bit ADPCM, left block
  then right. value2 = frames (= bytes), **bit 31 = restart the encoder**.
- **`DC25` `CMD_CONSOLE`** (DC→host): console text from a running KOS title:
  value0 = fd (1 or 2), then the bytes. **No answer**; a lost datagram is a lost
  line. Sent only when `g_gd_kos` is set (only dcload-ip-rs sets it; dc-tool-ip
  does not know `DC25`). An acknowledged `DC02` has no deadline, so one lost
  frame hung KOS for good.

`DC23`/`DC24` and disc reads are answered by LoadBinary/PartBinary into the
destination and a **ReturnValue whose `address` names what was served** (the
LBA for audio, `0x40000000 | LBA` for a disc read) **and whose `size` is the
clock trim in ppm** for audio (§4.13). `cmd_retval()` latches `size` in
`syscall_retsize`; every other ReturnValue sends 0 there.

**`MARK`** (host→DC, `cmd_mark`): `address` a 64 KB-aligned start, `size` a
multiple of 64 KB (≤ 16 MB), bit 31 clear = **paint** (one word every 256 B,
through P2, holding its P1 address `^ 0x5a3cc3a5`), set = **check** (each
sampled line purged, then read through P2; a block stops at its first changed
word). Reply `MARK` at the same address: `size` 0 after a paint, the bitmap's
byte count after a check (bit i = block i changed), or `0xffffffff` if the range
touches the loader's image, stack, `.hiram` or Maple page. The host paints
before `EXEC` and checks a few blocks a second while the title runs (§16).

**A `DBIN` names its range.** `cmd_donebin()` answers with the first missing
part of the LoadBinary window (or `0, 0` when complete); `cmd_sendbinq()` ends a
memory read with a `DBIN` carrying the address and size it served. Hosts match
on the ID only, so only the filter that needs it sees the fields.

**Copies store in the destination's own segment.** `memdiff()` (`memfuncs.c`)
does not mask addresses, so every `memfuncs.c` copy writes where it was asked
to, cached or not — it used to write a P2 destination through the source's P1
segment, cached, and GTA II then read stale file headers from its P2 buffers.
`cmd_partbin` also purges every RAM destination through its P1 alias, and
`cmd_loadbin` purges a P2 destination's range before our uncached stores. The
`SBIQ` direction (a P2 source) is fixed by the same change but not re-measured
(`dcload-ip-rs selftest-readback`); hosts should still address RAM through
`0x0c…`. **No `memfuncs.c` copy may target an address the title's MMU
translates** (§4.15).

**`MAPL` carries the loader's own argument block**, not a Maple frame: one byte
each of port (0-3), unit (0 = the controller, 1-5 = its sub-units), Maple
command and **payload length in LONGWORDS**, then that many longwords
(`cmd_maple`). The reply is a `MAPL` whose `size` is the number of bytes copied
out of the Maple receive buffer and whose data is the raw response frame. A
response code is **signed**: -1 is "nothing at that address", -4 is "busy, ask
again" (retried 64 times inside `cmd_maple`). Rules in `maple.c`:

- `maple_docmd()` copies `datalen << 2` bytes; the payload copy and the response
  read go through **P2** (the Maple DMA writes the buffer and the operand cache
  does not snoop DMA). The reply length is read as `unsigned char`.
- The receive buffer is cleared before each cycle, as KOS does (not with
  `memset_zeroes_64bit()`, which forces P1), and the response header is stamped
  `MAPLE_NO_REPLY` (`0xeeeeeeee`); a cycle whose stamp survives is run again, up
  to `MAPLE_DMA_TRIES` (`g_maple_dma_empty`).
- **The busy bit is not a completion signal**: it may not be set yet right after
  the trigger, and re-triggering on top of a running cycle wedged the bus until
  the controller was unplugged. The driver waits for the **answer**, bounded by
  `MAPLE_ANSWER_SPIN_LIMIT` / `MAPLE_DMA_SPIN_LIMIT` (`g_maple_dma_timeouts`), and
  re-triggers only from a controller confirmed idle.

**VM2 / VMUPro game ID** is entirely host-side (`src/vm2.rs`, §16) over `MAPL`:
`ALLINFO` to find the device by the `extended` field, then Maple command 33 with
the memory-card function code and the product number. A loader older than the
`MAPL` fixes cannot serve it; the host's own `loaders/` keeps them in step.

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
  `dctoolip.cdi`. `-B` takes an already-scrambled binary (ours is); `-b` would
  scramble it again.

## 11. Debugging and instruments

**Ports.** dcload's GDB stub: TCP `:2159` (`dc-tool -g -x prog.elf`,
`target remote :2159`). Emulator GDB: TCP `:3263` (`.gdbinit`,
`.vscode/launch.json`; `arch sh4`, `endian little` first). A faulting program
writes `dcload_exception_dump.bin` (`exception_struct_t`).

**flycast as target**: `docs/flycast-debug-loop.md` +
`scripts/flycast-debug-loop.sh`. Needs flycast built with
`-DENABLE_GDB_SERVER=ON`; it starts suspended (release with
`scripts/flycast-resume.py`). Anything touching `:3263` must `detach`, or the
guest stays halted. flycast answers only `Z0` breakpoints (no watchpoints), and
its SH4 UBC does not compare: trap writes host-side (`dc-catch-vector-write.sh`)
or patch flycast.

**On real hardware** the loader answers `SBIQ` from inside `bb->loop()`, which
is how every counter is read there. While a title runs, dcload looks at the
network during GD waits, audio fetches, the CD-DA idle window and, with the
hook, the tick's idle listen.

| Script | What it answers |
| --- | --- |
| `dc-counters.py <ip>` | the counters on hardware, via `SBIQ`. Checks the base (from VERS) **and** 256 bytes of code against the ELF before decoding. For a host-relocated loader, produce the matching ELF with `dcload-ip-rs relocate loaders/dcload-relocatable.elf <base> -o <file>` and pass `--elf`. `--repeat N --interval S` prints deltas; `--raw addr len` dumps. Needs `sh-elf-nm`. |
| `dc-rates.sh [s]` | counters sampled twice, printed as rates per second. |
| `dc-peek.py <symbol\|addr>` | read guest memory under flycast; `--base`/`--elf` after a relocation. Never hard-code a counter address. |
| `dc-sample.py`, `dc-track.py` | poll counters; `dc-track` re-attaches per sample (a held GDB connection freezes the guest). |
| `dc-ticks.py` | read the `GUEST_TICK` sampling block. |
| `dc-freeze.py`, `dc-pc.py` | snapshot during a freeze; where the guest executes. |
| `dc-screen.py <out.png>` | the displayed framebuffer (`--scale`, `--repeat`, `--probe` for CRC/luma). Re-attaches per frame. |
| `dc-regs.py` | where a write was aimed (QACR, C2DSTAT, DMAC, TA pointers). `DAR2 = 0` is normal. |
| `dc-integrity.py` | did the title overwrite the loader (sections vs ELF or a baseline). |
| `dc-irqwatch.py`, `dc-trap.py` | interrupts; `Z0` breakpoint with a self-test (the BIOS ROM ignores flycast's patch). |
| `dc-catch-vector-write.sh` | host-side hardware watchpoint (`cdb.exe`) with a positive control: no `SELFTEST-OK` means no result. |
| `dc-keys.sh`, `flycast-send-key.ps1` | press Dreamcast buttons in flycast without taking the focus. |
| `flycast-counters.sh` | reads the `dcdiag` flycast patch — **absent from the current flycast tree**; re-apply before use. |
| `sa-repeat.sh`, `sa-gdi-control.sh`, `flycast-transition.sh` | Sonic Adventure session drivers; `sa-gdi-control.sh` boots the GDI in flycast without dcload (§14.18). |
| `make-preset-db.py` | generates `docs/game-presets.tsv`. |

**Always-compiled counters** (CD-DA in §4.13, hook in §4.15, G2 DMA in §4.16):

- Footprint: `g_gd_sp_min`, `g_gd_sp_in_image` (§4.6).
- GD: `g_gd_idx_counts[]` (per syscall index; `[2]` ExecServer and `[4]`
  GetDrvStat run once per frame in Sonic Adventure, `[3]` InitSystem must be 1 —
  a free check of the array alignment), `g_gd_cmd_counts[]` (per GD command,
  counted before the lock), `g_gd_park_longs`, `g_cdfs_sync_chunks`,
  `g_cdfs_sync_reentered`, `g_gd_spindown`, `g_cdfs_read_retries`/`_fails`/
  `_holes`/`_stale`, `g_gd_stale_lbin`, `g_gd_in_transfer`, and the lock group
  (`g_gd_lock_stuck`/`g_gd_lock_held_long`/`_stuck_owner`/`_stuck_ticks`/
  `g_gd_lock_owner`/`_gen`).
  **`_fails` and `_holes` are different ends of the link**: fails means the host
  never answered, holes means it answered short. Since the host stopped
  acknowledging disc reads, `_holes` is the only place a lost packet in a read
  shows up.
- Transfers: `g_lbin_count`, `g_lbin_noecho`, `g_bin_data_done`,
  `g_dbin_count`, `g_dbin_incomplete`, `g_pbin_ok`/`_rejected`/`_clamped`,
  `g_last_load_addr`/`_size`, `g_last_reject_*`.
- RX: `g_rx_frames`, `g_rx_polls`, `g_rx_wraps`, `g_rx_overflow`, `g_rx_reinit`,
  `g_rx_linkchange`, `g_rx_link_giveup`, `g_rx_hdr_defer`, `g_rx_resync`,
  `g_rx_missed` (the chip's own drop tally), `g_rx_last_capr`/`_cbr`.
- Timeouts: `g_fine_timeouts` (fine deadline exits) and `g_idle_polls_max` are
  **different exits** — reading the second as "no timeout fired" hid a 3 s
  freeze. `g_pmcr_backwards`.
- DHCP / warm start: `g_dhcp_replies`, `g_dhcp_not_ours`, `g_warm_start`,
  `g_warm_ip`.
- Maple: `g_maple_dma_empty`, `g_maple_dma_timeouts` (§8).

**The loader writes nothing outside its own footprint while a title runs.** An
instrument that needs to must justify the address against the title
(`GUEST_TICK_BLOCK` is the one such address, off by default). A former
post-mortem block at `0x8cf0c000` wrote into Windows CE's driver globals on
every read.

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
6. **Changing a build variable without `make clean`.** The `%.o` rules depend on
   the Makefiles, not on variables given on the command line, so
   `make CDDA_ADPCM=0` after a default build only relinks old objects — a mixed
   binary that links fine. The same holds for `DREAMCAST_IP`. `make loaders`
   cleans by itself; anything else: `make clean` first, then check the result
   (`_end`, or `strings -a dcload | grep -E '^[0-9.]+$'` for the IP).
7. **Setting `VERSION` anywhere but `Makefile.cfg`.**
8. **Adding a command or syscall on one side only** — both `commands.h` and both
   `syscalls.h`, byte-identical layouts.
9. **Trusting a guard that no longer guards.** Two scripts (removed 2026-10-03)
   kept reporting success over code that no longer existed, and the host's
   CD-DA trim tests passed on a geometry the console no longer had. Prove a
   check can fail before trusting it. **A timeout is a check
   too**: the read deadline could not fire for two months because nothing
   started TMU2, and the first session that relied on it froze the game for
   7 s. **And a bound guards the loop it is in, not the function**: a repair
   loop with no budget below a carefully bounded one turned a silent console
   into an unbounded `SendBinQ` flood that froze the title.
10. **Testing the old image.** `make` does not regenerate the CDI, and
    regenerating does not deploy it. Run `mkdcdisc`, copy, then prove the
    deployed image contains the new `1st_read.bin` **by content** (search for
    its first 64 bytes), never by timestamp or size.
11. **Hard-coding an address the linker owns.** A GD-vector tripwire compared
    against a literal; an unrelated change moved the target and the machine
    rebooted at `EXEC` with no trace. Compare against the symbol.
12. **Adding DC-side state without checking `_end`** — §4.6, both families.
13. **Leaving an instrument on.** `GD_TRACE`/`GD_TRACE_CALLER` can stop a title
    booting. **And an instrument on a failure path is inside the blast radius**:
    a trace `write()` transmits, waits, and is fetched back with `SendBinQ`, so
    a trace fired on a failed read produced a read that could not be answered
    either. Failure-path traces are `gd_trace` (off); the counters say the same
    thing at no network cost.
14. **`REIOS: Booting up` in `flycast.log` is not a reset** — the SH4 executed
    address 0, e.g. a null jump. **Unless `SYS_MISC 1` precedes it**: then it is
    the title asking the BIOS for the menu (Katana `syBtExit()`), the disc boots
    again, and every counter read afterwards comes from *that* build (§14.19).
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
    use (§4.5). The hook's tick works only while the GD lock is free, and the
    hook re-verifies the title's VBR on GD syscalls (isoldr's
    `exception_vbr_ok()`).
21. **Reading a hardware field by the name in the comment.** AICA TL is
    attenuation; writing it as a volume silenced CD-DA while every counter was
    healthy. Check fields against a working driver (KOS `arm/aica.c`).
22. **Assuming an interrupt you armed is the one that fires.** A title's own
    masks may carry the same event on a higher level: the end of our BBA DMA
    came as the title's IML4, not our IML2 (§4.16). Read the title's masks
    (`g_irq_iml`) before choosing where to listen, and count the event at any
    level before concluding it does not fire.
23. **Measuring across the title on a clock the title can touch.** A wait that
    spans the title's own code — an asynchronous read, a period mark, a service
    gap — is only as good as a clock nobody else writes. TMU2 was ours until
    Sonic Adventure 2: its asynchronous reads "timed out" at the first look,
    one frame after the request, and the signature was
    a count that could not add up (65 deadline exits of 250 ms in 4.5 s of a
    one-at-a-time engine). Check the arithmetic of a timeout counter against
    wall time before chasing what it seems to report (§4.5 invariant 4).

## 15. Where to look first

- **Wire protocol** → `host-src/tool/commands.h`, `host-src/tool/syscalls.h`,
  `target-src/dcload/commands.{c,h}`, `target-src/dcload/syscalls.{c,h}`.
- **Throughput and latency** → `Makefile.cfg` FIFO delays, `GD_EMU_ASYNC`, the
  asynchronous reads (§4.5) and the host's pacing (§16). Upload loss was
  congestion, proven by `RT_RXMISSED`.
- **A title that stutters while it runs** → the per-chunk cost, not the total
  bandwidth: a title that streams reads during gameplay pays it inside a frame.
  Measure from the host's `debug!` timestamps for consecutive `ReadSector`
  requests and against `g_cdfs_sync_chunks`. **`--diag` is inside what it
  measures** (its `SBIQ` reads are answered from within a read's wait): take one
  run without it.
- **Base address / memory map** → `target-src/dcload/Makefile` (the layout
  comment) first, then `dcload.x.in`, `target-src/1st_read/`, `dcload-crt0.s`,
  `go.S`, `exception.S`, `commands.c` (`_dcload_base`), `maple.c`, `hiram.h`,
  and the host's `src/loaders.rs`. **Do not add a literal.**
- **GD read path** → headers of `cdfs_syscalls.c` and `cdfs_redir.s`, then
  `commands.c` (`cmd_loadbin`/`cmd_partbin`/`cmd_retval`, `bin_info`).
- **CD-DA** → the header of `cdda.c`, then §4.13.
- **Interrupt hook, asynchronous reads** → `irq.c`, `irq_hook.S`, the `ga_*`
  functions of `cdfs_syscalls.c`, §4.15.
- **Anything that runs while a game runs** → `_end` and §4.6.
- **A new example program** → copy one, add it to `example-src/Makefile`.
- **A new on-screen field** → `video.s`, `dcload.c`.
- **A new Maple or PMCR command** → README only; `dc-tool` forwards verbatim.
- **A new KOS** → check `utils/scramble` and the CI image tags.

## 16. The Rust host server

`dcload-ip-rs`, at `/mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/`, serves
GD-ROM and CD-DA emulation and has **its own `AGENTS.md`**: loader placement,
title patches (GAPS guard, GD driver body, Windows CE, KOS), `MARK`, `--diag`,
VM2, pacing and the CD-DA encoder are described there. `dc-tool-ip` remains the
reference for the basic protocol. **Never run `cargo` in the user's `target/`**
(it breaks their Windows build); use `CARGO_TARGET_DIR=<scratchpad>`.

**Contracts the DC side must keep in step with.** There are no feature bits:
the host chainloads its own `loaders/` for every disc image, so that directory
is redeployed with any change here (§4.11, §14.19).

- `MAX_XFER = 256 × CHUNK_SIZE` ↔ `BIN_INFO_MAP_SIZE` 256 (`commands.c`).
- **VERS**: after the version string's NUL, 4-byte fields read *forward*: the
  linked base, then the cable (0 VGA, 2 RGB, 3 composite). Append new fields at
  the end; older loaders send fewer.
- **Layout**: `loaders::layout()`, `live_footprint()`, `HIRAM_RESERVED`
  (`0x3000`), `LOADER_SPAN` (`0x10000`) mirror the Makefile layout (§4.11). The
  relocator assumes only `R_SH_DIR32`, linker-symbol addresses, and `.guestvbr`
  references only through the jump table, and repeats the link's stack asserts
  (every image section, `.gdstage` included, under `_stack`; 800 B from `_end`).
- **Symbols read by name** — renaming one breaks the host: the counters
  (`src/diag.rs`, `src/stackwatch.rs`, `scripts/dc-counters.py`); `_gd_stage`
  and `_gd_stage_big` (where a disc read may land inside the loader);
  `_g_gd_kos` (set to 1 before `EXEC` for a KOS binary, §4.5); `_gd_bios_entry`
  (`cdfs_redir.s`, written over a title's direct calls to the BIOS GD driver).
- **Disc reads** (`send_sectors()`): one at a time, **no LoadBinary echo, no
  DoneBinary probe** — the loader judges the chunk (§4.5). The LoadBinary names
  exactly the requested destination; the ReturnValue's address is
  `0x40000000 | LBA` (`READ_RETVAL_TAG` ↔ `GD_READ_TAG`). Above `MAX_XFER` the
  host falls back to `send_data()`, which verifies the echo and resends from
  the address `DoneBinary` reports.
- **One pause after the LoadBinary**: `cmd_loadbin` zeroes the part map and
  purges the cache over the whole destination, and parts arriving meanwhile
  overflow the ring — which costs the whole answer, not one part.
- **Runtime pacing** (6 packets / 600 µs) keeps a burst under the 16 KB RX ring:
  outrunning it desyncs CAPR from CBR. The ring is already at its back-pressure
  limit (`g_rx_overflow`, `g_rx_resync` non-zero with `g_rx_missed` 0); the
  residual loss is the status-word race of §4.8 rule 4, which costs a read its
  250 ms deadline.
- **Audio** (`DC23`/`DC24`): no echo or DoneBinary; ReturnValue `address` =
  LBA, `size` = trim ppm. The host's `CDDA_GIVE_UP` (15 ms) **must stay below
  the loader's 20 ms fetch deadline**, its encoder cache (`RECENT` = 48) must
  cover the ring (26 sub-fetches), and its trim thresholds assume a stream paced
  one sub-fetch every ~53 ms (§4.13 rule 10).
- **KOS titles**: `g_gd_kos` masks interrupts across every adapter wait, which
  is what makes it safe for the host to point KOS's dcload magic at the live
  loader; console writes then go as `DC25` (§8), which only this host knows.
- **`MAPL`'s argument block** and the signed response code (§8) are the
  interface `src/vm2.rs` is written against; the reply's `size` is the only
  length to trust.

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
| `sonic-adventure-investigation.md` | 2026-08: every cause eliminated, and the footprint root cause. |
| `cdda-crackle-investigation.md` | the first CD-DA engine (integrator). Transport and AICA measurements still hold. |
| `cdda-double-buffer-investigation.md` | the current engine brought up on hardware. |
| `wince-investigation.md` | Sega Rally 2 from black screen to playable, then the interrupt hook and asynchronous reads (§9), Katana titles included. |
| `g2-dma-investigation.md` | G2 DMA for the CD-DA rings and the BBA's RX. |
| `flycast-debug-loop.md` | the emulator-as-target workflow. |
| `read-back-verification.md` | what read-back verification proves and does not. |
| `dreamshell-presets/` | 6168 archived DreamShell presets. |
| `game-presets.tsv` | presets folded one row per game (generated by `scripts/make-preset-db.py`; the host reads its own copy). |
