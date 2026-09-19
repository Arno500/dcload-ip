/*
 * hiram.h -- placement of large buffers outside the loader image.
 *
 * WHY THIS EXISTS
 *
 * dcload lives at 0x8c004000, in the hole between the BIOS syscall area and
 * 1ST_READ.BIN. A retail title treats that hole as free memory and puts a
 * stack in it: Sonic Adventure enters the GD syscalls with SP = 0x8c00b9d0
 * and grows DOWN, straight towards dcload's BSS. The distance between _end
 * and that SP is the entire margin, and every byte of BSS spends it. When
 * _end was 0x8c00e7c0 the title's stack reached `bb`, the adapter pointer,
 * and dcload's next bb->loop() was an indirect call through garbage --
 * AGENTS.md 4.6 has the full account.
 *
 * So the rule from that investigation is: prefer putting large buffers
 * OUTSIDE the image over growing BSS. maple.c already does this by hand for
 * its DMA buffer (0x8cfe8000, 2 KB). This header generalises it.
 *
 * A variable marked HIRAM_BUF is emitted into the .hiram output section,
 * which dcload.x places at a fixed high-RAM address and marks NOLOAD, so it
 * costs nothing in dcload.bin and nothing in _end. dcload-crt0.s zeroes it at
 * start-up, as it does BSS. It holds the two 1536-byte packet buffers and,
 * with WITH_CDDA, the CD-DA staging buffer; 12 KB is reserved.
 *
 * THE TRADE
 *
 * This moves the buffers out of the region a title's stack roams and into
 * the region a title's allocator might claim. The first risk is measured
 * and has bitten this loader; the second has not been seen, and 0x8cfe8000 is
 * what isoldr's own free-high-RAM heuristic picks. If a title ever turns out
 * to own that RAM, build with PKT_BUFS_IN_HIRAM=0: the buffers go back into
 * BSS and _end climbs by ~5.4 KB (measured with the default options).
 */

#ifndef __HIRAM_H__
#define __HIRAM_H__

#ifndef PKT_BUFS_IN_HIRAM
#define PKT_BUFS_IN_HIRAM 1
#endif

#if PKT_BUFS_IN_HIRAM
#define HIRAM_BUF __attribute__((section(".hiram")))
#else
#define HIRAM_BUF
#endif

#endif /* __HIRAM_H__ */
