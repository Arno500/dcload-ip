include ../../Makefile.cfg

TARGET = 1st_read.bin

#
# WHERE THIS BOOTSTRAP COPIES THE LOADER TO.
#
# These must match the dcload build sitting in ../dcload, which is relinkable
# per game now (see the header of ../dcload/Makefile). They are asked FOR, not
# restated: a mismatch here would not fail the build, it would zero-fill and
# jump to the wrong place, which on a real console is a black screen with no
# way to tell why. Overriding DCLOAD_BASE for the CD image therefore means
# overriding it for both directories, and this reads it from the same place.
#
# GUEST_VBR is where exception.bin goes and is the same for every base.
# ZERO_END is the top of the region the bootstrap clears before copying: the
# stock hole up to 1ST_READ.BIN for a low base, the end of the loader's own
# span for a high one.
#
DCLOAD_BASE      ?= $(shell $(MAKE) --no-print-directory -C ../dcload -s print-base)
DCLOAD_GUEST_VBR ?= $(shell $(MAKE) --no-print-directory -C ../dcload -s print-guest-vbr)
DCLOAD_ZERO_END  ?= $(shell $(MAKE) --no-print-directory -C ../dcload -s print-zero-end)

LOADER_DEFS = -Wa,--defsym,DCLOAD_BASE=$(DCLOAD_BASE) \
              -Wa,--defsym,DCLOAD_GUEST_VBR=$(DCLOAD_GUEST_VBR) \
              -Wa,--defsym,DCLOAD_ZERO_END=$(DCLOAD_ZERO_END)

all: rm-elf $(TARGET)

.PHONY : asm_only
asm_only:
	$(TARGETCC) $(TARGETCFLAGS) -Wa,-I../dcload $(LOADER_DEFS) -Wa,-adghlmns=loader.asm -c loader.s

.PHONY : clean
clean:
	-rm -f $(TARGET) loader.bin loader.elf loader.o *.asm *.map

.PHONY : distclean
distclean: clean

.PHONY : rm-elf
rm-elf:
	-rm -f $(TARGET) loader.elf

loader.elf: loader.s disable.s ../dcload/dcload.bin ../dcload/exception.bin
	$(TARGETCC) $(TARGETCFLAGS) -o $@ loader.s disable.s -nostartfiles \
	    -nostdlib -Ttext=0x8c010000 -Wl,-Map=loader.map -Wa,-I../dcload $(LOADER_DEFS)

loader.bin: loader.elf
	$(TARGETOBJCOPY) -O binary $^ $@

# 1st_read.bin
$(TARGET): loader.bin
	$(KOS_BASE)/utils/scramble/scramble $< $@
