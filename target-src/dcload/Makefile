include ../../Makefile.cfg

CC	= $(TARGETCC)
CFLAGS	= $(TARGETCFLAGS) -DDCLOAD_VERSION=\"$(VERSION)\" -DDREAMCAST_IP=\"$(DREAMCAST_IP)\" -DEXCEPTION_SECONDS=$(EXCEPTION_SECONDS) -Wall -Wextra -ffreestanding -fno-zero-initialized-in-bss -fno-common -fomit-frame-pointer -fno-strict-aliasing -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -fno-delete-null-pointer-checks -fno-stack-protector -fno-stack-check -fno-merge-constants -fno-merge-all-constants -std=gnu11
INCLUDE	= -I../../target-inc

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

dcload: $(DCLOBJECTS)
	$(CC) $(CFLAGS) -Wl,--warn-common -Wl,--no-undefined -Wl,-Map,dcload.map -Wl,-Tdcload.x -nostartfiles -nostdlib -static -Wl,-z,now $^ -o $@ -lgcc

exception: $(EXCOBJECTS)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x8c00f400 -Wl,-Map,exception.map -nostartfiles -nostdlib -static -Wl,-z,now $^ -o $@

.PHONY : clean
clean:
	rm -f *.o dcload exception *.asm *.map

.PHONY : distclean
distclean: clean
	rm -f dcload.bin exception.bin
