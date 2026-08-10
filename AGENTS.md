# dcload-ip — Agent Notes

> **ÉTAT AU 2026-08-09 — LIRE AVANT TOUT LE RESTE.** À la demande de
> l'utilisateur, **toutes** les modifications de code de l'enquête CDFS /
> Sonic Adventure ont été retirées de l'arbre de travail, des deux côtés
> (`target-src/`, `host-src/` ici, et `src/` dans `dcload-ip-rs`). Le code
> présent est celui du commit `52380db` en amont. Tout ce que décrivent les
> §4.3 (flags `CDFS_ASYNC`, `CDFS_SYNC_CHUNK_SECTORS`, `G1_DMA_IRQ_STUB`,
> `RX_*`…), §4.4 (base `0x8c004000`, découpe résident/transient), §16
> (`ABIN`), §18 et §19 **n'est plus dans le code** — c'est le journal de
> l'enquête, pas la description de l'arbre. Les modifications sont
> intégralement conservées : branche/tag git `backup/cdfs-sa-work-2026-08-09`
> dans les deux dépôts, plus patches et copies dans
> `/mnt/e/Nextcloud/Projets/Dreamcast/dcload-work-backup-2026-08-09/`
> (voir son `README.md`). `scripts/` et `docs/` sont intacts.
>
> **Second retrait le même soir.** Une deuxième série de modifications (ARP
> gratuit + keepalive, `DREAMCAST_IP` statique, handoff caches actives en P1,
> `GD_TRACE`, bloc post-mortem) avait été écrite sur cet arbre restauré ; elle a
> été retirée à son tour, côté DC seulement. Elle est dans la branche/tag
> `backup/sa-71st-read-2026-08-09` (commit `8000846`) et dans le §6 du `README.md`
> de la sauvegarde. Le serveur Rust `dcload-ip-rs` a suivi (`src/dispatch.rs`
> revenu à `096847e`, commit de sauvegarde `55771ce`) — **sauf `src/fs.rs`, dont
> le correctif Unix est conservé** sur demande, sans quoi la caisse ne compile
> pas sous Linux/WSL. Donc plus de contrôle de flux fenêtré à l'upload : le
> chemin d'upload est de nouveau celui du stock.

> This file is the operating manual for AI coding agents working in this
> repository. It is intentionally exhaustive. Every section is a fact
> that an agent reading the source cold would not necessarily infer.
> Update it as the project changes.

## 1. What this project is

`dcload-ip` is a **Sega Dreamcast network loader**. It has two halves
that cooperate over UDP:

1. A **Dreamcast-side** program (`dcload`, running on the SH4 CPU at
   `0x8cf00000` after the 1st_read bootstrap copies it there — see
   §4.4, it used to be `0x8c004000`). It
   implements ARP, ICMP, UDP, and an optional IPv4 DHCP client on the
   BBA (`HIT-0400`) or LAN Adapter (`HIT-0300`). It accepts commands
   to upload (`LBIN` / `PBIN` / `DBIN`), execute (`EXEC`), read memory
   (`SBIN` / `SBIQ`), service a GDB stub on TCP `:2159`, pass Maple
   packets through (`MAPL`), and let the host control the SH4
   performance counters (`PMCR`).
2. A **PC-side** program (`dc-tool-ip`, a single C binary using plain
   BSD sockets) that talks to dcload, plus ships and reads files and
   provides the GDB proxy.

The repo is a fork of KallistiOS' `dcload-ip`, overhauled by
Moopthehedgehog and currently maintained by Mickaël Cardoso (SiZiOUS)
and contributors. License: **GPLv2** (`COPYING`). Current version
(baked into both binaries): `2.0.4` — bump it in `Makefile.cfg` only.

There is **no autotools, no CMake, no Meson, no Makefile.am, no
configure script**. The build is driven by hand-written GNU Makefiles
that include `Makefile.cfg` and `Makefile.hostdetect` from the repo
root.

## 2. Repository layout

```
.
├── Makefile                # top-level driver, just `include`s Makefile.cfg + recurses
├── Makefile.cfg            # single source of truth for toolchain / IPs / tunables / VERSION
├── Makefile.hostdetect     # sets MINGW, MINGW32, MINGW64, CYGWIN, MACOS, BSD, WINDOWS
├── README.md               # user-facing docs (build, install, test, MAPL/PMCR, exception dump)
├── CHANGES                 # release notes back to 1.0.0
├── NETWORK                 # 14-line summary of protocols
├── COPYING                 # GPLv2
├── Cdif131e.pdf / .txt     # reference material for the GD-ROM format (do not edit)
├── .gdbinit                # `arch sh4`, `endian little`, target :3263
├── .gitlab-ci.yml          # primary CI (two gcc jobs)
├── .github/workflows/
│   └── sync-to-gitlab.yml  # mirrors master → GitLab
├── .vscode/
│   ├── launch.json         # sh-elf-gdb attach config to :3263
│   └── settings.json       # files.associations for cstdint/memfuncs.h
├── host-src/
│   └── tool/               # the only host code; produces dc-tool-ip
├── target-src/
│   ├── dcload/             # the main DC binary; produced as dcload.bin + exception.bin
│   └── 1st_read/           # CD bootstrap; produces scrambled 1st_read.bin
├── target-inc/             # header-only: newlib stubs + asm helpers (no Makefile)
│   ├── machine/            # arch-specific headers
│   ├── sys/                # syscall-style headers
│   └── *.h                 # libc-ish headers (stdio.h, stdlib.h, …)
├── example-src/            # 3 demo .bin programs uploaded with `dc-tool -x`
├── make-cd/                # recipe to burn a real CD-R with wodim + genisoimage
├── make-cdi/               # recipe to produce a .cdi image with mkdcdisc
└── auto-load/              # EMPTY stub directory — ignore it
```

Things that surprise first-time agents:

- `host-src/machine/` and `host-src/sys/` do **not exist** in this
  repo, even though some KOS-style projects use those paths.
- `auto-load/` is empty. Do not put code there; it is unbuilt.
- `target-inc/` has **no Makefile** of its own — it is a header
  include path. The `target-src/dcload` and `example-src` Makefiles
  add it with `-I../../target-inc` and `-I../target-inc`.
- The shipped Dreamcast artifacts are `target-src/dcload/dcload.bin`
  and `target-src/dcload/exception.bin` and
  `target-src/1st_read/1st_read.bin`. The ELF siblings and `dc-tool-ip`
  are gitignored at the top level.
- `*.asm` files committed in the tree alongside `.o` files are
  **build output** (assembly listings), not hand-written. See §6.

## 3. Build system

### 3.1 Toolchain assumptions

- The project expects a **KallistiOS SH-ELF toolchain** (KOS `dc-chain`
  output). The compiler prefix comes from `$(KOS_CC_BASE)`, which is
  set by KOS's `environ.sh`.
- Always `source` the KOS `environ.sh` before running `make`, or every
  cross-build target will fail with "command not found".
- `target-src/1st_read/Makefile` shells out to
  `$(KOS_BASE)/utils/scramble/scramble` to scramble the bootstrap.
  That binary comes from KOS — if you build KOS from source (as
  GitLab CI does), `make` it once with
  `cd $KOS_PATH/utils/scramble && make`.
- A non-KOS sh4-elf toolchain works if you uncomment `USING_KOS_GCC`
  in `Makefile.cfg`. The alternate block is hard-coded to a path
  under `/mnt/c/DreamcastKOS/`, which you must adjust.
- The host tool (`dc-tool-ip`) is plain `gcc`. It does not require
  the SH cross toolchain.

### 3.2 Host detection (`Makefile.hostdetect`)

Sets one or more of these variables, then `Makefile.cfg` reacts:

- `BSD` — any host ending in `BSD`
- `MACOS` — `uname -s` returns `Darwin`
- `MINGW` / `MINGW32` / `MINGW64` / `CYGWIN` / `WINDOWS` — set by
  matching the prefix of `uname -s` (`MINGW*` or `CYGWIN*`).
  `MINGW64` is further distinguished by checking `$MSYSTEM_CHOST`
  from MSYS2.

Do not duplicate this logic — just `include Makefile.hostdetect`
wherever you need the flags.

### 3.3 Root Makefile

`Makefile` is a 32-line driver:

```make
include Makefile.cfg
SUBDIRS = host-src target-src example-src
all: subdirs
$(SUBDIRS):
    $(MAKE) -C $@
target-src: host-src
install:
    $(MAKE) -C host-src/tool install
clean / distclean: recurse into SUBDIRS
```

The `target-src: host-src` and `1st_read: dcload` chain expresses
the real dependency: you must build `dcload` (target-side) before
scrambling it into `1st_read.bin`. Running `target-src/1st_read`
first will fail at `.incbin` time with "no such file".

### 3.4 Common build commands

```sh
# full build (host tool + dcload + 1st_read + example programs)
source /opt/toolchains/dc/kos/environ.sh
make

# install dc-tool-ip to /opt/toolchains/dc/bin (or $(TOOLINSTALLDIR))
sudo make install

# incremental sub-builds
make -C host-src/tool
make -C target-src/dcload
make -C target-src/1st_read

# clean
make clean
make distclean      # also removes *.bin
```

The root `make` also rebuilds `example-src/` because it is in
`SUBDIRS`. If you only want the loader, build the first three
subdirs explicitly.

### 3.5 Makefile.cfg — the one file that changes between machines

`Makefile.cfg` is the only file an end user edits. Important knobs:

- `USING_KOS_GCC = 1` — use KOS toolchain (default). Uncomment to
  switch to a standalone sh4-elf-gcc.
- `HOSTCC` / `HOSTCFLAGS` / `HOSTLDFLAGS` — defaults are `gcc -Og`.
  Auto-adds `-D_WIN32` and `.exe` extension on Windows.
- `TARGETPREFIX` / `BINTARGETPREFIX` — taken from `$(KOS_CC_BASE)` in
  KOS mode, or hard-coded under `/mnt/c/DreamcastKOS/` otherwise.
- `TOOLINSTALLDIR = /opt/toolchains/dc/bin` — destination of
  `make install`.
- `WITH_BFD` — `0` links dc-tool against `libelf` (default), `1`
  links against sh-elf `libbfd` and `libiberty` (auto-flipped to 1
  under MinGW). macOS needs Homebrew's `libelf` in `/opt/homebrew`.
- `BFDLIB` / `BFDINCLUDE` / `ELFLIB` / `ELFINCLUDE` — must point at
  the **sh-elf** BFD headers, not the host's system BFD. With KOS
  these are derived from `$(TARGETPREFIX)`.
- `TARGETCCVER` — auto-detected from the compiler's `--version` and
  clamped to 4. Used by `example-src/Makefile` to pick
  `dc$(TARGETCCVER).x` (i.e. `dc3.x` or `dc4.x`).
- `VERSION = 2.0.4` — embedded into both binaries via
  `-DDCLOAD_VERSION=`. Bump it here on release.
- `STANDALONE_BINARY = 1` — adds `-static` to the host link line;
  on by default for any `WINDOWS` build.
- `SAVE_MY_FANS = 0|1` — slows dc-tool's poll loop to cut CPU
  usage on laptops. Wired through to host code as
  `-DSAVE_MY_FANS=$(SAVE_MY_FANS)`.
- `EXCEPTION_SECONDS = 15` — on-screen register-dump duration
  (0–60). Wired through to target code as
  `-DEXCEPTION_SECONDS=$(EXCEPTION_SECONDS)`.
- `DREAMCAST_BBA_RX_FIFO_DELAY_{COUNT,TIME}` and the `…_LAN_…`
  pair — burst pacing for dc-tool → dcload. The values are wired
  into host code as `-DDREAMCAST_BBA_RX_FIFO_DELAY_COUNT=…` etc.
  Tune these if you see `link change…` mid-transfer (BBA) or the
  LAN adapter hanging (raise `TIME` by ~100 µs at a time). The
  comment block in `Makefile.cfg` lists the recommended safe and
  max presets.
- `DREAMCAST_IP = 0.0.0.0` — baked in as `-DDREAMCAST_IP="…"`. Any
  `0.x.x.x` enables DHCP; use `169.254.x.x` if you plan to populate
  the host's ARP table with `arp` / `netsh` neighbor entries
  (see README "Notes" section).

## 4. Dreamcast binary build (`target-src/dcload`)

### 4.1 Output artifacts

`make` in `target-src/dcload/` produces:

- `dcload` — final ELF, base `0x8cf00000` (linker script `dcload.x`
  pins the memory region). ~45 KB `.bin`.
- `exception` — standalone ELF linked at `-Ttext=0x8cf0b400` from
  `exception.S` only. 3 KB `.bin`.
- `dcload.bin`, `exception.bin` — raw `objcopy -O binary` outputs.
  These two are consumed by `target-src/1st_read/loader.s` via
  `.incbin`.
- Per-source `*.asm` assembly listings (build artifacts; see §6).
- `dcload.map`, `exception.map` — link maps.

`make clean` removes objects, maps, and `*.asm`; `make distclean`
additionally removes the `.bin` outputs.

### 4.2 Source list and what each file does

The build uses the SH4 compiler with a long list of aggressive
optimization-disabling flags, all set in
`target-src/dcload/Makefile`:

```
-fno-zero-initialized-in-bss -fno-common -fomit-frame-pointer
-fno-strict-aliasing -fno-unwind-tables -fno-asynchronous-unwind-tables
-fno-exceptions -fno-delete-null-pointer-checks -fno-stack-protector
-fno-stack-check -fno-merge-constants -fno-merge-all-constants
-std=gnu11 -ffreestanding
```

`OBJCOPY` strips `.stack` (`-R .stack`). Linker flags include
`-Wl,--warn-common -Wl,--no-undefined -Wl,-Map=dcload.map
-Wl,-Tdcload.x -nostartfiles -nostdlib -static -Wl,-z,now` and pulls
in `-lgcc` for the SH4 helpers.

Notable source files:

- `dcload.c` — main entry; sets up adapter detection, DHCP retry,
  lease-time perfcounter, video background, and the command loop.
- `dcload-crt0.s` — start-of-day: sets up the stack, **installs VBR**,
  zeroes the **two** BSS ranges (`[__bss_start, _resident_end)` and
  `[__transient_bss_start, _end)` — they are not contiguous since the
  resident/transient split, and zeroing the whole span would erase
  `.transient.text`, i.e. `main` itself), calls `main`.
- `disable.s` / `disable.c` — turns off the SH4 cache (the SH4
  cannot run cached code at the 1st_read stage).
- `startup_support.c` — BSS-zeroing and C++ constructors.
- `video.s` — video output for the on-screen status (Marcus
  Comstedt's `video.s` from KOS).
- `packet.c` / `packet.h` — packet builder / parser for the
  commands in `commands.h`.
- `net.c` / `net.h` — protocol glue (ARP/ICMP/UDP), adapter
  detection and performance tuning.
- `adapter.c` / `adapter.h` — abstract BBA / LAN-Adapter driver
  interface.
- `rtl8139.c` — BBA (RTL8139) driver.
- `lan_adapter.c` — LAN Adapter driver.
- `dhcp.c` / `dhcp.h` — IPv4 DHCP client, with retry counter that
  the README explains.
- `perfctr.c` / `perfctr.h` — SH4 performance counter control,
  and the DHCP-lease-time tracking that lives in counter #1.
- `memfuncs.c` / `memfuncs.h` — hand-written memcpy/memcmp/etc.
  (SH4 needs aligned fast paths).
- `maple.c` / `maple.h` — Maple bus driver (Marcus Comstedt).
- `cdfs.h` / `cdfs_redir.c` / `cdfs_syscalls.c` — redirection of
  the Dreamcast's `cdfs` syscalls to the PC. The C side is small;
  the matching `*.asm` is enormous because it is the generated
  syscall stub table (see §6).
- `syscalls.c` / `syscalls.h` — host-issued `DC00`–`DC22` syscall
  handlers.
- `commands.c` / `commands.h` — the `EXEC`/`LBIN`/… command
  dispatcher.
- `exception.S` — the on-screen exception display, and the VBR table
  dcload hands the game. **Its `+0x600` interrupt vector is `nop; rte;
  nop`** — interrupts are ignored, as in stock dcload. See §18 for why the
  background-pump/VBR-patch machinery that used to live there was removed.

### 4.3 Behaviour flags (`target-src/dcload/Makefile`)

All `?=`, so `make VAR=x` overrides them. Each is wired in as a `-D`.
The `%.o` rules depend on the Makefile and on `../../Makefile.cfg`, so
flipping one **does** trigger a rebuild (this was not always true).

| Flag | Default | What it does |
| ---- | ------- | ------------ |
| `CDFS_TRACE` | `0` | UDP `TRAC` trace of every GD syscall. Costs a lot: ~60 KB/s throughput with it on vs ~161 KB/s off, plus resident margin. Only for debugging. |
| `LAUNCH_MACHINE_CLEANUP` | `1` | `setup_machine()`-style ASIC/TMU scrub before `go()`. Never write `TCR0` here — that caused a black screen. |
| `RX_DIRECT` | `0` | Land a PBIN payload straight from the RX ring into its destination (one copy instead of two). Measured harmful: 4% of payloads clobbered by the NIC mid-read, holes 0% -> 10%. |
| `RX_FRAME_COMPLETE_CHECK` | `1` | Stop draining once `cur_rx == CBR`. `RxBufEmpty` cannot say this (we publish `CAPR = cur_rx - 16`), and without it we consume a frame that was never written. Not to be confused with the implausible-header deferral, which is unconditional. |
| `RX_GATE_ON_ROK` | `1` | Despite the name, gates nothing. It unmasks the RX status bits in `RT_INTRMASK` — the load-bearing part, since the mask decides whether those bits are observable at all — and adds observational counters. The real entry decision is in `rtl_bb_poll_iteration`: `RT_INT_RX_ACK`, else an ungated `RxBufEmpty` safety net. |
| `CDFS_ASYNC` | `1` | `0` routes cmd 17 (DMAREAD) through the **synchronous** cmd 16 path: the syscall blocks until the whole read has landed, then answers COMPLETED. No slot pool, no `CMD_RETVAL` notification that can be lost, no identity rebuilt from address ranges — isoldr's single-shot model, and the three places §3.3 says dcload diverged most. Frees **608 resident bytes** (headroom 100 → 708 with `G1_DMA_IRQ_STUB=1`). `cmd_partbin` writes the data either way (a plain LBIN upload allocates no slot either). Watch out: the sync path FAILS a read on `GD_SYSCALL_TIMEOUT_SECONDS` (20 s), and SA executes half-filled buffers rather than handling that error. **It also disables the ABIN window once `running`** (`commands.c`): blocked in its read loop, dcload has no safe point to publish the frontier from, so the host's window never slides — measured 758 ms per 16 KB chunk, 19 KiB/s, before the change; 239 KiB/s and zero resends after. It is left ARMED for the pre-EXEC upload, which needs it (a blind-burst 1.5 MB upload never converges). That phase change required a matching host fix: `ACKS_SUPPORTED` in `dcload-ip-rs/src/dispatch.rs` used to latch `Some(true)` forever and now **demotes** when a DC that acked goes silent. **The two halves must stay in step.** |
| `CDFS_SYNC_CHUNK_SECTORS` | `8` | isoldr's `emu_async`, on the synchronous path only: ask the host for this many sectors per request instead of the whole read. 8 is DreamShell's preset for SA (`dma = 0, async = 8`) **and** 16 KB — exactly the BBA RX ring, so no chunk can outrun the ring the way a 32 KB read does. `0` = old single-shot. With `CDFS_ASYNC=0` this is **resumable**: cmd 17 asks for one chunk, answers PROCESSING, and each `gdGdcGetCmdStat`/`gdGdcExecServer` advances one more (`cdfs_sync_step`). That is isoldr's `gdcExitToGame()` with the game's own polling as the resume point — dcload has no coroutine. **Do not turn this back into a loop inside the syscall**: the first version did, and it RESET the machine at ~103 reads, because a TX is only safe at syscall top level (`pkt_buf` is shared and `bb->loop()` builds into it). Costs 472 resident bytes (headroom 708 → 236). Counters: `g_cdfs_sync_chunks`, `g_cdfs_sync_retries`. |
| `G1_DMA_IRQ_STUB` | `0` | Drain the ASIC DMA-done bits (`SB_ISTNRM` 12..18) our own design orphans, once per GD syscall. Titles arm that group on one interrupt bank and drain it in that bank's handler, kept alive by ordinary disc traffic; we serve sectors over UDP, so no G1 burst completes, the handler stops running, and whatever else lands there (Maple) latches forever. Costs **96 resident bytes** (headroom 196 → 100). It cannot *raise* the completion — `SB_ISTNRM` is write-1-to-clear. **Measured on SA, 2026-08-07**: drains exactly bit 12 (`g_g1_dma_last = 0x1000`), and the storm goes from ~95k interrupts/s to **0**; the title then reaches 106 reads instead of ~76 and reconfigures its framebuffer (RGB0888@0x600000 → RGB565@0x200000), but still stalls with a black screen, now looping in its own code at `0x8c0379xx` instead of the interrupt dispatcher. So: real effect, not a fix. See §18 and `g1_dma_irq_stub()`. |
| `RX_HEADER_STABLE_RESYNC` | `0` | Recover early when CBR is frozen *and* the header is implausible. With `0` the streak is still computed but never read, and `g_rx_header_stable_resyncs` stays 0 — a default build cannot tell you whether enabling it would help. |

### 4.4 Memory map (load addresses)

dcload is back at its **original low base, `0x8c004000`** (2026-08-08),
after two years in high RAM. Pinned by `dcload.x`:

- `0x8c004000`–`0x8c00b39c` — `dcload` resident set. Split in two by the
  linker script: everything up to `_resident_end` must survive while a
  game runs (`ASSERT(_resident_end <= 0x8c00b400)`); `.transient.text`,
  `.transient.data` and `.transient.bss` sit above it (to `_end` =
  `0x8c00ef90`) and may be clobbered after `EXEC`.
- `0x8c00f400` — `_stack`, **and** the VBR handed to the game, **and**
  the base of `exception` (linked at `-Ttext=0x8c00f400`, 3 KB), **and
  THE BIOS VBR on this machine**. `ASSERT((_stack - _end) > 1024)`
  guards the stack budget (currently 1136); if it fires, do NOT lower
  it — take the bytes from `BIN_INFO_MAP_SIZE` (commands.c), which is
  adjacent to the stack.
- `0x8c010000` — the game's load address. `exception.bin` ends exactly
  here: the total footprint is `0xc000` and so is the hole between the
  BIOS syscall area and 1ST_READ.BIN. Exact fit, by construction.
- `0x8cf0c000` — post-mortem block (`PM_BASE`, cdfs_syscalls.c). **Left
  in high RAM deliberately**: at the low base it would land on
  `0x8c010000`, the game's load address. Now fully detached from
  dcload's body, and `loader.s`' zero-fill no longer reaches it at all,
  so it survives reboots better than before.
- `loader.s` zero-fills `0x8c004000`–`0x8c010000` before copying the
  embedded binaries in.

Why this base — three reasons, in order of weight:

1. **DreamShell's per-game database says so for the title that drove the
   move.** All four Sonic Adventure presets (`SONIC ADVENTURE` ×2,
   `INTERNATIONAL`, `LIMITED EDITION`, each for cd/sd/ide) carry
   `memory = 0x8c000100` or `0x8c004000`, plus `dma = 0`, `async = 8`,
   `irq = 0`. Across all 6168 presets, `0x8cf00000` appears **zero**
   times. Archived in `docs/dreamshell-presets/`.
2. **`0x8c00f400` is the BIOS VBR**, and `exception.bin` lands exactly
   there from this base. isoldr hands every title that same VBR
   (`startup.s`, `_boot_vbr`). From high RAM we were handing out an
   address no retail title has ever seen.
3. **The resident bound becomes a real boundary**: `0x8c00b400` keeps
   the resident set below `0x8c00c000`, the start of the BIOS work area
   retail titles fill (see `loader.s`). It used to be inherited
   discipline rescaled mechanically.

The hazards that justified the high bases are preserved in `dcload.x`'s
header and still true — but they only ever ruled out *other high*
addresses. SA's large asset DMAREADs cover `0x0cd00000..0x0ce40800`,
which evicted `0x8ce00000` and never comes near `0x8c004000`.

Measured after the move: upload, `EXEC` and asset reads all work, and a
session shows a **single** `REIOS: Booting up` — dcload cohabits with SA
from low RAM. It did **not** fix SA's black screen (see §18).

`crt0` still installs VBR at boot. At this base the BIOS VBR again
points exactly at `exception.bin`, so it would work by accident — keep
the explicit install anyway, so the next relocation does not reintroduce
a silent reset loop.

The example programs' ABI (`0x8c004004` magic, `0x8c004008` syscall
trampoline) is **correct again** at this base, for free.

## 5. 1st_read bootstrap (`target-src/1st_read`)

`target-src/1st_read/Makefile` builds the 1st_read CD image
(`1st_read.bin`) by linking two tiny `.s` files against the two
`.bin` outputs of `target-src/dcload`:

```
loader.s     # disable IRQs, zero 0x8cf00000–0x8cf0c000, copy
             # exception.bin to 0xacf0b400, copy dcload.bin to
             # 0xacf00000, jump there. All sizes are baked in as
             # symbol differences at link time.
disable.s    # writes 0x00000808 to CCR (0xff00001c) to turn off
             # the SH4 cache. Lives in P2 area (0xa0000000).
```

Order of operations:

1. `rm-elf` — deletes stale `1st_read.bin`/`loader.elf`.
2. `loader.elf` — links `loader.s` + `disable.s` at
   `-Ttext=0x8c010000` (the 1st_read load address set by the
   Dreamcast BIOS). Requires `dcload.bin` and `exception.bin` to
   exist.
3. `loader.bin` — `objcopy -O binary`.
4. `1st_read.bin` — runs **KOS's `scramble` utility** on
   `loader.bin`. Retail Dreamcasts refuse to execute a binary
   that hasn't been scrambled.

The dcload magic-number handshake used by example programs lives at
`DCLOADMAGICADDR` = base + 4 and the syscall trampoline pointer at
base + 8 (read by the SH-ELF code in `example-src/dcload-syscall.s`).
`example-src` still hard-codes the pre-relocation `0x8c004004` /
`0x8c004008` — see §4.3.

## 6. Generated files / gotchas

- **Every translation unit** in `target-src/dcload/` and
  `example-src/` emits a `*.asm` sibling via
  `-Wa,-adghlmns=$*.asm`. The pattern is a superset listing
  (assembly, debug, etc.). These are **build output** — never
  hand-edit them. `make clean` removes them. They are committed
  to the tree in some places by accident; do not be alarmed.
- A few of these listings are huge:
  - `cdfs_syscalls.asm` (~95 KB)
  - `commands.asm` (~135 KB)
  - `dcload.asm` (~89 KB)
  - `syscalls.asm` (~115 KB)
  They are not sources, they are listings; ignore their size.
- `.gitignore` at the top level covers `*.o`, `*.bin`, `*.lzo`,
  `*.srec`, `*.exe`, `*.elf`, `*.asm`, `*.map`, plus the three
  example programs and the DC ELF. Subdirs are not independently
  gitignored, so intermediate files do get committed sometimes.
- The `*.asm` flag set is `-adghlmns=`:
  - `a` — include directives
  - `d` — debug
  - `g` — expand only basic blocks (not full expansion)
  - `h` — include high-level source
  - `l` — include assembly
  - `m` — include macros
  - `n` — omit forms processing
  - `s` — include symbols
- Do not change the protocol struct layouts lightly. The packed
  structs in `host-src/tool/commands.h` and
  `target-src/dcload/syscalls.h` are wire-format; the on-the-wire
  definitions and the in-memory layouts on both ends must match
  exactly. Every new command needs edits on both sides.
- The handshake uses a "DCLOAD is loaded" sentinel at
  `0x8c004004` (`0xdeadbeef`). The example programs check it
  before issuing syscalls — see
  `example-src/dcload-syscalls.c:5-9` for the canonical pattern.

## 7. PC host tool build (`host-src/tool`)

### 7.1 What it produces

`dc-tool-ip` (or `dc-tool-ip.exe` on Windows). Linked against:

- `libelf` (default) or sh-elf `libbfd` + `libiberty` + `libsframe`
  (Binutils ≥ 2.40) + `libintl` (MinGW auto-flips to BFD path).
- `-lz` on MinGW (and on BFD builds where `ZLIB_REQUIRED := 1`).
- `-lws2_32 -lwsock32 -liconv` on MinGW.

Sources:

- `dc-tool.c` — single-file CLI; ~1600 lines. Hosts a bundled
  BSD-style `getopt` fallback when the system has none. Reads
  the dc-tool version out of `PACKAGE_VERSION` (which is
  `DCLOAD_VERSION` from `Makefile.cfg`).
- `syscalls.c` — implements the per-syscall send/recv
  (`dc_open`, `dc_read`, …) that mirror the `DC00`–`DC22`
  command set on the DC side.
- `shim.c` — MinGW compatibility layer for `vasprintf`,
  `libintl_asprintf`, `__ms_vsnprintf`, `_imp____acrt_iob_func`,
  only built under the original (non-MSYS2) MinGW.
- `unlink.c` — Windows `unlink` wrapper.
- `utils.c` / `utils.h` — logging, IP helpers, and the
  `exception_struct_t` (the register-dump format).

### 7.2 `dc-tool-ip` command line

The available options, from `dc-tool.c:1356-1358` (string is
`"x:u:d:a:s:t:i:nlqhrgf"` on MinGW, with `m:c:` added under
non-MinGW):

| Flag | Argument | Meaning |
| ---- | -------- | ------- |
| `-x` | `filename` | Upload `filename` and execute (default load address `0x0c010000`) |
| `-u` | `filename` | Upload `filename`, do not execute |
| `-d` | `filename` | Download to `filename` |
| `-a` | `address` | Override default upload address (default `0x0c010000`) |
| `-s` | `size` | Override download size |
| `-t` | `ip[:port]` | Target address; port optional, default `53535` |
| `-n` | — | Do not attach console / fileserver |
| `-q` | — | Do not clear screen before download |
| `-m` | `path` | Map `/pc/` on the DC side to `path` (no chroot/root needed) |
| `-c` | `path` | Chroot to `path` (root required) |
| `-i` | `isofile` | Enable CDFS redirection using this `.iso` |
| `-r` | — | Reset the DC (only when dcload is in control) |
| `-g` | — | Start a GDB server on TCP `:2159` |
| `-l` | — | Force legacy 1024-byte payload (v2.x only) |
| `-f` | — | Disable FIFO delays; **much** faster but increases packet loss |
| `-h` | — | Usage |

Only one of `-x`/`-u`/`-d`/`-r` may be used in a single
invocation (enforced in `main()`). `-m` and `-c` are mutually
exclusive.

The tool runs a version handshake before doing anything
(`prepare_comms`): it sends `CMD_VERSION` stuffed with the encoded
`dc-tool` version (`(major<<16)|(minor<<8)|patch`),
detects whether the target is legacy (port 31313) or v2 (port
53535), and identifies the adapter (BBA vs LAN-Adapter). If
`-l` is given, it forces the legacy code path.

Packet timeout: `PACKET_TIMEOUT = 250000` µs (250 ms).

### 7.3 Networking constants (worth memorising)

- UDP port: **53535** (v2.0+), legacy **31313** (v1.x).
- `dc-tool -t <ip>:<port>` overrides the UDP port.
- `dc-tool -l` forces legacy 1024-byte payloads. Default payload
  is 1452 B (1494 B with headers). Legacy mode caps payload size
  at 11 MB.
- GDB stub listens on TCP **:2159** (always this port).
- BBA runs at 100 Mbit; LAN Adapter at 10 Mbit. The driver
  chooses its delay knobs based on the detected adapter.
- `dc-tool -f` disables FIFO pacing (faster, but you will get
  dropped packets on long cable runs).

## 8. Protocol reference (verify against the wire)

`host-src/tool/commands.h` is the canonical list:

| ID | Name | Use |
| -- | ---- | --- |
| `EXEC` | `CMD_EXECUTE`  | execute |
| `LBIN` | `CMD_LOADBIN`  | begin receiving binary |
| `PBIN` | `CMD_PARTBIN`  | part of a binary |
| `DBIN` | `CMD_DONEBIN`  | end of binary |
| `SBIN` | `CMD_SENDBIN`  | upload a binary from DC |
| `SBIQ` | `CMD_SENDBINQ` | upload, quiet |
| `VERS` | `CMD_VERSION`  | version handshake |
| `RETV` | `CMD_RETVAL`   | return value |
| `RBOT` | `CMD_REBOOT`   | reboot |
| `MAPL` | maple packet   | host-to-DC maple |
| `PMCR` | perf counter   | host-to-DC PMCR |
| `EXPT` | exception dump | DC-to-host on crash |

The 4-byte `command_t` ID is the only field interpreted as a
string; `address` and `size` are big-endian on the wire
(`ntohl`-converted on receive), followed by `size` bytes of
variable-length data.

`syscalls.h` (mirrored on both ends) defines the per-syscall
string IDs `DC00`–`DC22`:

| ID | Use |
| -- | --- |
| `DC00` `DC01` `DC02` `DC03` `DC04` `DC05` `DC06` `DC07` `DC08` `DC09` `DC10` `DC11` `DC12` `DC13` `DC14` `DC15` `DC16` `DC17` `DC18` `DC19` `DC20` `DC21` `DC22` | exit, fstat, write, read, open, close, creat, link, unlink, chdir, chmod, lseek, time, stat, utime, bad, opendir, closedir, readdir, cdfsread, gdbpacket, rewinddir, cdfstoc |
| `DD02` | legacy write variant (kept for v1.x compat) |
| `EXPT` | exception dump |

The `command_t` family structs (`command_3int_t`,
`command_int_string_t`, etc.) are packed and `aligned(4)` on the
target side; on the host they are only `packed`. Adding a
command means a new `CMD_*` macro in both `commands.h` files, a
new string ID, and a handler in `target-src/dcload/commands.c`
and the host side in `host-src/tool/syscalls.c`.

## 9. Example programs (`example-src`)

Three tiny demo programs; the directory is otherwise a template
for "how to build a DC user program that uses dcload syscalls":

- `crt0.S`, `startup_support.c`, `dcload-syscall.s` — minimal
  startup + the syscall trampoline (which reads the pointer at
  `0x8c004008` and jumps to it).
- `dcload-syscall.h` — low-level syscall number macros
  (`pcreadnr` … `pcgethostinfo`).
- `dcload-syscalls.c` / `dcload-syscalls.h` — POSIX-y wrappers
  (`read`, `write`, `open`, `exit`, `time`, `gethostinfo`, …)
  that go through `dcloadsyscall(...)`. They all guard on
  `*DCLOADMAGICADDR == DCLOADMAGICVALUE` so that the same ELF
  can be linked into a non-dcload program and fail gracefully.
- `dc3.x` / `dc4.x` — linker scripts. `example-src/Makefile`
  picks one via `dc$(TARGETCCVER).x` (`TARGETCCVER` is clamped
  to 4 in `Makefile.cfg`, so on modern toolchains you always
  use `dc4.x`).
- The three demos are:
  - `console-test.c` — `open("console-test.c", O_RDONLY)` and
    `write` it to the host. Exercises the file-server path.
  - `exception-test.c` — forces an FPU exception via
    `__call_builtin_sh_set_fpscr` + `fdiv fr0, fr1`. Exercises
    the exception dump path. Uses `dcload-syscall.h` directly
    (not the wrapper header) because it is intentionally
    unsafe.
  - `gethostinfo.c` — calls `gethostinfo(&our_ip, &tool_ip, &tool_port)`
    and prints the result via the host's `write` console.

Builds output as `console-test.bin`, `exception-test.bin`,
`gethostinfo.bin` (raw `objcopy -O binary`). Run with
`dc-tool -x console-test` (etc.) from this directory.

`dc-tool` looks for the file on the host's current working
directory and uploads it byte-for-byte. The default load address
is `0x0c010000` (override with `-a`).

## 10. CD / disc image recipes

`make-cd/` and `make-cdi/` are **separate from `make all`** — they
are not pulled in by the top-level Makefile. They both fetch
`1st_read.bin` from `target-src/1st_read/`:

### `make-cd/` — burn a real CD-R

Uses `wodim` (CDRECORD) and `genisoimage` plus KOS's `makeip`
(`/opt/toolchains/dc/kos/utils/makeip/makeip`) to build `IP.BIN`
from `ip.txt` + `iplogo.png`. The recipe writes an audio track
of silence, then the data track. The CD recorder is hard-coded
to `dev=0,0,0` — change it for your setup. Requires a real
optical drive.

### `make-cdi/` — produce a DiscJuggler image

Uses KOS's `mkdcdisc` at
`/opt/toolchains/dc/bin/mkdcdisc` with `-N -m` flags. Output is
`dctoolip.cdi` in the current directory. Use this if you want
to run the disc in an emulator (e.g. nullDC, Demul, Flycast) or
burn it later with ImgBurn.

If you just want the file to drop on a CD yourself, copy
`target-src/1st_read/1st_read.bin` and `make-cd/IP.BIN` per the
README.

## 11. GDB / debugging

- GDB-on-dcload port: TCP `:2159` on the host running
  `dc-tool -g -x <prog.elf>`. From `sh-elf-gdb`:
  `target remote :2159`. Add `gdb_init()` to your KOS program
  to enable.
- In-ICE GDB port: TCP `:3263`. `.gdbinit` and
  `.vscode/launch.json` are preconfigured for this. Always set
  `arch sh4` and `endian little` before connecting.
- The dcload side dumps a full register context to a file called
  `dcload_exception_dump.bin` in the terminal's CWD whenever a
  loaded program faults. Format is the `exception_struct_t` in
  `host-src/tool/utils.h` and `host-src/tool/utils.c`.
- **flycast-as-target debug loop** (build → CDI → flycast on Windows →
  GDB attach on `:3263` → game upload via `dcload-ip-rs`): see
  `docs/flycast-debug-loop.md` and `scripts/flycast-debug-loop.sh`.
  Requires flycast built with `-DENABLE_GDB_SERVER=ON` — the config
  flag `Debug.GDBEnabled` alone does nothing on a build without it.
  Such a build starts the emulation **suspended**; release it with
  `scripts/flycast-resume.py`. Any tool touching `:3263` must end with
  a GDB `detach`, or the target stays halted and looks frozen.
- **Reading DC state without stopping it**: `scripts/dc-peek.py
  <symbol|0xaddr> [--len N]` resolves symbols from the dcload ELF and
  reads guest memory over raw GDB packets. `scripts/dc-sample.py` polls
  a table of counters at a fixed rate and prints deltas;
  `scripts/dc-freeze.py` snapshots everything during a freeze. Never
  hard-code counter addresses in a script — adding a counter shifts
  `.data` and the values become plausible nonsense.
- **Seeing what the console DISPLAYS** → `scripts/dc-screen.py <out.png>` reads
  the framebuffer the PVR scans out of guest VRAM over the GDB stub, and writes
  a PNG with `zlib` alone. Screenshotting the flycast window is a dead end (a
  Vulkan surface), but the pixels themselves are just guest memory. Geometry and
  format come from `FB_R_CTRL`/`FB_R_SOF1`/`FB_R_SIZE`. Answers "where is the
  title stuck?", which no counter can.
- **Asking WHERE a write was aimed** → `scripts/dc-regs.py` dumps the MMIO
  registers that select a destination: `QACR0/1` (store queues), `SB_C2DSTAT` /
  `LMMODE` and the SH4 DMAC channel-2 set (`SAR2`/`DAR2`/`DMATCR2`/`CHCR2`), and
  the TA's own output pointers. A mis-aimed transfer leaves its aim behind, so
  this is the post-mortem to run on a frozen machine. `DAR2 = 0` is normal on
  this console — see §18 before reading it as a null pointer.
- **You cannot trap a write as it happens under flycast.** Its GDB stub answers
  `Z1/Z2/Z3/Z4` with an empty packet (unsupported); only `Z0` software
  breakpoints work, and only with `len == 2`. Its SH4 UBC is 41 lines of
  register storage with no compare logic anywhere in the memory path, so
  programming `BARA` from dcload would compile, run, and never fire. To catch a
  writer, use a **host-side hardware watchpoint with `cdb.exe`** on the
  translated host address, or add a hook in flycast and rebuild.
  `scripts/dc-catch-vector-write.sh` does the cdb route for the GD vector at
  `0x8c0000bc`: it derives the legitimate value from `cfs_redir_k` in the ELF,
  reads the RAM base from `flycast.log` (**last** `RAM(16 MB)` line — it is
  logged twice and the base changes every run), and arms `ba w4`. `--verify`
  proves the address translation against the GDB stub before you spend a run on
  it; `--dry-run` just prints what would be armed. It carries a **built-in
  positive control**: x64 data breakpoints are per-thread and flycast emulates
  on its own thread, so the script arms before launch and treats dcload's own
  `cfs_redir` install — a guest write, on the emulation thread — as proof the
  watchpoint is live. No `SELFTEST-OK` in the log means the result is
  meaningless, not that nobody wrote. Generalise the same shape for any other
  address; see the memory `flycast-debug-toolbox` for the `ww*.ps1` originals.
- **Counting what the EMULATOR does, at full speed** → the `dcdiag` patch in
  the flycast tree (`core/hw/holly/dcdiag.h`, plus increments in
  `holly_intc.cpp` and `ta.cpp`), read with `scripts/flycast-counters.sh`
  (`[interval]` for rates). Header-only + definitions in an existing TU, so
  **no CMake change and no new source file**; it does need a flycast rebuild,
  which is on the user's MSVC side. Counters: Holly interrupts raised per
  `SB_ISTNRM`/`EXT`/`ERR` bit, guest acknowledgements per bit, and — the point
  of the exercise — TA lists **opened / closed / parameters submitted**, per
  list type. A list opened and never closed raises no interrupt, so no
  interrupt counter can distinguish "never submitted" from "submitted and never
  terminated". Prefer this to `bp flycast!asic_RaiseInterrupt`: that breakpoint
  costs a debugger round trip per interrupt and drops VBlank from 60/s to
  1.7/s, so it can answer yes/no and nothing else — every rate measured under
  it, and every GDI-vs-dcload comparison, is void (§18).
- **Always-compiled forensics** (no `CDFS_TRACE` needed, read with
  `dc-peek`): `g_gd_idx_counts[]` histogram and `g_gd_seq[]` ring of the
  last GD syscalls (this is what identifies the condition a title is
  spinning on), `g_gd_calls` / `g_gd_ticks`, the RX counters in
  `rtl8139.c`, and the post-mortem block at `PM_BASE` which survives a
  reboot.
- **Checking the DATA, not the accounting** → `dcload-ip-rs --verify-reads`,
  documented in `docs/read-back-verification.md`. Every counter above is
  arrival accounting; this is the only instrument that compares delivered
  bytes against sent bytes (`SBIQ` read-back in the window between the last
  PBIN and the RETV). Reach for it whenever the transfer log is clean and the
  title still misbehaves. It needs no DC-side change — `cmd_sendbinq` is
  already resident.
- **Checking dcload's OWN image** → `scripts/dc-integrity.py`, over GDB. Every
  other instrument watches the data path; this one answers "did the title write
  over the loader?", which all of them are blind to. `selfwr` only covers CDFS
  writes. Compares `.text`/`.rodata`/`.transient.text`/`exception.bin` against
  the ELF, or against a `--save-baseline` taken while the game runs (which
  absorbs dcload's own legitimate writes and needs no benign-list).
  Measured on Sonic Adventure, 2026-08-07: **`.text` intact except 8 bytes at
  `0x8cf00000`** — `start:` and the `0xdeadbeef` magic, both boot-only — and
  the title's own exception dispatcher installed at VBR+0x100/+0x400/+0x600
  over `exception.bin`. See §18.

## 12. CI and branching

- **Primary CI is GitLab**, defined in `.gitlab-ci.yml`:
  - Stage 1 (`build:kos-scramble`) — clones KOS, builds the
    `scramble` utility, and tarballs it as an artifact.
  - Stage 2 (`build:dcload-gcc4`) — uses
    `segadreamcast/toolchains:binutils-2.34-gcc-4.7.4-newlib-2.0.0-gdb-9.1`,
    unpacks the tarball, `source`s KOS `environ.sh`, and runs
    `make && make install`.
  - Stage 3 (`build:dcload-gcc9`) — same with
    `…-gcc-9.3.0-newlib-3.3.0-…`.
- **GitHub Actions only mirrors** `master` to
  `gitlab.com/kallistios/dcload-ip.git`
  (`.github/workflows/sync-to-gitlab.yml`). No tests run on
  GitHub.
- **Default branch is `master`.** Issues and PRs go to the
  GitHub repo; the GitLab repo is read-only mirror.
- The image is `segadreamcast/toolchains` on Docker Hub.

## 13. Cross-platform quirks (worth knowing for builds)

- **MinGW** — `Makefile.hostdetect` sets `MINGW` for the
  original MinGW/MSYS and `MINGW64` for MSYS2. `WITH_BFD` is
  auto-flipped to 1, so dc-tool is linked against sh-elf
  `libbfd` + `libsframe` (when present) + `libiberty` + `libintl`.
  `shim.c` provides `vasprintf` etc. for the original MinGW.
- **Cygwin** — `WINDOWS` and `CYGWIN` are set; behaves like
  MinGW for build flags.
- **macOS** — `MACOS` is set, `libelf` is included from
  `/opt/homebrew/{include,lib}`, `-DMACOS` is added to the host
  compile so `dc-tool.c` does `#include <libelf/libelf.h>`
  rather than `<libelf.h>`. No `Makefile`-level handling for
  Apple Silicon vs Intel; just use the Homebrew prefix.
- **BSD** — `BSD := 1` is set but no other special handling
  exists; you may need to install `libelf` via your package
  manager.

## 14. Common pitfalls (so you don't burn a CD for nothing)

1. **Forgetting `source` of KOS `environ.sh`** — the first
   `make` will fail at the first `$(TARGETPREFIX)/bin/sh-elf-gcc`
   invocation with "command not found".
2. **Building `target-src/1st_read` before `target-src/dcload`**
   — the `.incbin` lines for `dcload.bin`/`exception.bin` will
   fail.
3. **Building `make-cd/` or `make-cdi/` from the repo root** —
   they are not part of the root `make`. `cd` into them
   explicitly, or you'll get a "no rule" error.
4. **Editing `*.asm` files** — they are build listings and
   will be regenerated. Edit the `.c` / `.s` / `.S` source
   instead.
5. **Putting a hand-coded IP in `Makefile.cfg` and being
   surprised when it doesn't reach the DC** — remember to also
   configure the host's `arp` table (or use DHCP). The README
   has the exact recipe.
6. **`DREAMCAST_IP = 0.0.0.0` is DHCP**, not "any IP". The
   `0.x.x.x` block is reserved for DHCP. If you want static,
   use `169.254.x.x` plus `arp`/`netsh` on the host.
7. **Bumping `VERSION` only in one place** — `VERSION` lives in
   `Makefile.cfg`. Both `host-src/tool/Makefile` and
   `target-src/dcload/Makefile` already consume it via
   `-DDCLOAD_VERSION=\"$(VERSION)\"`. Do not redefine it
   elsewhere.
8. **Adding a syscall ID on one side only** — `commands.h` and
   `syscalls.h` exist on both host and target; both need the
   same `CMD_*` and `CMD_*` macros, and the struct layouts
   (e.g. `command_3int_string_t`) must match exactly.
9. **Trying to use `dc-tool -l` against a v1.x dcload** — `-l`
   is the v2-only "force legacy payload" flag, not a
   "v1-compat" flag. To talk to v1.x dcload, just run dc-tool
   without `-l`; the version handshake will pick the right
   socket/port.
10. **Running `make` from a subdirectory** — the root
    `Makefile` is the only one that sets up the subdir order
    (`host-src` → `target-src`). If you `cd` into a subdir and
    `make`, you lose the host-built-first ordering for the
    `target-src: host-src` rule.
11. **Macros leaking between subdirs** — `Makefile.cfg` is
    included by every subdir, so if you add a new `-D…` macro
    there it will affect all builds. If it is host-only or
    target-only, put it in the relevant subdir's Makefile
    instead.
12. **Changing `DREAMCAST_IP` and expecting a rebuild** — it is a
    `-D` with no dependency, so `make clean` is mandatory. (The
    `target-src/dcload` flags in §4.3 *do* trigger a rebuild; this
    one does not.)
13. **Rebuilding and then testing the old image** — `make` alone does
    not regenerate `dcload.cdi`, and regenerating it does not deploy
    it. Always run `mkdcdisc` and copy the result, then prove the
    deployed image really contains the new binary by *containment*
    (search for the first 64 bytes of `1st_read.bin` inside the CDI)
    — never by timestamp or size, since a stale CDI has exactly the
    same byte count.
14. **Hard-coding an address that the linker owns** — the GD-vector tripwire
    in `cdfs_syscalls.c` compared `0x8c0000bc` against a literal `0x8cf00444`.
    An unrelated edit removed four bytes from the fixed jump table in
    `dcload-crt0.s`, `cfs_redir` moved to `0x8cf00410`, and the tripwire then
    "repaired" the vector to the stale address on the first GD syscall. The
    second syscall jumped 0x34 bytes into `dispatch_game`; with `SR.BL=1` at
    handoff the SH4 does not take that exception, it goes to the **reset
    vector**, so the machine rebooted at `EXEC` leaving no breadcrumb — the
    reset had cleared the post-mortem block too. It now compares against
    `cfs_redir_k`, the very word `cdfs_redir_enable` writes. Symptom to
    recognise: `REIOS: Booting up` in `flycast.log` right after a clean upload.
15. **Adding a section to `dcload.x`** — re-check everything that
    consumes `_edata` / `_end`. Those symbols stopped meaning "all of
    the BSS" when `.transient.*` was introduced, silently, and `crt0`
    started erasing `main()`.
16. **Assuming a guard still guards** — `scripts/check-resident-invariant.sh`
    had a hard-coded address prefix and silently reported a clean bill
    of health for weeks after a relocation. Verify a check still
    detects something before trusting it.

## 15. Where to look first when changing something

- **Bumping the wire protocol** → `host-src/tool/commands.h`,
  `host-src/tool/syscalls.h`, `target-src/dcload/commands.c`,
  `target-src/dcload/syscalls.c`, `target-src/dcload/commands.h`,
  `target-src/dcload/syscalls.h`. Both sides must change in
  lock-step.
- **Tuning throughput** → `Makefile.cfg` (FIFO delays) and
  `dc-tool.c` (`PACKET_TIMEOUT`, `BBA_RX_FIFO_DELAY_*`).
- **Changing the DC base address or memory map** → `dcload.x`,
  `target-src/1st_read/loader.s`, `dcload-crt0.s`, `go.s`,
  `exception.S`, `cdfs_redir.s`, `commands.c`,
  `cdfs_syscalls.c` (`DCLOAD_RESIDENT_*_PHYS`, `PM_BASE`),
  `target-src/dcload/Makefile` (`exception -Ttext=`), plus
  `scripts/check-resident-invariant.sh` and `scripts/dc-peek.py`.
  Substitute on code lines only — the prose in comments describes
  measurements made at older bases and rewriting it would falsify the
  record. The link step (`--no-undefined` plus the `dcload.x` asserts)
  catches inconsistencies, so a broken relocation does not flash.
- **Touching the CDFS read path** → `cdfs_syscalls.c` (slots, statuses,
  retry policy) and `commands.c` (`cmd_partbin` / `cmd_donebin`, the
  frontier and `win_mask` accounting). The frontier logic exists in two
  places that MUST agree; `scripts/frontier_fuzz.c` fuzzes it against a
  brute-force reference — run it before changing either.
- **Adding anything that must work while a game runs** → check it lands
  below `_resident_end` and that nothing it calls is transient; run
  `scripts/check-resident-invariant.sh`. Remember `LBIN`/`PBIN`/`DBIN`
  are the CDFS *data* path, not just the upload path.
- **Adding a new example program** → copy one of the existing
  three in `example-src/`, add it to the `OBJECTS`/`all` list
  in `example-src/Makefile`, and add a `-Wl,-T…` line if it
  needs its own link script.
- **Adding a new on-screen status field** → `video.s` for the
  primitive text routines, `dcload.c` for the loop that paints
  them, `Makefile.cfg` (no change required, but
  `EXCEPTION_SECONDS` already drives the on-screen timeout).
- **Adding a new Maple or PMCR command** → README section, no
  new syscalls required; the host `dc-tool` is the dispatcher
  and forwards packets verbatim.
- **Updating the build for a new KOS** → check
  `$(KOS_BASE)/utils/scramble/scramble` still exists and that
  the toolchain image tags in `.gitlab-ci.yml` are still
  published.

## 16. Server side

A Rust server has been written, with more features and speed:
`/mnt/e/Nextcloud/Projets/Dreamcast/dcload-ip-rs/` (mainly
`src/dispatch.rs`). It is the host side used for CDFS work.

Two things about it that the DC side depends on:

- It advertises `PROTOCOL_VERSION_MODERN = [2,1,0]`, which is what gates
  `CMD_ACKBIN` ("ABIN") progress acknowledgements in `commands.c`. Do not
  lower it without removing that gate. `dc-tool-ip` (2.0.4) does not
  negotiate it and keeps the older blind-burst path.
- It serves CDFS reads **strictly one at a time**: the whole
  LBIN/PBIN/ABIN/DBIN exchange runs to completion inside its request
  handler. With dcload's four slots, three reads are normally queued and
  look, from a slot's point of view, exactly like lost requests. That is
  why `cdfs_last_data_tick` exists on the DC side.

## 17. Reference / source-of-truth files

If something in this document conflicts with the code, the code
wins — but the most likely places to look are:

- `Makefile.cfg` — toolchain, tunables, version.
- `Makefile.hostdetect` — host flags.
- `host-src/tool/dc-tool.c` — CLI and protocol behaviour.
- `target-src/dcload/dcload.c` — DC main loop.
- `target-src/dcload/dcload.x` — DC memory map.
- `target-src/1st_read/loader.s` — bootstrap flow.
- `host-src/tool/commands.h` and `target-src/dcload/syscalls.h`
  — wire-format IDs and structs.
- `README.md` and `CHANGES` — user-facing behaviour and history.
- `docs/loader-comparison.md` — three-way comparison of DreamShell isoldr,
  dc-virtcd and dcload on the same problem (interrupts, async reads, memory
  cohabitation, syscall hooking, GDC command coverage, hacks,
  instrumentation). The reference to consult before inventing a mechanism.
  Its §8 lists what is still open; its dcload column is DATED (2026-08-07)
  and must be refreshed whenever a mechanism changes.
- `docs/flycast-debug-loop.md` — the emulator-as-target workflow.
- `docs/read-back-verification.md` — `--verify-reads`: what it proves, what it
  cannot, and how to read a mismatch report.
- `Cdif131e.txt` — SPI / GD-ROM command spec, in the repo.

## 18. Cohabitation with Sonic Adventure — measured, not assumed

From GDB reads on a live freeze plus 11 host logs (2026-08-07), with
`scripts/dc-integrity.py`. This EXTENDS §4.4 and the agent memory
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

## 19. The black screen is a transition, and SA's display lists are empty

Measured 2026-08-08 with the `dcdiag` counters (§11), at **full speed** — 60
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

## 20. The GD driver is now a server task (isoldr model) — 2026-08-10

This section describes code that **is** in the tree, unlike §§4.3/18/19.

### 20.1 What changed

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
  invariant of §4.3 holds.
- The syscall table is isoldr's full 18 entries; the `r6 == -1` misc calls are
  no longer claimed. The old `cmp/hs` range check admitted one index past the
  table — now `cmp/hi`.

**Verified working**: 86 reads served per SA session, 16 KB chunked requests
visible on the host, `gd_server_live = 1`, lock taken and released correctly.
It did **not** fix SA (see 20.3).

Hazard specific to this design, written on the assembly too: a parked frame is
restored onto whatever `r15` the next ExecServer arrives with, so **no function
live across a yield may take the address of a local**.

### 20.2 `REIOS: Booting up` does NOT mean the machine reset

This reinterprets a lot of earlier evidence. `reios_boot` is registered as a
hook at address 0 (`reios.cpp`, `SYSCALL_ADDR(0xA0000000)`), so the line means
only that **the SH4 executed at physical address 0** — a jump through a null
pointer reaches it just as a reset vector does. flycast's CPU context is *not*
cleared on the way there.

For SA on this path there is **no SH4 exception at all**: the `[BBA-DIAG]`
logger in `Do_Exception` (verified present in the shipped binary — check the
string, not the source) prints nothing, neither the bounded prefix nor the
`FATAL exception while BL=1` line. So §14.14's "untaken exception under BL=1"
is not what happens here, even though `go.s` does hand over `SR = 0x500000f0`.

You cannot breakpoint address 0: `scripts/dc-trap.py` arms a `Z0` there and its
self-test proves the BIOS ROM ignores flycast's `trapa` patch. The self-test is
the point — without it a miss looks like a result.

### 20.3 Where Sonic Adventure actually dies

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

### 20.4 Two instruments that were lying, now fixed

- `scripts/dc-integrity.py` had `EXCEPTION_BASE = 0x8CF0B400` hard-coded from
  the old high base. Every run reported hundreds of differences in memory
  dcload does not own. It now derives the address from `exception`'s own ELF
  section header (its `.stack` section sits at `0x3fffff00`, so a
  lowest-symbol scan finds that instead — use `objdump -h`).
- **`PM_BASE = 0x8cf0c000` is no longer safe.** With dcload back at the low
  base nothing caps SA's allocator there any more, and SA overwrites the
  block: after a failure it reads `boots = 1, reads = 0`, i.e. re-claimed.
  §18's "allocator ceiling is exactly `0x0cf00000`" was a consequence of
  dcload sitting at `0x8cf00000`, not an intrinsic property of the title.

### 20.5 Reachability: dcload now announces itself

`announce_presence()` (net.c) sends a gratuitous ARP from the main loop.
Without it a static `DREAMCAST_IP` is unreachable **by construction**: the
host's first packet is unicast so it must ARP, flycast's BBA bridge does not
open its capture device until the guest has transmitted once, and the host's
neighbour entry decays to `Unreachable` — a state in which Windows discards
datagrams while `send()` reports success. DHCP hid this because DISCOVER is
that first frame. `Makefile.cfg` now carries `DREAMCAST_IP = 192.168.1.130`;
revert it to `0.0.0.0` for DHCP, and remember §14.12 (`make clean` required).

Also fixed: `cdfs_syscalls.c` did not include `<unistd.h>`, so the tree did
not build at all under GCC 15 (implicit `write` is an error now).

### 20.6 The fatal read, narrowed by elimination (2026-08-10, later)

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

### 20.7 RX ring: four rules the stock tree violated

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

### 20.8 The death is in SA's own code, not in the transfer (2026-08-10)

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

### 20.9 Delivered bytes are PROVEN correct, and tracing moves the failure

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
of `docs/read-back-verification.md` -- the last one standing after §20.6 -- is
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

### 20.10 Conformity review against isoldr's ARCHITECTURE.md

`firmware/isoldr/ARCHITECTURE.md` is the reference document for this port. Four
real divergences were found by reading it against the tree; three are fixed.

**FIXED -- the SR handed to the game (`go.s`). The important one.**
dcload loaded `SR = 0x500000f0`: MD=1, RB=0, **BL=1, IMASK=15**. Every interrupt
level masked and exceptions blocked, so a title could not take a VBlank, a Maple
completion or anything else until it cleared those bits itself, and any
exception it did take became a manual reset instead of a fault. isoldr hands
`0x60000101` -- BL=0, IMASK=0, RB=1. This is why §18's measurement of SA's
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

### 20.11 The "Missing 1440 bytes at address" storm

Three separate things, fixed or characterised.

**1. A regression I introduced (§20.10's bulk rule).** Adopting isoldr's
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

### 20.12 A real hang found, and the failure finally MOVES

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

### 20.13 ROOT CAUSE: Sonic Adventure's stack runs through dcload's BSS

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
address 0, which is exactly what "REIOS: Booting up" reports (§20.2). It is
also mutual: dcload writing `bin_info.map` writes into SA's live stack.

**This explains the entire investigation.** Deterministic (SA reaches that
depth at a fixed point in its loading sequence); no SH4 exception (jumping to
zero is a legal jump); dcload's counters reading as foreign data in earlier
samples (they ARE being overwritten -- that was SA's stack, not a mid-reboot
artifact as recorded in §20.8); the PC trace showing SA running its own code
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

### 20.14 SONIC ADVENTURE BOOTS AND PLAYS (2026-08-10)

Intro video, skippable, then gameplay in Station Square. **1624 disc reads
served, `boots = 1`** -- no reboot at any point. The ceiling had been 86 reads
and a guaranteed reset.

The cure was §20.13's root cause: **stop sharing memory with the title.**
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
4. **Bounded the two PHY waits** in `rtl_bb_loop` (§20.12) -- a real hang.
5. **Turn the tracing OFF.** `GD_TRACE_CALLER` costs a UDP round trip per
   event and SA alternates stacks on every GD syscall, so it fired ~120 times a
   second and was itself holding the title back: with it on, the game reached
   the AFS index reads and stopped; with it off, it plays. The instrument that
   found the bug must not be left in the running configuration.

Supporting fixes that were necessary but not sufficient: the RX ring rules
(§20.7), the gratuitous ARP and static IP (§20.5), the GCC 15 build fix, the
host's LoadBinary echo check and growing resend window (§20.11), and **two
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
