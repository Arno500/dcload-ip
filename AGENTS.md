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
| `cdfs.h`, `cdfs_redir.s`, `cdfs_syscalls.c` | GD-ROM emulation (§4.5). Both carry explanatory headers. |
| `syscalls.c/.h` | host syscalls `DC00`–`DC24` (§8). |
| `commands.c/.h` | the command dispatcher and the LoadBinary window (`bin_info`). |
| `exception.S` | exception display **and** the VBR table handed to the title; `+0x600` is `nop; rte; nop` (no interrupt is hooked). |

### 4.3 Build flags

All in `target-src/dcload/Makefile`, which triggers a rebuild when edited.
GD constants are in §4.5, CD-DA flags in §4.13.

**Size knobs.** Footprint is correctness (§4.6). Measured 2026-09-12 as the
change in `_end` from the defaults of then (`_end = 0x8c00c58c`; since the CD-DA
simplification of 2026-09-19 and the paced fill of 09-20 the defaults give
`0x8c00bee0`):

| Flag | Default | Effect | `_end` saved |
| --- | --- | --- | --- |
| `PKT_BUFS_IN_HIRAM` | `1` | packet buffers and the CD-DA staging buffer in `.hiram` instead of BSS | 5440 B |
| `WITH_CDDA` | `1` | CD-DA engine (§4.13) | 5152 B if 0 (2026-09-20) |
| `WITH_LAN_ADAPTER` | `1` | HIT-0300 driver; 0 = BBA only | 2040 B if 0 |
| `WITH_PMCR_CMD` | `1` | serve `PMCR` (perfctr.c stays) | 808 B if 0 |
| `WITH_MAPLE` | `1` | serve `MAPL` | 656 B if 0 |
| `DCLOAD_GC_SECTIONS` | `1` | `--gc-sections`; also reveals dead code | 328 B |
| `DCLOAD_LTO` | `0` | `-flto`. **Off on purpose**: it changes the depth of the C frame the GD coroutine parks (§4.5 — check `g_gd_park_longs` on hardware) and suppresses the `*.asm` listings | 2428 B if 1 |

Combinations: `WITH_LAN_ADAPTER=0 WITH_MAPLE=0 WITH_PMCR_CMD=0` gives
`0x8c00b7e4`; adding `DCLOAD_LTO=1` gives `0x8c00adb0`.

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
| `0x8c00bee0` | `_end` with default flags (2026-09-20). Code and BSS are all below it. |
| `0x8c00f400` | `_stack` (LOW layout), **the VBR handed to the title**, the link address of `exception`, and the BIOS VBR. Only `_stack` moves with the base. |
| `0x8c010000` | the title's load address; `exception.bin` ends just before it. |
| `0x8cfe8000` | Maple DMA buffer (2 KB), outside the image (`_maple_dma_buffer`). |
| `0x8cfe9000` | `.hiram`, 12 KB reserved (NOLOAD, zeroed by crt0): packet buffers and CD-DA staging buffer. `dcload.x` asserts it does not reach the Maple buffer. |
| `0x8cf0c000` | post-mortem block (`PM_BASE`). **Advisory**: titles overwrite it. |

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
- `gdGdcGetCmdStat` reports progress (`COMPLETED` consumed once, then `IDLE`;
  `req_count` never 0 or 1). `gdGdcGetDrvStat` reports PLAYING while a read or
  CD-DA is live and calls `cdda_service()` **before** taking the GD lock.
- `CMD_REQ_STAT` and `CMD_GETSCD` report the CD-DA position while music plays.
  Unmodelled commands are force-completed, never failed (a title that gets
  FAILED for a routine command tends to give up).

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
   `GD_READ_DEADLINE_TICKS` 1.2 s for disc reads). The seconds timeout counts
   whole seconds on the PMCR (2 s fires at 3 s on hardware; never under a
   flycast without the local PMCR patch), `RTL_IDLE_POLL_LIMIT` only counts
   polls with no frame, and both are disarmed when `timeout_loop` is cleared
   under a wait. Arm, call, then clear — never clear someone else's.

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
| `GD_READ_DEADLINE_TICKS` | `15000000` | 1.2 s at Pck/4. |
| `GD_READ_RETRIES` | `4` | re-requests before failing a read. |
| `GD_SYSCALL_TIMEOUT_SECONDS` | `6` | coarse seconds backstop (fires at 7 s). |
| `GD_DRAIN_ITERS` | `0` | pre-request RX drain; measured useless at 256 and 50000, kept so nobody re-tests it blind. |
| `GD_SERVICE_ITERS` | `256` | used by `GD_SERVICE_EVERY_SYSCALL` (§4.3). |
| `GD_TRACE_CALLER` / `GD_TRACE_DEST_FROM` | `0` / `0xffffffff` | caller PC/SP tracing; expensive. |
| `GD_LOCK_STUCK_TICKS` | `3125000` | 250 ms, the watchdog threshold. |

**Known gaps:**

- **TMU2 is not started at boot in the default build.** The read deadline and
  the lock watchdog measure on it, but only `cdda.c` (before its first fetch)
  and `setup_machine()` (only with `ISOLDR_SETUP_MACHINE=1`) start it. Until a
  title plays music, those two bounds cannot expire.
- **No `g2_lock()` around CPU reads of the BBA.** `cdda.c` locks G2 for the
  AICA; the BBA ring is polled unlocked. One freeze dump showed AICA reads at
  zero together with `g_rx_hdr_defer` +6147 and resyncs, which fits a G2 burst
  collision. Before adding one, know that on Sonic Adventure under flycast
  (2026-08-09) no title G2 DMA was ever in flight at `rtl_bb_loop()` entry,
  and that a first attempt which *waited* on the DMA-busy bits rebooted the
  machine — observe before acting.
- `*_STREAM` commands complete without being served; `GETTOC2` does not model
  low/high density areas.

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

**Where that stands now:** since the CD-DA simplification of 2026-09-19 the
defaults give `_end = 0x8c00bee0`, **below** `0x8c00c000` again, but still
above Sonic Adventure's SP (`0x8c00b9d0`). Even the smallest build
(`0x8c00adb0`) leaves 3104 B under SA's stack, below the 4096 B the host
requires. The host places a title by these rules (§4.11, §16): it refuses a
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
   ORIGIN moves. HIGH: relative to the base — stack top `+0xb000`, `.hiram`
   `+0xc000` (12 KB), Maple DMA `+0xf000`, span `0x10000`.
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
  bases much above `0x8c008000`.

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

1. Never service from an interrupt, while `g_gd_in_transfer` is set, or while
   a title's G2 DMA into sound RAM is in flight.
2. Every key-on restarts the ADPCM encoder; any re-key goes through
   `cdda_prime()` (re-keying onto the existing ring decodes it from a reset
   decoder: full-scale noise).
3. The host's `CDDA_GIVE_UP` must stay below `CDDA_FETCH_DEADLINE_TICKS`.
4. Key on only over a whole lead; key off before the lead runs out. A byte the
   decoder plays out of sequence is not a click.
5. A fetch is complete only on its own window: close it before the request.
6. Measure with the true model; trigger with the lagged one. The lead is
   modulo one loop, so anything that could outlast it is judged on TMU2.
7. All AICA access inside `g2_lock()`, with a FIFO wait at most every eight
   32-bit stores (bounded at ~2 ms).
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
| `CDDA_FETCHES_PER_SERVICE` | `2` | sub-fetches per service call: the catch-up cap |
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
- The mixer level is read at key-on only.
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
| `-l` | — | legacy 1024-byte payload |
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
- **`DC24` `CMD_CDDAREAD_ADPCM`**: the same audio as 4-bit ADPCM, left block
  then right. value2 = frames (= bytes), **bit 31 = restart the encoder**.

Both are answered by LoadBinary/PartBinary into value1 and a **ReturnValue
whose `address` is the LBA served and whose `size` is the clock trim in ppm**
(§4.13). `cmd_retval()` latches `size` in `syscall_retsize`; every other
ReturnValue sends 0 there.

`LBIN`/`PBIN`/`DBIN` also carry disc sectors to a running title (§4.5).

**Known bug, unfixed (measured 2026-08-20 with `dcload-ip-rs
selftest-readback`):** writing with `PBIN` to a **P2** address (`0xac…`) and
reading it back with `SBIQ` at the same P2 address returns, from byte 8 on, the
previous transfer's bytes. Either direction alone, and the physical window
`0x0c…`, are fine (32/32). The suspect is `SH4_aligned_memcpy` in
`cmd_partbin`/`cmd_sendbinq`. Hosts should address RAM through `0x0c…`.

**A `DBIN` names its range.** `cmd_donebin()` answers with the first missing
part of the LoadBinary window (or `0, 0` when complete); `cmd_sendbinq()` ends a
memory read with a `DBIN` carrying the address and size it served. Without that
distinction a counter read during a transfer swallowed the transfer's `DBIN`
and credited a read with holes as complete. Hosts match on the ID only, so only
the filter that needs it sees the fields.

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
  `g_cdfs_read_retries`/`_fails`, `g_gd_in_transfer`, and the lock group
  (`g_gd_lock_stuck`/`_stuck_owner`/`_stuck_ticks`/`g_gd_lock_owner`/`_gen`).
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
- The post-mortem block at `PM_BASE` survives a reboot but not a title (§4.4).

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
10. **Testing the old image.** `make` does not regenerate the CDI, and
    regenerating does not deploy it. Run `mkdcdisc`, copy, then prove the
    deployed image contains the new `1st_read.bin` **by content** (search for
    its first 64 bytes), never by timestamp or size.
11. **Hard-coding an address the linker owns.** A GD-vector tripwire compared
    against a literal; an unrelated change moved the target and the machine
    rebooted at `EXEC` with no trace. Compare against the symbol.
12. **Adding DC-side state without checking `_end`** — §4.6.
13. **Leaving an instrument on.** `GD_TRACE`/`GD_TRACE_CALLER` can stop a title
    booting.
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
20. **Expecting an interrupt hook to keep the music playing.** A CD-DA fetch
    transmits, and an interrupt can land while `pkt_buf` or `bin_info` is in
    use (§4.5), so an IRQ-driven service could only push what is already staged.
    A real VBR hook would also need re-verifying the title's VBR (isoldr's
    `exception_vbr_ok()`) and a continuation outside the vector entries
    (`VBR+0x5f0` is free; Sonic Adventure writes `+0x600`).
21. **Reading a hardware field by the name in the comment.** AICA TL is
    attenuation; writing it as a volume silenced CD-DA while every counter was
    healthy. Check fields against a working driver (KOS `arm/aica.c`).

## 15. Where to look first

- **Wire protocol** → `host-src/tool/commands.h`, `host-src/tool/syscalls.h`,
  `target-src/dcload/commands.{c,h}`, `target-src/dcload/syscalls.{c,h}`.
- **Throughput and latency** → `Makefile.cfg` FIFO delays, `GD_EMU_ASYNC`, and
  the host's pacing (§16). A disc read blocks the title for its whole duration,
  so milliseconds per chunk matter more than KB/s. Upload loss was congestion,
  proven by `RT_RXMISSED` (887 drops in one upload before windowed flow
  control).
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
- **Relocates and chainloads** `loaders/dcload-relocatable.elf` (§4.11), always
  via `SCRATCH_BASE` `0x8ce00000`, and replaces a running loader when its image
  differs from the file even at the same base.
- **Patches titles** before upload: the GAPS guard (§4.12), `--vga` (forces the
  PDTRA cable read; `auto` uses the cable dcload reports), PPF patches
  (`patches/`, checked against the PPF blockcheck).
- **`--diag`**: the counter panel (`d` toggles, `w` writes `dcload-diag.txt`;
  the header shows the measured sample interval, not the requested one).
  `stackwatch` reads `g_gd_sp_min` every 10 s in every session.

**Contracts the DC side must keep in step with:**

- `MAX_XFER = 256 × CHUNK_SIZE` ↔ `BIN_INFO_MAP_SIZE` 256 (`commands.c`).
- **VERS**: after the version string's NUL, 4-byte fields read *forward*: the
  linked base, then the cable (0 VGA, 2 RGB, 3 composite). Append new fields at
  the end; older loaders send fewer.
- **Layout**: `loaders::live_footprint()`, `HIRAM_RESERVED` (`0x3000`),
  `LOADER_SPAN` (`0x10000`) mirror the Makefile layout table; the relocator
  assumes only `R_SH_DIR32`, linker-symbol addresses, and `.guestvbr`
  references only through the jump table (§4.11).
- **Counter names** are read from the ELF by name (`src/diag.rs`,
  `src/stackwatch.rs`, `scripts/dc-counters.py`); renaming one breaks them.
- Disc reads are served **one at a time**, to completion, inside the request
  handler. `send_data()` verifies the LoadBinary echo; recovery resends a run
  from the address `DoneBinary` reports, doubling up to 64 parts (resending a
  part is harmless).
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
| `flycast-debug-loop.md` | the emulator-as-target workflow. |
| `read-back-verification.md` | what read-back verification proves and does not. |
| `dreamshell-presets/` | 6168 archived DreamShell presets. |
| `game-presets.tsv` | presets folded one row per game (generated by `scripts/make-preset-db.py`; the host reads its copy in its own project root, beside `game-memory.tsv`). |
