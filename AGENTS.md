# dcload-ip — Agent Notes

> This file is the operating manual for AI coding agents working in this
> repository. It describes **the code that is present**. Where a fact was
> expensive to learn, the measurement is kept with it; where a section used to
> carry a running investigation log, that log now lives in `docs/`.
>
> If this document conflicts with the code, the code wins — see §17 for the
> files to check first.

## 1. What this project is

`dcload-ip` is a **Sega Dreamcast network loader**. Two halves cooperate over
UDP:

1. A **Dreamcast-side** program (`dcload`, running on the SH4 at
   `0x8c004000` after the 1st_read bootstrap copies it there — §4.4). It
   implements ARP, ICMP, UDP and an optional IPv4 DHCP client on the BBA
   (`HIT-0400`) or LAN Adapter (`HIT-0300`). It accepts commands to upload
   (`LBIN`/`PBIN`/`DBIN`), execute (`EXEC`), read memory (`SBIN`/`SBIQ`),
   service a GDB stub, pass Maple packets through (`MAPL`) and drive the SH4
   performance counters (`PMCR`). It also **emulates the GD-ROM drive** for a
   launched title, serving disc sectors from an image on the PC (§4.5).
2. A **PC-side** program (`dc-tool-ip`, a single C binary on plain BSD sockets)
   that talks to dcload, ships and reads files, and proxies GDB.

Fork of KallistiOS' `dcload-ip`, overhauled by Moopthehedgehog, maintained by
Mickaël Cardoso (SiZiOUS) and contributors. License **GPLv2** (`COPYING`).

Version is `2.0.4`, in `Makefile.cfg` **only** — both Makefiles consume it as
`-DDCLOAD_VERSION`. (`README.md` still says 2.0.2 and `CHANGES` stops at 2.0.1;
they lag, `Makefile.cfg` is authoritative.)

There is **no autotools, CMake, Meson or configure script**. Hand-written GNU
Makefiles include `Makefile.cfg` and `Makefile.hostdetect` from the repo root.

## 2. Repository layout

```
.
├── Makefile                # top-level driver: include Makefile.cfg + recurse
├── Makefile.cfg            # toolchain / IP / tunables / VERSION — the one file users edit
├── Makefile.hostdetect     # sets MINGW, MINGW32, MINGW64, CYGWIN, MACOS, BSD, WINDOWS
├── AGENTS.md               # this file (CLAUDE.md is a symlink to it)
├── README.md               # user-facing docs; CHANGES; NETWORK; COPYING
├── Cdif131e.pdf / .txt     # SPI / GD-ROM command spec — reference, do not edit
├── .gdbinit                # arch sh4, endian little, target :3263
├── .gitlab-ci.yml          # primary CI (two gcc jobs)
├── .github/workflows/      # sync-to-gitlab.yml — mirrors master → GitLab
├── .vscode/                # sh-elf-gdb attach config to :3263
├── host-src/tool/          # the only host code; produces dc-tool-ip
├── target-src/dcload/      # the main DC binary → dcload.bin + exception.bin
├── target-src/1st_read/    # CD bootstrap → scrambled 1st_read.bin
├── target-inc/             # header-only include path (no Makefile of its own)
├── example-src/            # 3 demo .bin programs uploaded with dc-tool -x
├── make-cd/                # burn a real CD-R (wodim + genisoimage)
├── make-cdi/               # produce a .cdi image (mkdcdisc)
├── docs/                   # see §17
└── scripts/                # debugging instruments, see §11
```

Things that surprise first-time agents:

- `host-src/machine/` and `host-src/sys/` do **not** exist here, even though
  some KOS-style projects use those paths.
- `target-inc/` has no Makefile — it is an include path, added as
  `-I../../target-inc` (dcload) and `-I../target-inc` (examples).
- The shipped Dreamcast artifacts are `target-src/dcload/dcload.bin`,
  `target-src/dcload/exception.bin` and `target-src/1st_read/1st_read.bin`.
  The ELFs and `dc-tool-ip` are gitignored.
- `*.asm` files sitting next to `.o` files are **build output** (assembly
  listings), not hand-written. See §6.
- `target-src/dcload/scif.c` and `scif.h` (SCIF serial console, inherited from
  the serial loader) are **not linked** — `scif.o` is absent from `DCLOBJECTS`
  and every `scif_puts` call site is commented out. Likewise `bswap.s` is
  unbuilt; `bswap.h` carries the inline versions actually used.

## 3. Build system

### 3.1 Toolchain assumptions

- Expects a **KallistiOS SH-ELF toolchain** (KOS `dc-chain`). The prefix comes
  from `$(KOS_CC_BASE)`, set by KOS's `environ.sh`.
- Always `source` KOS `environ.sh` before `make`, or every cross target fails
  with "command not found".
- `target-src/1st_read/Makefile` shells out to
  `$(KOS_BASE)/utils/scramble/scramble`. That binary comes from KOS; if you
  built KOS from source (as GitLab CI does), `make` it once in
  `$KOS_PATH/utils/scramble`.
- A non-KOS sh4-elf toolchain works if you comment out `USING_KOS_GCC` in
  `Makefile.cfg`. The alternate block is hard-coded under
  `/mnt/c/DreamcastKOS/` — adjust it.
- The host tool is plain `gcc`; it needs no SH cross toolchain.
- The tree builds under **GCC 15** (the implicit-`write()` error was fixed by
  including `<unistd.h>` in `cdfs_syscalls.c`).

### 3.2 Host detection (`Makefile.hostdetect`)

Sets `BSD` (host ends in `BSD`), `MACOS` (`uname -s` = `Darwin`), and
`MINGW`/`MINGW32`/`MINGW64`/`CYGWIN`/`WINDOWS` by matching the `uname -s`
prefix (`MINGW*`, `CYGWIN*`); `MINGW64` is further distinguished via MSYS2's
`$MSYSTEM_CHOST`. Do not duplicate this logic — `include` the file.

### 3.3 Root Makefile

A 32-line driver: `SUBDIRS = host-src target-src example-src`, `target-src:
host-src`, `install:` recurses into `host-src/tool`. The subdir order matters:
`1st_read` `.incbin`s `dcload.bin` and `exception.bin`, so `dcload` must be
built first.

### 3.4 Common build commands

```sh
source /opt/toolchains/dc/kos/environ.sh
make                          # host tool + dcload + 1st_read + examples
sudo make install             # dc-tool-ip → $(TOOLINSTALLDIR)

make -C host-src/tool         # incremental sub-builds
make -C target-src/dcload
make -C target-src/1st_read

make clean                    # objects, maps, *.asm
make distclean                # also *.bin
```

### 3.5 `Makefile.cfg` — the one file that changes between machines

- `USING_KOS_GCC = 1` — KOS toolchain (default). Comment out for a standalone
  sh4-elf-gcc.
- `HOSTCC` / `HOSTCFLAGS` / `HOSTLDFLAGS` — `gcc -Og`; auto-adds `-D_WIN32`
  and `.exe` on Windows.
- `TARGETCFLAGS = -Os -ml -m4-single-only` (KOS mode).
- `TOOLINSTALLDIR = /opt/toolchains/dc/bin`.
- `WITH_BFD` — `0` links dc-tool against `libelf` (default), `1` against
  sh-elf `libbfd` + `libiberty`; auto-flipped to 1 under MinGW. macOS takes
  `libelf` from `/opt/homebrew`.
- `BFDLIB` / `BFDINCLUDE` / `ELFLIB` / `ELFINCLUDE` — must point at the
  **sh-elf** headers, not the host's system BFD.
- `TARGETCCVER` — auto-detected and clamped to 4; `example-src/Makefile` picks
  `dc$(TARGETCCVER).x`, so on modern toolchains always `dc4.x`.
- `VERSION = 2.0.3`.
- `STANDALONE_BINARY` — set to 1 only under `WINDOWS`; adds `-static`.
- `SAVE_MY_FANS = 0|1` — slows dc-tool's poll loop to cut laptop CPU.
- `EXCEPTION_SECONDS = 15` — on-screen register-dump duration (0–60).
- `DREAMCAST_BBA_RX_FIFO_DELAY_{COUNT,TIME}` + the `…_LAN_…` pair — burst
  pacing for dc-tool → dcload. Raise `TIME` by ~100 µs at a time if you see
  `link change…` mid-transfer (BBA) or the LAN adapter hanging. Presets are in
  the file's comment block.
- `DREAMCAST_IP = 192.168.1.130` — **static, on purpose**. Any `0.x.x.x`
  switches to DHCP. See §4.7 on reachability, and §14.6 (`make clean` is
  mandatory after changing it).

## 4. The Dreamcast binary (`target-src/dcload`)

### 4.1 Artifacts

- `dcload` — final ELF, linked with `dcload.x`. `dcload.bin` is ~25 KB.
- `exception` — standalone ELF from `exception.S` alone, `-Ttext=0x8c00f400`.
  `exception.bin` is exactly 2048 bytes.
- Both `.bin`s are consumed by `target-src/1st_read/loader.s` via `.incbin`.
- `dcload.map` / `exception.map` — link maps. Read them; §4.6 depends on them.
- Per-source `*.asm` listings (§6).

### 4.2 Source list

Compiler flags (in `target-src/dcload/Makefile`) disable everything that could
relocate or reorder code behind your back:

```
-Wall -Wextra -ffreestanding -std=gnu11
-fno-zero-initialized-in-bss -fno-common -fomit-frame-pointer
-fno-strict-aliasing -fno-unwind-tables -fno-asynchronous-unwind-tables
-fno-exceptions -fno-delete-null-pointer-checks -fno-stack-protector
-fno-stack-check -fno-merge-constants -fno-merge-all-constants
```

`OBJCOPY` strips `.stack` (`-R .stack`). Link:
`-Wl,--warn-common -Wl,--no-undefined -Wl,-Map,dcload.map -Wl,-Tdcload.x
-nostartfiles -nostdlib -static -Wl,-z,now`, plus `-lgcc` for the SH4 helpers.

| File | Role |
| --- | --- |
| `dcload-crt0.s` | start of day: stack, **VBR install**, zero BSS (`_edata`..`_end`, one contiguous range), call `main`. Also holds the fixed jump table and the `0xdeadbeef` magic the example programs check. |
| `dcload.c` | `main`: adapter detection, DHCP retry, lease-time perfcounter, video background, the command loop, `announce_presence()`. |
| `go.s` / `go.h` | the handoff to a launched game — SR, CCR, entry. **Read its header before touching it** (§4.7). |
| `disable.s` / `disable.h` | turns off the SH4 cache (must run from P2). |
| `startup_support.c` | BSS zeroing helpers and C++ constructors. |
| `video.s` / `video.h` | on-screen status output (Marcus Comstedt's `video.s`). |
| `packet.c/.h` | packet builder/parser; `bswap.h` supplies `ntohl`/`htons`. |
| `net.c/.h` | ARP/ICMP/UDP glue, adapter detection, `announce_presence()`. |
| `adapter.c/.h` | abstract BBA / LAN-Adapter driver interface (`bb`). |
| `hiram.h` | `HIRAM_BUF` — the attribute that puts a buffer in `.hiram` (high RAM) instead of BSS, and the argument for doing so. Read it before adding any large buffer. |
| `rtl8139.c/.h` | BBA driver, including the RX ring (§4.8). |
| `lan_adapter.c/.h` | LAN Adapter driver. |
| `dhcp.c/.h` | IPv4 DHCP client with the retry counter the README explains. |
| `perfctr.c/.h` | SH4 performance counters; counter #1 tracks the DHCP lease. |
| `memfuncs.c/.h`, `memcpy.S`, `memcmp.c` | hand-written aligned mem* fast paths. |
| `maple.c/.h` | Maple bus driver (Marcus Comstedt). Its DMA buffer is **not** in BSS — §4.6. |
| `cdfs.h`, `cdfs_redir.s`, `cdfs_syscalls.c` | GD-ROM drive emulation (§4.5). `cdfs_redir.s` is the syscall trampoline and the coroutine park/resume; `cdfs_syscalls.c` is the server. Both carry long explanatory headers — read them first. |
| `syscalls.c/.h` | host-issued `DC00`–`DC22` syscall handlers. |
| `commands.c/.h` | the `EXEC`/`LBIN`/`PBIN`/`DBIN`/… dispatcher and `bin_info`. |
| `exception.S` | the on-screen exception display **and** the VBR table dcload hands the game. Its `+0x600` interrupt vector is `nop; rte; nop` — dcload hooks no interrupt, same as DreamShell isoldr's `net` build. |

### 4.3 Build flags

`target-src/dcload/Makefile` carries one tracing flag and a block of size
knobs. The size knobs all buy the same thing — distance under a launched
title's stack (§4.6) — so they are listed with what they cost:

| Flag | Default | What it does |
| --- | --- | --- |
| `GD_TRACE` | `0` | Trace the GD request/answer contract to the host console (`dcload-ip-rs` displays it). **Off by default because each traced event is a full UDP round trip**, which perturbs the very timing you are measuring — with caller tracing on, Sonic Adventure fires ~120 events/s and stops booting. See `cdfs_syscalls.c`. |
| `DCLOAD_GC_SECTIONS` | `1` | `-ffunction-sections -fdata-sections -Wl,--gc-sections`. No behavioural change; the only symbols it drops today are genuinely dead (`memmove`, `memcmp`, `PMCR_RegRead`, a shadowed `loop_secs_elapsed` global). |
| `PKT_BUFS_IN_HIRAM` | `1` | Put `raw_pkt_buf` and `raw_current_pkt` (1536 B each) in the `.hiram` NOLOAD section at `0x8cfe9000` instead of BSS. **−3088 B of `_end` (3072 B of buffer plus alignment), more than every other knob combined.** `hiram.h` states the trade. |
| `WITH_LAN_ADAPTER` | `1` | Build the HIT-0300 LAN Adapter driver. 0 → BBA only, `adapter_detect()` stops probing for it. −2040 B. |
| `WITH_MAPLE` | `1` | Serve `MAPL` and build `maple.c`. −640 B. |
| `WITH_PMCR_CMD` | `1` | Serve `PMCR`. Does **not** remove `perfctr.c` — dcload uses counter 1 itself for the DHCP lease and the adapter loop timeouts. −808 B. |
| `DCLOAD_LTO` | `0` | `-flto`. −1856 B. **Off on purpose**: it inlines across translation units, which changes the depth of the C frame the GD coroutine parks into a 96-long buffer (§4.5), and it makes `lto-wrapper` discard the `-Wa` options so the `*.asm` listings (§6) stop being produced. Neither is checkable at build time — if you turn it on, read `g_gd_park_longs` on a real boot before trusting the build. |

Measured with `_end` from `dcload.map`, one knob at a time from the defaults.
Stock (everything off, buffers in BSS) is `_end = 0x8c00b588`; the defaults
above are `0x8c00a558`; `DCLOAD_LTO=1 WITH_LAN_ADAPTER=0 WITH_MAPLE=0
WITH_PMCR_CMD=0` is `0x8c009068`, which is 10600 B of margin under Sonic
Adventure's syscall SP against the stock 1096.

One more, a `#define` in `rtl8139.c` rather than the Makefile (override with `-D`):

| Flag | Default | What it does |
| --- | --- | --- |
| `RTL_WARM_START` | `1` | Adopt a BBA a previous dcload already brought up instead of re-initialising it (§4.9). Set to 0 to always take the cold path; costs ~512 B of image. |

`EXCEPTION_SECONDS`, `DREAMCAST_IP` and `VERSION` reach the compile from
`Makefile.cfg`. Everything else that used to be a Makefile flag is now a
constant in `cdfs_syscalls.c` (§4.5) — the `%.o` rules depend on both Makefiles,
so editing either does trigger a rebuild.

### 4.4 Memory map

Pinned by `dcload.x`: `ram (rwx) : ORIGIN = 0x8c004000, LENGTH = 0xb400`.

| Address | What |
| --- | --- |
| `0x8c004000` | dcload's base. `+4` is the `0xdeadbeef` magic, `+8` the syscall trampoline pointer — the ABI the example programs use. |
| `0x8c00a558` | `_end` (current, default knobs — §4.3). The whole loader, code and BSS, is below this. Sonic Adventure enters GD syscalls with `SP = 0x8c00b9d0` and grows *down* — that is the margin §4.6 is about, and it is what any addition spends. |
| `0x8c00f400` | `_stack`, **and** the VBR handed to the game, **and** the base of `exception` (`-Ttext=0x8c00f400`), **and the BIOS VBR on this machine**. |
| `0x8c010000` | the game's load address. `exception.bin` (2048 B) ends before it; total footprint `0xc000`, exactly the hole between the BIOS syscall area and 1ST_READ.BIN. |
| `0x8cfe8000` | Maple DMA buffer (2 KB), deliberately outside the loader image (§4.6). Hard-coded in `maple.c`. |
| `0x8cfe9000` | `.hiram` — the two 1536-byte packet buffers, also outside the image. Placed by `dcload.x`, `NOLOAD`, so it costs nothing in `dcload.bin` and nothing in `_end`; `dcload-crt0.s` zeroes it explicitly because BSS zeroing no longer covers it. Empty when `PKT_BUFS_IN_HIRAM=0`. Put a new large buffer here — mark it `HIRAM_BUF`, see `hiram.h` — rather than in BSS. |
| `0x8cf0c000` | post-mortem block (`PM_BASE`, `cdfs_syscalls.c`). **Advisory only** — at the low base nothing caps a title's allocator there, and Sonic Adventure re-claims it. The caveat is written at the definition. |

Two link-time asserts guard this, and they are the whole safety net:

```
ASSERT((_stack - _end) > 800, "Not enough stack space")
ASSERT(_end <= ORIGIN(ram) + LENGTH(ram), "Region 'ram' overflowed")
```

There is **no resident/transient split** any more, and no `_resident_end`:
everything in the image is live while a game runs, and `crt0` zeroes one
contiguous BSS range. (`scripts/check-resident-invariant.sh` still assumes the
old split and no longer reports anything meaningful — see §14.9.)

Why this base, in order of weight:

1. **DreamShell's per-game database prescribes it.** All four Sonic Adventure
   presets carry `memory = 0x8c000100` or `0x8c004000` (plus `dma = 0`,
   `async = 8`, `irq = 0`). Across all 6168 presets `0x8cf00000` appears
   **zero** times. Archived in `docs/dreamshell-presets/`.
2. **`0x8c00f400` is the BIOS VBR**, and `exception.bin` lands exactly there
   from this base — the same VBR isoldr hands every title. From high RAM we
   were handing out an address no retail title has ever seen.
3. It is the loader's original address; the example-program ABI
   (`0x8c004004` / `0x8c004008`) is correct here for free.

`crt0` still installs VBR explicitly at boot even though the BIOS VBR now
points at the right place — keep it, so the next relocation does not
reintroduce a silent reset loop.

### 4.5 GD-ROM emulation: a server task, on isoldr's model

`cdfs_redir.s` + `cdfs_syscalls.c` emulate the BIOS GD driver so a launched
title reads its disc over UDP. **The structure is not a matter of taste**: the
driver a title is written against is a *coroutine*, not a function.

- `gdGdcReqCmd` queues, answers `PROCESSING`, returns a channel. No I/O.
- `gdcServerMain` is an endless dispatch loop, entered once and never left.
  `gdGdcInitSystem` parks the caller and runs it; the first `gdGdcExecServer`
  reaches it lazily, so a title that never calls InitSystem still gets a server.
- `gdGdcExecServer` resumes the server; `gdcExitToGame()` parks it again. One
  hardware stack; the inactive side's frame is copied into `saved_regs[]` (96
  longs, of which 11 are overhead). `g_gd_park_longs` publishes the depth used
  — anything approaching 80 means enlarge the buffer.
- `data_transfer` reads in chunks of `GD_EMU_ASYNC` sectors (isoldr's
  `emu_async`).
- `gdGdcGetCmdStat` reports progress; `COMPLETED` is consumed once, then
  `IDLE`. `req_count` follows the BIOS sequence and is never 0 or 1.
- `gdGdcGetDrvStat` reports PLAYING while a read is live, PAUSED otherwise.

Two invariants written on the code, both paid for:

- **A TX is only safe at the top level of a syscall.** `pkt_buf` is single and
  shared, and `bb->loop()` dispatches incoming commands that build into it.
  Never transmit nested inside a `bb->loop()`. A corollary that cost a whole
  session: emit any trace **before** building the command, or the trace's own
  `write()` overwrites it in `pkt_buf` and the host never sees the request.
- **No function live across a yield may take the address of a local.** A parked
  frame is restored onto whatever `r15` the next ExecServer arrives with.

Tuning constants, all in `cdfs_syscalls.c` (override with `-D`):

| Constant | Value | Note |
| --- | --- | --- |
| `GD_EMU_ASYNC` | `8` | Sectors per host request = 16 KB, exactly the BBA RX ring, and DreamShell's SA preset. |
| `GD_BULK_SECTORS` | `0` | isoldr reads ≥100-sector requests in one shot; **do not re-import that rule**. It turns a 105-sector read into 150 back-to-back packets into an 11-frame ring: measured 840 lost packets in one run. isoldr can do it because its transports are DMA with no ring. |
| `GD_YIELD_BETWEEN_CHUNKS` | `0` (undefined) | Chunk on the wire, **atomic to the game**. Each 16 KB piece fits the ring, but the title is never handed control mid-read. With the yield, SA died on delivery of chunk 1. |
| `GD_SYSCALL_TIMEOUT_SECONDS` | `6` | Deliberately shorter than the host's ~4 s give-up plus margin, so a retry can actually happen instead of us still being blocked when the host is ready again. |
| `GD_READ_RETRIES` | `4` | Re-requests before failing a chunk. |
| `GD_DRAIN_ITERS` | `0` | Bounded `bb->loop()` before each request. Left at 0 **and left in the tree**: it was measured at 256 and 50000 iterations and moves nothing, so the next person to suspect a stale ring can see it was tested. |
| `GD_TRACE_CALLER` | `0` | Trace the caller's PC/SP. This is what found the root cause in §4.6 — and it is also expensive enough to stop a title booting. Diagnostic only. |
| `GD_TRACE_DEST_FROM` | `0xffffffff` | Trace only reads landing at or above this destination. |

Known gaps, none exercised by the titles tested: the `*_STREAM` commands fall
through to COMPLETED rather than being served or refused; `GETTOC2` does not
emulate a real GD's low/high density areas; there is no `g2_lock()` around CPU
reads of the BBA.

### 4.6 The footprint rule — why the map looks like this

**A retail title will use the BIOS work area as a stack, because from its point
of view that is free memory. That area is where dcload lives.**

Measured, not inferred: Sonic Adventure enters GD syscalls with
`SP = 0x8c00b9d0` and its stack grows *down*. When `_end` was `0x8c00e7c0`,
that SP landed inside dcload's BSS, ~2.4 KB above `bb` — the adapter pointer.
Once `bb` was overwritten, dcload's next `bb->loop()` was an indirect call
through garbage, the guest executed at address 0, and flycast logged
`REIOS: Booting up`. It was also mutual: dcload writing `bin_info.map` wrote
into the title's live stack.

Two changes moved `_end` from `0x8c00e7c0` to `0x8c00b2a8` and made Sonic
Adventure boot and play (1624 reads served, no reset):

1. **`BIN_INFO_MAP_SIZE` 11656 → 256** (`commands.c`) — 11.4 KB of packet map.
   The host splits any transfer larger than `MAX_XFER = 256 * CHUNK_SIZE` into
   successive LoadBinary transfers, so **these two constants must stay in
   step** (`MAX_XFER` lives in the Rust host, §16). Only the initial upload
   ever needed the old size; the largest in-game read is 215040 bytes.
2. **Maple `dmabuffer` moved to `0x8cfe8000`** (`maple.c`) — 2 KB more, and it
   was the buffer the SP actually landed in. That address is isoldr's own
   free-high-RAM heuristic.

A later pass took `_end` from `0x8c00b588` to `0x8c00a558` at default settings
— margin 1096 B → 5240 B — with no feature removed. In order of weight:

3. **Both packet buffers left BSS** for `.hiram` at `0x8cfe9000` (§4.4),
   −3088 B. They were the last large objects in the image, and BSS is the part
   of the image *closest* to a descending stack.
4. **The 64-bit division went away** (`PMCR_Delta_Seconds`, `perfctr.h`), −928 B.
   Two call sites divided a perf-counter delta by 200000000 to get seconds;
   at `-Os` GCC emitted a call to `__udivdi3`, which pulled in
   `__udiv_qrnnd_16`, `__clz` and the 256-byte `__clz_tab`. **Watch for this
   whenever a `long long` meets a `/` or `%`** — it is the one construct that
   can add most of a kilobyte to this image from a single line, and nothing in
   the build warns about it. Check with `sh-elf-nm dcload | grep libgcc`-ish
   symbol names, or `grep libgcc dcload.map`.
5. **`--gc-sections`**, −160 B — and it is also the check that tells you when
   something has become dead.

The remaining knobs (§4.3) trade features for another ~4.5 KB.

So, when adding anything to the DC side:

- **Footprint is a correctness property, not a nicety.** Check `_end` in
  `dcload.map` after any change that adds state. isoldr's network build is
  13 KB; we are ~21 KB of image and filling a hole a title expects to own.
- Prefer putting large buffers **outside the image** — mark them `HIRAM_BUF`
  (`hiram.h`) and they land in `.hiram` — over growing BSS.
- Keep small hot state — `bb` above all — as far as possible from where a
  title's stack roams. Note this is **not** where `bb` sits today: it is the
  last object in BSS, so it is the first thing a descending stack reaches.
  That is inherited link order, not a decision; ordering BSS deliberately is
  free and has never been done.
- A guard is cheap: latching the SP seen at syscall entry and counting it when
  it falls inside `[0x8c004000, _end)` would have found this in minutes.

The full investigation, including every cause eliminated by measurement, is
`docs/sonic-adventure-investigation.md`. Read it before re-suspecting the
transport: destination address, payload content, chunk size, host pacing, the
RX ring, the GD state machine and the coroutine were each ruled out
individually, and all of them were downstream of this.

### 4.7 Handoff to a game, and reachability

- **`go.s` hands `SR = 0x60000101`** — MD=1, RB=1, BL=0, IMASK=0. It used to
  load `0x500000f0` (BL=1, IMASK=15), which masked every interrupt level and
  blocked exceptions, so a title could take neither a VBlank nor a Maple
  completion until it cleared those bits itself, and any exception it did take
  became a manual reset. RB=1 flips the register bank, so the entry address
  must leave `r4` before SR is written. The reasoning is on the file.
- **Caches are off at handoff** (`CCR = 0x0808`), written from inside `go()`
  running in P2 with the required settling window. This is a **deliberate
  divergence** from isoldr, which hands over with caches on: its transports are
  DMA, so the device writes the game's buffer and the loader purges afterwards,
  whereas dcload fills those buffers with CPU stores while the game reads them
  back uncached. Caches off makes that coherent by construction.
- **`announce_presence()` (net.c)** sends a gratuitous ARP from the main loop.
  Without it a static `DREAMCAST_IP` is unreachable *by construction*: the
  host's first packet is unicast so it must ARP, flycast's BBA bridge does not
  open its capture device until the guest has transmitted once, and the host's
  neighbour entry decays to `Unreachable` — a state in which Windows discards
  datagrams while `send()` reports success. DHCP hid this, because DISCOVER is
  that first frame.

### 4.8 RX ring rules (`rtl8139.c`)

Five rules, each of which the stock tree violated and each of which is a real
defect:

1. **`RT_INTRMASK` must be programmed.** The mask decides whether the RX status
   bits are observable at all; with it at 0 the `RT_INT_RX_ACK` arm of the poll
   loop is dead code.
2. **Keep an ungated `RxBufEmpty` fallback**, or frames that never re-assert
   RxOK are invisible.
3. **Never publish `CAPR` out of range on wrap.** `0x7ff0` tells the chip
   everything is drained; it moves its pointer to CBA and discards the queue.
   Measured in exactly that state: CAPR 6340 ahead of CBR 2212 with RxBufEmpty
   clear, dcload receiving nothing while the emulator reported `dropped=0`.
4. **Check header plausibility.** The chip writes the status word last, so an
   early read yields the previous occupant's bytes; a length taken from that
   advances `cur_rx` arbitrarily and desynchronises the ring for good.
5. **Overflow is normal back-pressure, not a fault.** The ring is 16 KB and a
   chunked read pushes ~18 KB through it. Drain first; re-initialise only if
   the ring refuses to empty.

And: **every hardware wait must be bounded.** The "link change" branch used to
reset the PHY and spin on `RT_MII_BMSR & 0x20` with nothing guaranteeing it
arrives. Measured: the poll loop fell from ~45000 iterations per 0.3 s to
**one** per 0.26 s while frames froze — dcload nominally in `bb->loop()` but
never looking at the ring again. Both waits are now capped by
`RTL_LINK_SPIN_LIMIT`.

### 4.9 Warm start — chainloading dcload from dcload

Uploading a new dcload from a running one and `EXEC`ing it re-runs
`adapter_detect()`, and the cold path is **not** a formality: `rtl_bb_detect()`
powers GAPS down, `rtl_bb_init()` powers it back up, clears the 32 KB SRAM,
soft-resets the chip twice and **restarts auto-negotiation** (`BMCR 0x9200`).
The link drops and comes back, which costs seconds by itself — and much more
when a link-change event lands before auto-negotiation has finished, because
`rtl_bb_loop()`'s PHY-reset branch then restarts auto-negotiation *again*. That
is the second instance that "sometimes takes very long", sends its DHCP
DISCOVERs into a link that is not forwarding yet, and sits on
`Waiting for IP...` while uploads work anyway (commands are matched on the
**MAC**, and `our_ip` is then taken from the packet's own destination — see
`cmd_loadbin()`; that is also why the host can still reach a DC that never got
a lease).

So dcload now looks at the adapter before touching it:

- **`rtl_warm_usable()` runs inside `rtl_bb_detect()`, before the "GAPS off"
  write** — after that write the only way back is the full cold init. It
  compares GAPS enable/SRAM base, `RXBUF`, all four `TXADDR`, `RXCONFIG`
  (accept bits excused: `cmd_execute()` calls `bb->stop()` on the way out),
  `TXCONFIG` masked to the bits we set, `INTRMASK`, `CHIPCMD`, and requires
  BMSR link + auto-negotiation. A chip that has been reset — or powered on —
  reads zero for most of those, so a cold adapter cannot pass by accident.
  Anything that does not match falls through to the cold path.
- **`rtl_warm_adopt()` replaces `rtl_bb_init()`**: re-read the MAC (a register
  read, no reset), jump `CAPR` to the chip's own `CBR` to **discard the ring
  backlog**, reset `cur_tx`, clear stale `INTRSTATUS`/`RXMISSED`, and re-enable
  the RX accept bits. Discarding matters: the host retransmits a command whose
  ack it never saw, so the frame most likely sitting there is a duplicate of
  the `EXEC` that started us — and `cmd_execute()` runs it, because `running`
  is 0 in a fresh image. That is an infinite reload loop.
- **`rtl_bb_loop()` must not clear `rtl_link_up` on a warm start.** No link
  change is coming, and `set_ip_dhcp()` is gated on that flag.

**The IP travels in the adapter's SRAM, not in RAM.** `rtl_handoff_save()`
(called from `cmd_execute()`, once, just before `go()`) writes magic + IP +
complement at GAPS offset `0x5000`, and `rtl_warm_adopt()` reads it back into
`g_warm_ip`, which `main()` applies **before** `set_ip_from_file()`. No RAM
address would do: a chainload goes through a loader that zero-fills
`0x8c004000`–`0x8c010000` and `crt0` zeroes BSS on top of that. `0x5000` is in
the gap the driver leaves — the ring is 16 KB + 16 at offset 0 and may spill
~1.5 KB past its end (nowrap), ending by `0x4600`; the TX descriptors start at
`0x6000`. It is wiped by exactly the events that must invalidate it: a cold
`rtl_bb_init()` (which memsets all 32 KB) and a power cycle.

What it deliberately does **not** carry is the DHCP lease. `dhcp_lease_time` is
0 in a fresh image, so the renewal branch stays out of the way and the
inherited address is held like a static one for as long as that instance runs.
On screen the IP is followed by `(Warm Start)`, which is the only way to tell
the fast path from the slow one without timing it.

BBA only — the LAN Adapter has nowhere to put the handoff and always takes the
cold path. `adapter_handoff_save()` is the neutral entry point in `adapter.c`.

### 4.10 DHCP: a reply is not ours just because it is a reply

Two filters, both added because the loader was acting on other people's DHCP
traffic. **This only bites on a real network** — an emulated bridge sees none
of it, which is why it survived so long.

1. **Ports, in `process_udp()`.** The DHCP branch used to be entered on
   `data[0] == BOOTREPLY` alone, so every broadcast datagram on the LAN
   (NetBIOS, SSDP, mDNS) went through the DHCP option parser. A server reply
   comes from 67 and lands on 68; nothing else does.
2. **xid and chaddr, in `handle_dhcp_reply()`.** The OFFER branch adopted
   whatever it was handed — server address, offered address **and transaction
   ID**. DHCP replies are frequently broadcast and the BBA accepts broadcasts,
   so any other machine on the LAN completing a lease while the Dreamcast waits
   handed us *its* offer. dcload then requested an address belonging to someone
   else under someone else's xid, and the ACK for its own request failed the
   xid test — nothing set `escape_loop`, so **the entire retry counter had to
   expire before another DISCOVER went out**. Replies arriving and being
   visibly ignored, with only a later (quiet) attempt succeeding, is that bug.
   The xid we sent is now kept in `dhcp_my_xid`; it used to be generated inside
   `kos_net_dhcp_fill_options()` and thrown away, which is why no such check
   was possible before.

A rejected reply returns -1 on purpose: `process_udp()` only escapes the wait
loop on 0, so the wait continues and the real reply is taken as soon as it
lands. `g_dhcp_replies` / `g_dhcp_not_ours` (§11) say whether this is happening
on a given network — and on real hardware they are readable with
`dc-tool-ip -d out.bin -a <addr> -s 8`, since dcload answers `SBIN` from inside
the DHCP wait.

Untouched, and worth knowing: a NAK still restarts `dhcp_go()` **recursively**
(bounded by `DHCP_NAK_NEST_MAX = 5`, after which DHCP is disabled outright).
That was reachable from a *foreign* NAK before the checks above; now only our
own can trigger it.

## 5. 1st_read bootstrap (`target-src/1st_read`)

`loader.s` + `disable.s` are linked at `-Ttext=0x8c010000` (the BIOS's 1st_read
load address), `objcopy`'d, then run through **KOS's `scramble`** — a retail
Dreamcast refuses an unscrambled binary.

`loader.s` masks interrupts (`SR = 0x500000f0`, its own, not the game's),
disables the cache, zero-fills `0x8c004000`–`0x8c010000`, copies
`exception.bin` to `0xac00f400` and `dcload.bin` to `0xac004000` (P2, caches
off), then jumps. All sizes are symbol differences resolved at link time, so
the copy cannot go stale.

## 6. Generated files / gotchas

- **Every** translation unit in `target-src/dcload/` and `example-src/` emits a
  `*.asm` listing via `-Wa,-adghlmns=$*.asm` (`a` directives, `d` debug,
  `g` basic blocks, `h` source, `l` assembly, `m` macros, `n` no forms,
  `s` symbols). These are build output — never hand-edit them; `make clean`
  removes them. Some are large (`commands.asm` ~69 KB, `syscalls.asm` ~52 KB,
  `dcload.asm` ~54 KB, `cdfs_syscalls.asm` ~46 KB); ignore their size.
  **`DCLOAD_LTO=1` suppresses them** — `lto-wrapper` discards `-Wa` options —
  so the Makefile stops asking for them in that configuration rather than
  emitting a warning per file. If the listings are what you came for, build
  without LTO.
- `.gitignore` covers `*.o *.bin *.lzo *.srec *.exe *.elf *.asm *.map` plus the
  example programs and the DC ELF, at the top level only — so intermediates do
  get committed from subdirectories sometimes.
- Protocol structs are **wire format**. The packed types in
  `host-src/tool/commands.h` and `target-src/dcload/syscalls.h` must match
  exactly on both sides; every new command needs edits on both.
- The "dcload is loaded" sentinel is `0xdeadbeef` at `0x8c004004`. Example
  programs check it before issuing syscalls — `example-src/dcload-syscalls.c:5`
  is the canonical pattern.

## 7. PC host tool (`host-src/tool`)

### 7.1 What it produces

`dc-tool-ip` (`.exe` on Windows), linked against `libelf` by default or sh-elf
`libbfd` + `libiberty` (+ `libsframe` on Binutils ≥ 2.40, + `libintl`) under
MinGW; `-lz` where `ZLIB_REQUIRED`; `-lws2_32 -lwsock32 -liconv` on MinGW.

| File | Role |
| --- | --- |
| `dc-tool.c` | the whole CLI (~1600 lines), with a bundled BSD `getopt` fallback. |
| `syscalls.c` / `syscalls.h` | per-syscall send/recv mirroring `DC00`–`DC22`. |
| `dc-io.h` | the transport primitives (`send_data`, `recv_data`, `send_command`, `recv_response`) and the `send_cmd` convenience macro. |
| `commands.h` | wire-format `command_t` and the `CMD_*` IDs. |
| `dcload-types.h` | `dcload_dirent_t` / `dcload_stat_t` — the DC's own layouts. |
| `config.h` | `PACKAGE_VERSION` ← `DCLOAD_VERSION`. |
| `utils.c` / `utils.h` | logging, IP helpers, `exception_struct_t` (the register-dump format). |
| `shim.c` | `vasprintf` etc. for the original (non-MSYS2) MinGW only. |
| `unlink.c` | Windows `unlink` wrapper. |

### 7.2 Command line

`AVAILABLE_OPTIONS` is `"x:u:d:a:s:t:i:nlqhrgf"` on MinGW, with `m:c:` added
elsewhere (`dc-tool.c:1356`).

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
| `-m` | path | map `/pc/` to path (no root needed) |
| `-c` | path | chroot to path (root needed) |
| `-i` | isofile | enable CDFS redirection from this image |
| `-r` | — | reset the DC (only while dcload is in control) |
| `-g` | — | GDB server on TCP `:2159` |
| `-l` | — | force legacy 1024-byte payload (v2.x only) |
| `-f` | — | disable FIFO delays: much faster, more loss |
| `-h` | — | usage |

Only one of `-x`/`-u`/`-d`/`-r` per invocation; `-m` and `-c` are mutually
exclusive. `prepare_comms()` runs a version handshake first — it sends
`CMD_VERSION` carrying `(major<<16)|(minor<<8)|patch`, detects legacy (port
31313) vs v2 (53535), and identifies the adapter.

### 7.3 Networking constants

UDP **53535** (v2.0+), legacy **31313**. Default payload 1452 B (1494 with
headers); `-l` forces 1024 B and caps a transfer at 11 MB. GDB stub TCP
**:2159**. `PACKET_TIMEOUT = 250000` µs. BBA is 100 Mbit, LAN Adapter 10 Mbit,
and the delay knobs are chosen from the detected adapter.

## 8. Protocol reference

`commands.h` on both sides is canonical. The 4-byte `command_t` ID is the only
field read as a string; `address` and `size` are big-endian on the wire,
followed by `size` bytes of data.

| ID | Name | Use |
| --- | --- | --- |
| `EXEC` | `CMD_EXECUTE` | execute |
| `LBIN` | `CMD_LOADBIN` | begin receiving binary |
| `PBIN` | `CMD_PARTBIN` | part of a binary |
| `DBIN` | `CMD_DONEBIN` | end of binary |
| `SBIN` / `SBIQ` | `CMD_SENDBIN` / `CMD_SENDBINQ` | read memory from DC (quiet variant) |
| `VERS` | `CMD_VERSION` | version handshake |
| `RETV` | `CMD_RETVAL` | return value |
| `RBOT` | `CMD_REBOOT` | reboot |
| `MAPL` | — | host-to-DC Maple packet |
| `PMCR` | — | host-to-DC performance-counter control |
| `EXPT` | `CMD_EXCEPTION` | DC-to-host exception dump |

`LBIN`/`PBIN`/`DBIN` are **not only the upload path** — they are also how disc
sectors reach a running game (§4.5). Anything you change there affects both.

`syscalls.h`, mirrored on both ends, defines `DC00`–`DC22`: exit, fstat, write,
read, open, close, creat, link, unlink, chdir, chmod, lseek, time, stat, utime,
bad, opendir, closedir, readdir, cdfsread, gdbpacket, rewinddir, cdfstoc — plus
`DD02` (legacy write) and `EXPT`.

Target-side `command_t` is `packed, aligned(4)`; host-side only `packed`.

## 9. Example programs (`example-src`)

A template for "how to build a DC program that uses dcload syscalls":
`crt0.S`, `startup_support.c`, `dcload-syscall.s` (the trampoline, which reads
the pointer at `0x8c004008`), `dcload-syscall.h` (raw numbers),
`dcload-syscalls.c/.h` (POSIX-ish wrappers, all guarded on
`*DCLOADMAGICADDR == DCLOADMAGICVALUE` so the same ELF fails gracefully off
dcload), and the `dc3.x` / `dc4.x` link scripts.

The three demos: `console-test.c` (opens its own source and writes it to the
host — exercises the file server), `exception-test.c` (forces an FPU exception
via `fdiv` — exercises the dump path; uses the raw header on purpose),
`gethostinfo.c` (prints `our_ip` / `tool_ip` / `tool_port`).

Run them from that directory with `dc-tool -x console-test`; default load
address `0x0c010000`.

## 10. CD / disc image recipes

Neither is part of the root `make` — `cd` into them.

- **`make-cd/`** — burns a real CD-R with `wodim` + `genisoimage`, building
  `IP.BIN` from `ip.txt` + `iplogo.png` via KOS's `makeip`. Writes a silent
  audio track then the data track. The recorder is hard-coded to `dev=0,0,0`.
- **`make-cdi/`** — `mkdcdisc -N -m` → `dctoolip.cdi`, for emulators or for
  burning later.

## 11. Debugging and instruments

**Ports.** dcload's GDB stub: TCP `:2159` (`dc-tool -g -x prog.elf`, then
`target remote :2159`; add `gdb_init()` to a KOS program). In-ICE / emulator
GDB: TCP `:3263`, preconfigured in `.gdbinit` and `.vscode/launch.json` —
always `arch sh4` and `endian little` first. A faulting program writes
`dcload_exception_dump.bin` in the terminal's CWD, in `utils.h`'s
`exception_struct_t` format.

**The flycast-as-target loop** (build → CDI → flycast on Windows → GDB on
`:3263` → game upload) is `docs/flycast-debug-loop.md` +
`scripts/flycast-debug-loop.sh`. It needs flycast built with
`-DENABLE_GDB_SERVER=ON` — the `Debug.GDBEnabled` config flag alone does
nothing on a build without it. Such a build starts **suspended**; release it
with `scripts/flycast-resume.py`. Anything touching `:3263` must end with a GDB
`detach`, or the target stays halted and looks frozen.

**Instruments in `scripts/`:**

| Script | What it answers |
| --- | --- |
| `dc-peek.py <symbol\|0xaddr>` | read guest memory, resolving symbols from the ELF. Never hard-code a counter address — adding a counter shifts `.data` and the values become plausible nonsense. |
| `dc-sample.py` | poll a table of counters at a fixed rate, print deltas. |
| `dc-track.py` | continuous sampling that **re-attaches per sample**, because flycast halts the guest while a debugger stays connected — a held connection freezes what you are watching. |
| `dc-freeze.py` | snapshot everything during a freeze. |
| `dc-pc.py` | where is the guest executing right now. |
| `dc-screen.py <out.png>` | what the console **displays**: reads the framebuffer the PVR scans out (geometry from `FB_R_CTRL`/`FB_R_SOF1`/`FB_R_SIZE`) and writes a PNG with `zlib` alone. Screenshotting the flycast window is a dead end (Vulkan surface); the pixels are just guest memory. **0.28 s** for a full 640×480 frame; `--scale 4` gives 160×120 in ~0.20 s and a 24 KB PNG instead of 287 KB. `--repeat N [--interval S]` captures a sequence and `--probe` prints a CRC and mean luma instead of writing a file, so "did that keypress do anything" costs no image at all. **`--repeat` re-attaches per frame on purpose** — flycast halts the guest for as long as a debugger is connected, so holding the socket returns the same frame N times (measured: six captures, six identical CRCs, on an animating screen). |
| `dc-regs.py` | where a write was **aimed**: `QACR0/1`, `SB_C2DSTAT`/`LMMODE`, the SH4 DMAC channel-2 set, the TA output pointers. `DAR2 = 0` is normal on this console — Holly supplies the CH2-DMA destination via `C2DSTAT`. |
| `dc-integrity.py` | did the title write over the loader? Compares sections against the ELF, or against a `--save-baseline` taken while the game runs. |
| `dc-irqwatch.py` | interrupt activity. |
| `dc-trap.py` | arms a `Z0` breakpoint, **with a self-test** — its own probe proves the BIOS ROM ignores flycast's `trapa` patch, so you cannot breakpoint address 0. |
| `dc-catch-vector-write.sh` | host-side hardware watchpoint via `cdb.exe` on the GD vector. Carries a **positive control**: x64 data breakpoints are per-thread, so it treats dcload's own `cfs_redir` install as proof the watchpoint is live. No `SELFTEST-OK` means the result is meaningless, not that nobody wrote. |
| `flycast-counters.sh` | reads the `dcdiag` patch in the flycast tree (Holly interrupts raised/acknowledged per bit, TA lists opened/closed/parameters, per list type) **at full speed**. Prefer this to `bp flycast!asic_RaiseInterrupt`, which costs a round trip per interrupt and drops VBlank from 60/s to 1.7/s — every rate measured under it is void. **Verified absent 2026-08-14**: the patch is no longer in the flycast tree (`core/hw/holly/dcdiag.h` exists, but nothing includes it and neither build carries the symbols), so the script exits with "unexpected cdb output (0 values)". Re-apply and rebuild before relying on it — and note the DC-side counters below answer frame-rate questions on their own (§16). |
| `sa-repeat.sh`, `sa-gdi-control.sh`, `flycast-transition.sh` | session drivers used during the Sonic Adventure work. |

**You cannot trap a write as it happens under flycast.** Its GDB stub answers
`Z1/Z2/Z3/Z4` with an empty packet; only `Z0` works, and only with `len == 2`.
Its SH4 UBC is register storage with no compare logic in the memory path, so
programming `BARA` from dcload would compile, run and never fire. Use a
host-side watchpoint, or patch flycast.

**Always-compiled counters** (read with `dc-peek`): `g_gd_idx_counts[]`
(histogram of GD syscall indices — this is what identifies the condition a
title is spinning on), `g_gd_park_longs`, `g_cdfs_read_retries` /
`g_cdfs_read_fails`, `g_lbin_count` / `g_dbin_count`, `g_pbin_ok` /
`g_pbin_rejected` / `g_pbin_clamped`, `g_last_load_addr` / `_size`,
`g_last_reject_*`, and the RX set in `rtl8139.c` (`g_rx_frames`, `g_rx_polls`,
`g_rx_wraps`, `g_rx_overflow`, `g_rx_reinit`, `g_rx_linkchange`,
`g_rx_link_giveup`, `g_rx_hdr_defer`, `g_rx_last_capr` / `_cbr`), plus
`g_dhcp_replies` / `g_dhcp_not_ours` (§4.10 — DHCP traffic seen against DHCP
traffic that was somebody else's). The
post-mortem block at `PM_BASE` survives a reboot but is no longer out of a
title's reach — treat it as advisory.

**Checking the DATA, not the accounting.** All of the above is arrival
accounting: bytes can be delivered, counted, and still be wrong. The Rust host
(§16) implements read-back verification — it re-reads the delivered range with
`SBIQ` between the last `PBIN` and the `RETV` and compares. See
`docs/read-back-verification.md`. It needs no DC-side change; `cmd_sendbinq` is
always available. On the Sonic Adventure failure it reported 85 reads verified,
0 mismatches, which is what killed the corruption hypothesis.

**A methodological warning, learned the hard way twice.** An instrument inside
the blast radius cannot report on the blast, and an instrument that costs a UDP
round trip changes the timing of what it measures. Before believing a null
result, prove the instrument still detects something.

## 12. CI and branching

- **Primary CI is GitLab** (`.gitlab-ci.yml`): stage 1 clones KOS and tarballs
  the `scramble` utility; stages 2 and 3 build in
  `segadreamcast/toolchains:binutils-2.34-gcc-4.7.4-newlib-2.0.0-gdb-9.1` and
  the gcc-9.3.0/newlib-3.3.0 image, sourcing KOS `environ.sh` then
  `make && make install`.
- **GitHub Actions only mirrors** `master` to
  `gitlab.com/kallistios/dcload-ip.git`. No tests run on GitHub.
- **Default branch is `master`**; the GitLab repo is a read-only mirror.

## 13. Cross-platform quirks

- **MinGW** — `MINGW` for original MinGW/MSYS, `MINGW64` for MSYS2. `WITH_BFD`
  auto-flips to 1. `shim.c` supplies `vasprintf` etc. for original MinGW only.
- **Cygwin** — `WINDOWS` + `CYGWIN`; behaves like MinGW for build flags.
- **macOS** — `libelf` from `/opt/homebrew`, `-DMACOS` so `dc-tool.c` includes
  `<libelf/libelf.h>`. No Apple-Silicon-vs-Intel handling; the Homebrew prefix
  covers it.
- **BSD** — `BSD := 1` and nothing else; install `libelf` yourself.

## 14. Pitfalls

1. **Forgetting to `source` KOS `environ.sh`** — the first cross-compile fails
   with "command not found".
2. **Building `1st_read` before `dcload`** — the `.incbin` lines fail.
3. **Building `make-cd/` or `make-cdi/` from the root** — they are not in
   `SUBDIRS`; `cd` into them.
4. **Editing a `*.asm` file** — it is a listing and will be regenerated.
5. **Hand-coding an IP without an ARP entry on the host** — see the README's
   Notes, and `announce_presence()` (§4.7) for why the DC must speak first.
6. **Changing `DREAMCAST_IP` and expecting a rebuild** — it is a `-D` with no
   dependency, so `make clean` is mandatory. (The `target-src/dcload` flags
   *do* trigger a rebuild; this one does not.)
7. **Bumping `VERSION` anywhere but `Makefile.cfg`** — both Makefiles already
   consume it.
8. **Adding a command or syscall ID on one side only** — both `commands.h` and
   both `syscalls.h` need it, and the struct layouts must match byte for byte.
9. **Assuming a guard still guards.** `scripts/check-resident-invariant.sh`
   checks for references into a `_resident_end` region that no longer exists,
   and `scripts/frontier_fuzz.c` fuzzes a `win_mask`/frontier accounting that
   was removed with `ABIN`. Both will happily report success. Verify a check
   detects something before trusting it — this exact script silently reported a
   clean bill of health for weeks after a relocation.
10. **Rebuilding and then testing the old image.** `make` does not regenerate
    the CDI, and regenerating it does not deploy it. Run `mkdcdisc`, copy, then
    prove the deployed image contains the new binary **by containment** (search
    for the first 64 bytes of `1st_read.bin` inside the CDI) — never by
    timestamp or size, since a stale CDI has exactly the same byte count.
11. **Hard-coding an address the linker owns.** A tripwire in
    `cdfs_syscalls.c` once compared the GD vector against a literal; an
    unrelated four-byte change to `dcload-crt0.s`' jump table moved the real
    target, the tripwire "repaired" the vector to the stale address, and the
    machine rebooted at `EXEC` with no breadcrumb. Compare against the symbol
    the code actually writes.
12. **Adding state to the DC side without checking `_end`.** §4.6 — this is the
    bug that took two days.
13. **Leaving an instrument enabled.** `GD_TRACE` / `GD_TRACE_CALLER` cost a
    UDP round trip per event and can by themselves stop a title from booting.
14. **`REIOS: Booting up` in `flycast.log` is not a reset.** `reios_boot` is
    hooked at address 0, so the line means only that the SH4 executed there — a
    jump through a null pointer reaches it exactly as a reset vector does, and
    the CPU context is not cleared on the way. To tell a null jump from an
    exception, check for flycast's `[BBA-DIAG]` line in `Do_Exception` (verify
    the string is in the shipped binary, not just in the source).
15. **Dividing a `long long`.** One `/` on a 64-bit value costs ~840 bytes of
    libgcc (`__udivdi3` + `__udiv_qrnnd_16` + `__clz` + a 256-byte
    `__clz_tab`), silently, in an image where §4.6 says bytes are correctness.
    `-Os` prefers the libcall even when the divisor is a constant. Use
    `PMCR_Delta_Seconds()` for counter deltas, or shift the value into 32 bits
    first; `grep libgcc target-src/dcload/dcload.map` says whether anything is
    pulling it back in.
16. **Putting a new large buffer in BSS.** Mark it `HIRAM_BUF` (`hiram.h`) and
    it costs nothing in `_end`. A `HIRAM_BUF` object is zeroed by
    `dcload-crt0.s` like BSS, but it is **not** covered by 1st_read's
    `0x8c004000`–`0x8c010000` zero-fill, and it is in RAM a title's allocator
    could conceivably claim — put a buffer there, not a flag someone else
    writes.

## 15. Where to look first

- **Wire protocol** → `host-src/tool/commands.h`, `host-src/tool/syscalls.h`,
  `target-src/dcload/commands.{c,h}`, `target-src/dcload/syscalls.{c,h}`. Both
  sides, in lock-step.
- **Throughput** → `Makefile.cfg` (FIFO delays), `dc-tool.c`
  (`PACKET_TIMEOUT`), and `GD_EMU_ASYNC` (§4.5). Two corrections to what this
  entry used to say, both paid for:
  - It claimed upload loss was "strictly periodic and therefore not
    congestion — do not tune the pacing further". **That was wrong.** The
    periodicity was an artefact of the loss being invisible: dcload's overflow
    bits were being cleared by its own per-frame interrupt acknowledge, so the
    one counter that could have shown congestion read zero. `RT_RXMISSED`, the
    chip's own tally, showed 887 dropped frames in a single upload. Closing the
    loop with a `DoneBinary`-probed window took it to 7.
  - **Latency and throughput are different problems here, and latency is the
    one a running title feels.** A disc read blocks the game for its whole
    duration, so the figure that matters is milliseconds per chunk, not KB/s.
    Chasing KB/s would have missed the 39 ms of host-side `thread::sleep` that
    was the actual stutter (§16).
- **Base address / memory map** → `dcload.x`, `target-src/1st_read/loader.s`,
  `dcload-crt0.s`, `go.s`, `exception.S`, `cdfs_redir.s`, `commands.c`,
  `cdfs_syscalls.c` (`PM_BASE`), `maple.c` (buffer address), `hiram.h` +
  `dcload.x`'s `.hiram` (the packet buffers),
  `target-src/dcload/Makefile` (`exception -Ttext=`), plus `scripts/dc-peek.py`.
  Substitute on code lines only — the prose in comments records measurements
  made at older bases, and rewriting it would falsify the record. The link step
  (`--no-undefined` plus the `dcload.x` asserts) catches inconsistencies.
- **GD read path** → `cdfs_syscalls.c` and `cdfs_redir.s` headers first, then
  `commands.c` (`cmd_partbin` / `cmd_donebin` and the `bin_info` window).
- **Anything that must work while a game runs** → check `_end` in
  `dcload.map`, and re-read §4.6.
- **A new example program** → copy one of the three, add it to `OBJECTS`/`all`
  in `example-src/Makefile`.
- **A new on-screen field** → `video.s` for the primitives, `dcload.c` for the
  loop that paints them.
- **A new Maple or PMCR command** → README only; `dc-tool` forwards packets
  verbatim.
- **A new KOS** → check `$(KOS_BASE)/utils/scramble/scramble` still exists and
  that the `.gitlab-ci.yml` image tags are still published.

## 16. The Rust host server

A separate, faster host implementation lives outside this repo at
`/mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/` (mainly `src/dispatch.rs`).
It is the host side used for GD-emulation work; `dc-tool-ip` remains the
reference implementation.

What the DC side depends on:

- **`MAX_XFER` must stay in step with `BIN_INFO_MAP_SIZE`** (256 chunks). The
  host splits any larger transfer into successive LoadBinary exchanges; the DC
  refuses what its map cannot describe.
- It serves reads **strictly one at a time**: the whole LBIN/PBIN/DBIN exchange
  runs to completion inside its request handler.
- Its recovery resends a **run** from the address `DoneBinary` reports,
  doubling per iteration and capped at 64. One-at-a-time recovery cost a round
  trip per lost packet; uncapped recovery resent 45 MB for a 6.4 MB upload.
  Overshoot is free — a part the DC already holds is rewritten with the same
  bytes.
- `send_data()` verifies the LoadBinary **echo** (address and command) rather
  than accepting whatever packet arrives next.
- Read-back verification (`DCLOAD_VERIFY_READS=1`) is described in §11 and
  `docs/read-back-verification.md`.
- **Pacing on the runtime CDFS path must be a spin, never `thread::sleep`.**
  dcload answers a disc read synchronously — it sits in `bb->loop()` until the
  last `PBIN` lands — so every microsecond the host spends not sending is a
  microsecond the **title is frozen**. `thread::sleep` on Windows rounds up to
  the system timer tick, which turned three innocent-looking waits per 16 KB
  chunk (`12 × sleep(1 ns)`, `sleep(1800 µs)`, `sleep(25 ms)`) into ~39 ms of
  deliberate idling. Measured on Sonic Adventure, before and after removing
  them, at the same point of the same boot:

  | | before | after |
  | --- | --- | --- |
  | poll iterations per 16 KB chunk | 5301 | **179** |
  | dcload's share of the machine | 35.6 % | **1.8 %** |
  | title frozen per chunk | 45.6 ms (2.3 PAL frames) | **1.5 ms** |
  | the title's own frame loop (`GetDrvStat`) | 45.0/s | **58.9/s** |

  Almost none of it was the network: 16 KB at 100 Mbit is 1.3 ms of wire time.
  `dc-tool` never had the problem because it busy-waits
  (`while ((time_in_usec() - start) < rx_fifo_delay);`) and puts its only
  `nanosleep` behind `SAVE_MY_FANS`, which is 0 by default. The Rust port
  inherited the *idea* of a 1 ns yield from dc-tool's comment ("there's no
  picosecond sleep, so this is about as good as it gets") without inheriting
  its cost profile.
- **`polls per chunk` is the calibration-free way to measure this.**
  `g_rx_polls` only advances while dcload holds the CPU, so its ratio to
  `g_cdfs_sync_chunks` is how hard the loader is spinning per unit of work,
  with no clock needed — which matters because PMCR reads 0 while a game runs
  (§11). `g_gd_idx_counts[2]` (ExecServer) and `[4]` (GetDrvStat) are called
  once per frame by Sonic Adventure, so they *are* a frame-rate counter; and
  `[3]` (InitSystem) must read exactly 1, which is a free check that you have
  the array alignment right.

## 17. Sources of truth

Code first. Then, most likely to answer a question:

| File | For |
| --- | --- |
| `Makefile.cfg` | toolchain, tunables, version |
| `Makefile.hostdetect` | host flags |
| `host-src/tool/dc-tool.c` | CLI and protocol behaviour |
| `target-src/dcload/dcload.c` | the DC main loop |
| `target-src/dcload/dcload.x` + `dcload.map` | the memory map, and `_end` |
| `target-src/dcload/cdfs_syscalls.c`, `cdfs_redir.s` | GD emulation — long headers, read them |
| `target-src/dcload/go.s` | the handoff, and why SR and CCR are what they are |
| `target-src/1st_read/loader.s` | bootstrap flow |
| `README.md`, `CHANGES` | user-facing behaviour and history |
| `Cdif131e.txt` | SPI / GD-ROM command spec |

Documents in `docs/`:

| Document | For |
| --- | --- |
| `loader-comparison.md` | three-way comparison of DreamShell isoldr, dc-virtcd and dcload on the same problem — interrupts, async reads, memory cohabitation, syscall hooking, GDC command coverage, instrumentation. **The reference to consult before inventing a mechanism.** Its dcload column predates the current tree; re-read the code alongside it. |
| `sonic-adventure-investigation.md` | the full record of the 2026-08-07→10 investigation: every cause eliminated by measurement, and the root cause. History, not a description of the tree. Read it before re-suspecting the transport. |
| `flycast-debug-loop.md` | the emulator-as-target workflow. |
| `read-back-verification.md` | what read-back verification proves, what it cannot, how to read a mismatch. |
| `dreamshell-presets/` | 6168 archived per-game presets — what DreamShell prescribes for a given title (`memory`, `dma`, `async`, `irq`). |
