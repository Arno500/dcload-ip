CDRECORD	= wodim dev=0,0,0 speed=8
DD		= dd
CP		= cp
MKISOFS		= genisoimage
CAT		= cat
TOUCH		= touch
1ST_READ	= ../target-src/1st_read/1st_read.bin

# -B, NOT -e: 1st_read.bin has already been through KOS's `scramble`
# (target-src/1st_read), and `-e` wants an ELF -- it asserts on the magic and
# dies. `-d romdisk` named a directory that is not in this tree, which failed
# the same run for a second reason.
#
# MKDCDISC IS NOT IN $(TOOLINSTALLDIR) on every machine; it is built in its own
# checkout. Take it from PATH when it is there, and fall back to where the
# dc-chain layout puts it, so this does not silently stop working again.
MKDCDISC ?= $(shell command -v mkdcdisc || echo /opt/toolchains/dc/mkdcdisc/build/mkdcdisc)

dctoolip: 1st_read.bin
	$(MKDCDISC) -n $@ -B $< -N -o $@.cdi -v 3 -m


1st_read.bin: $(1ST_READ)
	$(CP) $(1ST_READ) 1st_read.bin


$(1ST_READ):
	cd ../target-src && make


.PHONY : clean
clean:
	rm -f dctoolip.cdi 1st_read.bin

