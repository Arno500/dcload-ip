include ../../Makefile.cfg

CC	= $(TARGETCC)
CFLAGS	= $(TARGETCFLAGS) -DDCLOAD_VERSION=\"$(VERSION)\" -DDREAMCAST_IP=\"$(DREAMCAST_IP)\" -DEXCEPTION_SECONDS=$(EXCEPTION_SECONDS) -Wall -Wextra -ffreestanding -fno-zero-initialized-in-bss -fno-common -fomit-frame-pointer -fno-strict-aliasing -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -fno-delete-null-pointer-checks -fno-stack-protector -fno-stack-check -fno-merge-constants -fno-merge-all-constants -std=gnu11
INCLUDE	= -I../../target-inc

#
# ---------------------------------------------------------------------------
# WHERE THE LOADER LIVES
#
# A retail title decides for itself which parts of RAM it treats as free, and
# some of them treat the hole this loader sits in as exactly that (AGENTS.md
# 4.6). DreamShell's per-game database answers the question title by title in
# its `memory` field, and the values it uses are these:
#
#   0x8c004000  593 games   the stock base, and the one homebrew depends on
#   0x8c001100  196         MIN_GINSU
#   0x8cfe8000  120         HIGH
#   0x8c000100   80         MIN
#   0x8ce00000   16
#
# So the base is per-game, not per-build, and the host picks it: it uploads a
# loader linked for the address the title needs and chainloads into it before
# uploading the title. `make loaders` below builds that set.
#
# 0x8c004000 STAYS THE DEFAULT and is what the CD image boots. It is not just
# tradition: the loader publishes its magic at base+4 and its syscall
# trampoline at base+8, and every KOS program and every example in
# example-src/ reads those two addresses as literals. A loader anywhere else
# is invisible to them.
#
# WHAT THE BASE DOES *NOT* MOVE: the register state handed to the title.
# isoldr sets VBR = 0x8c00f400 and SP = 0x8c00f400 for every title it launches,
# from every value of `memory` -- startup.s' _boot_vbr / _boot_stack are plain
# constants and only the NAOMI path overrides them. That address is the BIOS
# VBR, so it is what a retail title has always been handed. Moving the loader
# out of low RAM must therefore NOT move the guest's vectors with it, and
# exception.bin (which IS those vectors) stays linked at 0x8c00f400 whatever
# the base -- see DCLOAD_GUEST_VBR below and go.s.
#
# TWO LAYOUT FAMILIES, because the remaining placements are only correct for a
# base below 1ST_READ.BIN:
#
#   LOW (base < 0x8c010000)  The stock layout, unchanged. The loader's own
#                            stack descends from 0x8c00f400, which is directly
#                            above the image, and the big buffers sit far away
#                            in high RAM. Only ORIGIN moves.
#
#   HIGH (base >= …)         0x8cfe8000 and 0x8cfe9000 are INSIDE the image
#                            when the base is 0x8cfe8000, and a stack at
#                            0x8c00f400 would descend through exactly the low
#                            RAM the title asked us to vacate. So the loader's
#                            own placements are taken relative to the base:
#
#                              base + 0x0000  image (text/rodata/data/bss)
#                              base + …       stack, descending from _stack
#                              base + 0xb000  _stack (dcload's own)
#                              base + 0xc000  .hiram packet buffers (3 KB)
#                              base + 0xd000  Maple DMA buffer (2 KB)
#
#                            Span 0xd800, so a base above 0x8cff2800 does not
#                            fit in RAM. The link ASSERTs catch that.
#
# Override any of these on the command line. They reach the generated linker
# script, the C sources and the exception handler's link address from this one
# place, so they cannot drift apart -- which matters, because a literal that
# had drifted from the address the linker actually chose once rebooted the
# machine with no breadcrumb (AGENTS.md 14.11).
# ---------------------------------------------------------------------------
#
DCLOAD_BASE ?= 0x8c004000

# The BIOS VBR. Handed to the launched title as both VBR and SP, and the link
# address of exception.bin, for every base. Not a knob.
DCLOAD_GUEST_VBR = 0x8c00f400

DCLOAD_IS_LOW := $(shell [ $$(( $(DCLOAD_BASE) )) -lt $$(( 0x8c010000 )) ] && echo 1 || echo 0)
ifeq ($(DCLOAD_IS_LOW),1)
  DCLOAD_STACK ?= $(DCLOAD_GUEST_VBR)
  DCLOAD_HIRAM ?= 0x8cfe9000
  DCLOAD_MAPLE ?= 0x8cfe8000
else
  DCLOAD_STACK ?= $(shell printf '0x%08x' $$(( $(DCLOAD_BASE) + 0xb000 )))
  DCLOAD_HIRAM ?= $(shell printf '0x%08x' $$(( $(DCLOAD_BASE) + 0xc000 )))
  DCLOAD_MAPLE ?= $(shell printf '0x%08x' $$(( $(DCLOAD_BASE) + 0xd000 )))
endif

CFLAGS += -DDCLOAD_BASE=$(DCLOAD_BASE) -DDCLOAD_STACK=$(DCLOAD_STACK) \
          -DDCLOAD_GUEST_VBR=$(DCLOAD_GUEST_VBR) \
          -DMAPLE_DMA_BUFFER_ADDR=$(DCLOAD_MAPLE)

# Sample the guest's PC from a TMU0 interrupt handler installed in the vector
# table we hand the title (exception.S). The only instrument that keeps working
# when a title stops calling GD syscalls, which is the state that has resisted
# every other measurement here. GUEST_TICK_WATCHDOG ticks with no one servicing
# the network and the handler jumps back into the loader so the ring can be
# read; 0 disables that. The block address is fixed for every base on purpose.
#
# THE BLOCK LIVES AT 0x8cff8000, AND THAT ADDRESS IS THE INSTRUMENT.
#
# It was first at VBR + 0x700, inside exception.bin, which is inside the region
# 1st_read's bootstrap ZERO-FILLS (0x8c004000..0x8c010000). So the samples died
# the moment the console was reset and the loader re-booted from the CD -- i.e.
# exactly when you go to read them. 0x8cff8000 is above every loader placement
# for every base (a HIGH loader at 0x8cfe8000 spans to 0x8cff5800), above
# zero_game_ram()'s range, outside crt0's BSS and .hiram zeroing, and outside
# the bootstrap's zero-fill. It is also RAM no title touches: DreamShell's own
# highest `memory` value is 0x8cff4800, so everything from there up is loader
# territory by convention.
#
# That buys the recovery path that makes this usable when the title hangs the
# machine outright: RESET the console, let it boot dcload from the CD again,
# and read the block. The samples are still there.
#
GUEST_TICK ?= 0
GUEST_TICK_PERIOD ?= 125000
GUEST_TICK_WATCHDOG ?= 300
GUEST_TICK_BLOCK ?= 0x8cff8000
CFLAGS += -DGUEST_TICK=$(GUEST_TICK) -DGUEST_TICK_BLOCK=$(GUEST_TICK_BLOCK) \
          -DGUEST_TICK_PERIOD=$(GUEST_TICK_PERIOD) -DGUEST_TICK_WATCHDOG=$(GUEST_TICK_WATCHDOG)

# Hand a title the machine state DreamShell's isoldr hands it, read off
# kos/ds/firmware/isoldr/loader/{startup.s,utils.c}: SR = 0x700000f0 (NOT the
# 0x60000101 this tree's go.S used to attribute to it -- that value is isoldr's
# NAOMI path only), GBR = 0x8c000000, SSR/SPC set, FPSCR = 0x00040001, and
# setup_machine() before the jump. See the headers on go.S and setup_machine().
#
# Split three ways, because it is three independent changes and the first
# regression it produced needed bisecting. ISOLDR_HANDOFF=1 still turns on all
# three; set any of them individually to take one back out.
#
#   ISOLDR_SR             SR = 0x700000f0 instead of 0x60000101. The big one:
#                         it sets BL=1, so the title can take neither an
#                         interrupt nor an exception until it clears it.
#   ISOLDR_REGS           GBR = 0x8c000000, SSR, SPC, FPSCR = 0x00040001.
#   ISOLDR_SETUP_MACHINE  setup_machine() before the jump: TMU restarted, ASIC
#                         masks cleared and pending acknowledged, GD interrupt
#                         cleared.
#
# MEASURED 2026-08-16, on a console: of the three "all but one" builds, EVERY
# ONE failed to launch Sonic Adventure -- which stock launches fine. The only
# setting all three share is ISOLDR_REGS, so that is what breaks a title, and
# ISOLDR_SR / ISOLDR_SETUP_MACHINE / GUEST_CACHES_ON were never actually judged:
# each had only ever been tried alongside the one thing that was already fatal.
# Do not re-derive this from a bundle; change one at a time.
#
ISOLDR_HANDOFF ?= 0
ISOLDR_SR ?= $(ISOLDR_HANDOFF)
ISOLDR_REGS ?= $(ISOLDR_HANDOFF)
ISOLDR_SETUP_MACHINE ?= $(ISOLDR_HANDOFF)
CFLAGS += -DISOLDR_SR=$(ISOLDR_SR) -DISOLDR_REGS=$(ISOLDR_REGS) \
          -DISOLDR_SETUP_MACHINE=$(ISOLDR_SETUP_MACHINE)

# Hand the title CCR = 0x0909 (both caches on), which is what isoldr does,
# instead of 0x0808. go.S argues for off; that argument assumes dcload's own
# stores are cached, and they are not -- the cache has been off since 1st_read.
# A title running entirely uncached on real hardware is many times slower, and
# nothing about that is visible under an emulator.
GUEST_CACHES_ON ?= 0
CFLAGS += -DGUEST_CACHES_ON=$(GUEST_CACHES_ON)

# Clear the IP.BIN region (0x8c008000..DCLOAD_GUEST_VBR) at loader startup, so a
# title that reads structures a real bootstrap would have built there finds them
# empty instead of finding the previous session's debris. NOT a fix for anything
# known: it was tried against Sonic Adventure 2 on hardware and changed nothing,
# as did loading the real header sector over it. Faithfulness only -- read the
# header on clear_ipbin_region() before spending a console run on it.
DCLOAD_CLEAR_IPBIN ?= 0
CFLAGS += -DDCLOAD_CLEAR_IPBIN=$(DCLOAD_CLEAR_IPBIN)

# Clear the RAM a title is about to be uploaded into, at loader startup. A real
# Dreamcast boots through IP.BIN, which clears memory first; we upload over
# whatever the last session left and jump. Emulated RAM starts zeroed, so the
# difference is invisible there. See zero_game_ram() in dcload.c.
DCLOAD_ZERO_GAME_RAM ?= 0
CFLAGS += -DDCLOAD_ZERO_GAME_RAM=$(DCLOAD_ZERO_GAME_RAM)

# Service the network from ReqCmd/GetCmdStat/GetDrvStat, not just from the read
# path. A DIAGNOSTIC: without it, a title that busy-waits on a GD query without
# ever completing a read leaves the loader running and unreachable, which looks
# exactly like a dead loader. See the comment above gd_service_net().
GD_SERVICE_EVERY_SYSCALL ?= 0
CFLAGS += -DGD_SERVICE_EVERY_SYSCALL=$(GD_SERVICE_EVERY_SYSCALL)

# Hand the launched title SR with IMASK=15 instead of IMASK=0. A DIAGNOSTIC:
# the vector table we hand a title acknowledges nothing, so an interrupt still
# enabled from the BIOS livelocks it before it runs. go.S' header has the whole
# argument and what the real fix would be. Never ship this at 1.
GUEST_IRQ_MASKED ?= 0
CFLAGS += -DGUEST_IRQ_MASKED=$(GUEST_IRQ_MASKED)

# Trace the GD request/answer contract to the host console (dcload-ip-rs shows
# it). Off by default: each traced event is a full UDP round trip, so it does
# perturb timing. See cdfs_syscalls.c.
GD_TRACE ?= 0
CFLAGS += -DGD_TRACE=$(GD_TRACE)

#
# ---------------------------------------------------------------------------
# IMAGE SIZE
#
# dcload's footprint is a correctness property, not a nicety. The loader lives
# at 0x8c004000, in the hole between the BIOS syscall area and 1ST_READ.BIN,
# and a retail title treats that hole as free memory: Sonic Adventure enters
# the GD syscalls with SP = 0x8c00b9d0 and grows DOWN. The distance between
# _end and that SP is the entire margin a title's stack has before it starts
# writing over the loader -- and once it reaches `bb`, the adapter pointer,
# dcload's next bb->loop() is an indirect call through garbage. AGENTS.md 4.6
# has the full account.
#
# So the knobs below all buy the same thing: distance under a title's stack.
# After any change here, check _end in dcload.map. There is no other test
# that tells you whether a title you have not tried yet will fit.
# ---------------------------------------------------------------------------
#

# Let the linker drop functions and data nothing references. No behavioural
# change and no cost; the only symbols it removes today are genuinely dead
# ones (memmove, memcmp, PMCR_RegRead, a shadowed loop_secs_elapsed global).
# Worth 160 bytes today, and it is also what tells you something went dead.
DCLOAD_GC_SECTIONS ?= 1
ifeq ($(DCLOAD_GC_SECTIONS),1)
  CFLAGS += -ffunction-sections -fdata-sections -Wl,--gc-sections
endif

# Link-time optimisation. Worth 1856 bytes, which is not nothing when the
# stock margin is barely 1 KB -- but it is OFF BY DEFAULT on purpose:
#
#  - It inlines across translation units, which changes the depth of the C
#    frame parked by the GD coroutine (cdfs_redir.s copies it into a 96-long
#    buffer). If you turn this on, read g_gd_park_longs on a real boot before
#    trusting the build; anything approaching 80 means the buffer is too
#    small. See AGENTS.md 4.5.
#  - It suppresses the per-source *.asm listings (lto-wrapper drops the
#    -Wa options), so the artefacts AGENTS.md 6 describes stop appearing.
#
# Neither of those can be checked at build time, which is why this is a knob
# and not the default.
DCLOAD_LTO ?= 0
ifeq ($(DCLOAD_LTO),1)
  CFLAGS += -flto
endif

# Put the two 1536-byte packet buffers in high RAM (a NOLOAD section in
# dcload.x) instead of in BSS. Worth 3088 bytes of _end, more than every
# other knob here put together. See hiram.h for the trade this makes.
PKT_BUFS_IN_HIRAM ?= 1
CFLAGS += -DPKT_BUFS_IN_HIRAM=$(PKT_BUFS_IN_HIRAM)

# Build the HIT-0300 LAN Adapter driver. Worth 2040 bytes. Set to 0 if you
# only ever use a BBA (HIT-0400): adapter_detect() then only probes the BBA
# and a LAN Adapter is not detected at all.
WITH_LAN_ADAPTER ?= 1
CFLAGS += -DWITH_LAN_ADAPTER=$(WITH_LAN_ADAPTER)

# Serve the MAPL command (host-to-DC Maple passthrough) and the driver behind
# it. Worth 640 bytes. A session that only boots a title never uses it;
# with this at 0 a MAPL packet is simply not answered.
WITH_MAPLE ?= 1
CFLAGS += -DWITH_MAPLE=$(WITH_MAPLE)

# Serve the PMCR command (host-driven SH4 performance counters). Worth 808
# bytes. This does NOT remove perfctr.c: dcload uses counter 1 itself for
# the DHCP lease and for the adapter loops' timeouts. Only the host-facing
# command goes away.
WITH_PMCR_CMD ?= 1
CFLAGS += -DWITH_PMCR_CMD=$(WITH_PMCR_CMD)

OBJCOPY	= $(TARGETOBJCOPY)

DCLOBJECTS	= dcload-crt0.o disable.o startup_support.o go.o video.o memcpy.o memcmp.o memfuncs.o packet.o net.o adapter.o rtl8139.o dhcp.o dcload.o perfctr.o cdfs_redir.o cdfs_syscalls.o syscalls.o commands.o
ifeq ($(WITH_LAN_ADAPTER),1)
  DCLOBJECTS += lan_adapter.o
endif
ifeq ($(WITH_MAPLE),1)
  DCLOBJECTS += maple.o
endif
EXCOBJECTS	= exception.o

# The per-source assembly listings (AGENTS.md 6). LTO discards these options
# anyway, with a warning per file, so do not ask for them in that case.
ifeq ($(DCLOAD_LTO),1)
  LISTING =
else
  LISTING = -Wa,-adghlmns=$*.asm
endif

%.o : %.c Makefile ../../Makefile.cfg
	$(CC) $(CFLAGS) $(INCLUDE) $(LISTING) -o $@ -c $<
%.o : %.s Makefile ../../Makefile.cfg
	$(CC) $(CFLAGS) $(INCLUDE) $(LISTING) -o $@ -c $<
%.o : %.S Makefile ../../Makefile.cfg
	$(CC) $(CFLAGS) $(INCLUDE) $(LISTING) -o $@ -c $<
%.bin: %
	$(OBJCOPY) -O binary $< $@

all: dcload.bin exception.bin

# The linker script is GENERATED, because ld gives no way to override a MEMORY
# region's ORIGIN from the command line -- --defsym reaches symbols only, and
# ORIGIN is what has to move. dcload.x.in is the source; dcload.x is build
# output and is gitignored. Run through the C preprocessor with -P (no line
# markers, which ld would reject).
dcload.x: dcload.x.in Makefile ../../Makefile.cfg
	$(CC) -E -P -x c -DDCLOAD_BASE=$(DCLOAD_BASE) -DDCLOAD_STACK=$(DCLOAD_STACK) \
	      -DDCLOAD_HIRAM=$(DCLOAD_HIRAM) $< -o $@

dcload: $(DCLOBJECTS) dcload.x
	$(CC) $(CFLAGS) -Wl,--warn-common -Wl,--no-undefined -Wl,-Map,dcload.map -Wl,-Tdcload.x -nostartfiles -nostdlib -static -Wl,-z,now $(DCLOBJECTS) -o $@ -lgcc

# Linked at the BIOS VBR, for every base: this image IS the vector table the
# launched title is handed (go.s), so unlike the loader it does not move.
exception: $(EXCOBJECTS)
	$(CC) $(CFLAGS) -Wl,-Ttext=$(DCLOAD_GUEST_VBR) -Wl,-Map,exception.map -nostartfiles -nostdlib -static -Wl,-z,now $^ -o $@

#
# The per-game loader set the host chainloads into: one ELF per base address.
#
# Each ELF is SELF-CONTAINED. exception.bin is folded in as an extra allocated
# section at the guest VBR, so uploading the ELF section by section -- which is
# what the host already does -- puts the vector table there too. Without that
# the host would have to know about a second artefact and its address, and a
# HIGH loader would hand its title a VBR pointing at whatever was left in low
# RAM.
#
# Built one at a time in this directory rather than in parallel scratch trees:
# DCLOAD_BASE reaches the C sources as a -D, so objects from one base must
# never be linked into another, and `make clean` between links is the only
# thing that guarantees it. The default build is restored at the end.
#
# objcopy warns "allocated section `.guestvbr' not in segment" on every one of
# these, and that is expected: --add-section adds a SECTION, and no program
# header covers it. Harmless here because the host walks the section table
# (dcload-ip-rs, loaders::is_uploadable) rather than the program headers. A
# loader that walked PT_LOAD segments instead would silently skip the guest
# vector table, which is worth knowing before writing one.
#
# 0x8c000100 and 0x8c001100 are NOT here, and they are 276 of the 1026 presets.
# They link and boot, but an image based there overwrites the BIOS syscall area
# (0x8c000000-0x8c003fff -- which is precisely why the stock base is 0x8c004000
# and not lower), and this loader depends on it: video.s' _get_font_address
# reads the pointer at 0x8c0000b4 and JUMPS THROUGH IT, once per string drawn,
# and cmd_loadbin draws "receiving data..." during the very upload that is
# destroying the code that pointer names.
#
# Measured, chainloading 0x8ce00000 -> 0x8c000100: the upload dies partway with
# "No DoneBinary response received", and the running loader is then executing in
# the empty 4 KB above its own _stack with PR pointing inside raw_current_pkt --
# the signature of a jump through a clobbered function pointer.
#
# Supporting them means giving up the BIOS font, i.e. the on-screen display, for
# those bases (isoldr's `low`/`syscalls` option is its answer to the same
# problem). That is a design decision, not an oversight -- until it is made,
# building these would only produce a loader that bricks the session.
LOADER_BASES = 0x8c004000 0x8ce00000 0x8cef8000 0x8cfe8000
LOADER_DIR   = loaders

.PHONY : loaders
loaders:
	@# Purge first. Without this, an ELF for a base that has since been REMOVED
	@# from LOADER_BASES stays on disk and keeps being deployed and chainloaded
	@# -- which is how a base that was withdrawn for bricking the session goes
	@# on bricking it.
	@rm -rf $(LOADER_DIR)
	@mkdir -p $(LOADER_DIR)
	@for base in $(LOADER_BASES); do \
	  echo "[loaders] linking dcload for $$base"; \
	  $(MAKE) --no-print-directory clean >/dev/null || exit 1; \
	  $(MAKE) --no-print-directory DCLOAD_BASE=$$base dcload exception.bin >/dev/null || exit 1; \
	  $(OBJCOPY) --add-section .guestvbr=exception.bin \
	             --set-section-flags .guestvbr=alloc,load,contents,readonly \
	             --change-section-address .guestvbr=$(DCLOAD_GUEST_VBR) \
	             dcload $(LOADER_DIR)/dcload-$$base.elf || exit 1; \
	done
	@$(MAKE) --no-print-directory clean >/dev/null && $(MAKE) --no-print-directory all >/dev/null
	@echo "[loaders] built:" && ls -l $(LOADER_DIR)

# Where this build puts everything, for a human or a script that needs to know
# without reading the map. `make config DCLOAD_BASE=0x8cfe8000` answers the
# "did my override actually take" question that a -n dry run does not, since a
# dry run prints nothing at all when the tree is already up to date.
.PHONY : config
config:
	@echo "DCLOAD_BASE      = $(DCLOAD_BASE)   (image, and the ELF entry point)"
	@echo "DCLOAD_STACK     = $(DCLOAD_STACK)   (loader's own stack top)"
	@echo "DCLOAD_HIRAM     = $(DCLOAD_HIRAM)   (.hiram packet buffers)"
	@echo "DCLOAD_MAPLE     = $(DCLOAD_MAPLE)   (Maple DMA buffer)"
	@echo "DCLOAD_GUEST_VBR = $(DCLOAD_GUEST_VBR)   (VBR and SP handed to the title)"
	@echo "layout family    = $(if $(filter 1,$(DCLOAD_IS_LOW)),LOW,HIGH)"

# Single values, for another Makefile to ask rather than restate. ../1st_read
# needs the base, the guest VBR and the top of the region its bootstrap clears;
# a copy of those numbers over there would not fail the build when it went
# stale, it would zero-fill and jump to the wrong address.
#
# DCLOAD_ZERO_END: for a low base, the stock hole up to 1ST_READ.BIN, which is
# also where exception.bin ends. For a high base, the end of the loader's own
# span -- the bootstrap must not clear low RAM it does not own in that case,
# and the guest vector table is copied separately anyway.
.PHONY : print-base print-guest-vbr print-zero-end
print-base:
	@echo $(DCLOAD_BASE)
print-guest-vbr:
	@echo $(DCLOAD_GUEST_VBR)
print-zero-end:
	@echo $(if $(filter 1,$(DCLOAD_IS_LOW)),0x8c010000,$(shell printf '0x%08x' $$(( $(DCLOAD_BASE) + 0xe000 ))))

.PHONY : clean
clean:
	rm -f *.o dcload exception *.asm *.map dcload.x

.PHONY : distclean
distclean: clean
	rm -f dcload.bin exception.bin
	rm -rf $(LOADER_DIR)
