/*
 * g2dma.h -- the four G2 bus DMA channels, driven by polling.
 *
 * WHY. Everything the loader moves over G2 it used to move with the CPU: the
 * BBA's RX ring read 32 bits at a time (~10 MB/s, ~140 us a frame, the whole
 * of the tick's cost under an asynchronous read, docs/wince-investigation.md
 * 9r) and the CD-DA ring written the same way (6.9 MB/s). A G2 DMA moves the
 * same bytes with the CPU free. docs/g2-dma-investigation.md has the plan and
 * the measurements.
 *
 * THE REGISTERS (KOS hardware/g2dma.c, DreamShell drivers/aica.h), one block
 * of 0x20 bytes per channel from 0xa05f7800: G2 address, system RAM address
 * (both physical, 32-byte aligned), length (a multiple of 32; bit 31 clears
 * the enable at the end), direction (1 = G2 to RAM), trigger select (0 = CPU;
 * KOS writes 1, "hardware", for the AICA's channel 0; | 4 lets the suspend
 * register pause it), enable, start (reads 1 while running), suspend.
 * Channel 0 is the AICA's -- the title's sound driver uses it -- 1 the one
 * KOS gives the BBA, 2 and 3 are free. The end of a transfer sets bit 15 + ch
 * of Holly's normal interrupt status; nothing here takes that interrupt, and
 * g2dma_wait() clears the bit so it never reaches a title.
 *
 * UNDER FLYCAST A DMA COPIES EVERYTHING THE MOMENT IT STARTS (aica_if.cpp);
 * only the end interrupt and the start bit are deferred. A missing wait is
 * therefore invisible there: the console is the only judge of ordering.
 */
#ifndef __G2DMA_H__
#define __G2DMA_H__

#include "adapter.h"	/* clk_now() */

#ifndef WITH_CDDA
#define WITH_CDDA 1
#endif
#ifndef G2DMA_BENCH
#define G2DMA_BENCH 0
#endif
#ifndef WITH_IRQ_HOOK
#define WITH_IRQ_HOOK 1
#endif

#define G2DMA_REG(ch, off) (*(volatile unsigned int *)(0xa05f7800U + 0x20U * (ch) + (off)))
#define G2DMA_STAG(ch)     G2DMA_REG(ch, 0x00)
#define G2DMA_STAR(ch)     G2DMA_REG(ch, 0x04)
#define G2DMA_LEN(ch)      G2DMA_REG(ch, 0x08)
#define G2DMA_DIR(ch)      G2DMA_REG(ch, 0x0c)
#define G2DMA_TSEL(ch)     G2DMA_REG(ch, 0x10)
#define G2DMA_EN(ch)       G2DMA_REG(ch, 0x14)
#define G2DMA_ST(ch)       G2DMA_REG(ch, 0x18)
#define G2DMA_SUSP(ch)     G2DMA_REG(ch, 0x1c)

/* System RAM a G2 DMA may reach: the whole 16 MB (KOS, isoldr). Write-only. */
#define G2DMA_G2APRO       (*(volatile unsigned int *)0xa05f78bcU)
#define G2DMA_G2APRO_ALL   0x4659007fU

/* Holly's interrupt status, normal and error; write 1 to clear. */
#define SB_ISTNRM          (*(volatile unsigned int *)0xa05f6900U)
#define SB_ISTERR          (*(volatile unsigned int *)0xa05f6908U)
#define G2DMA_IST_BIT(ch)  (1U << (15U + (ch)))

#define G2DMA_TO_G2        0U
#define G2DMA_TO_RAM       1U

/* The longest any wait here spins: 2 ms (Pck/4 ticks, clk_since()). 16 KB at the
 * slowest G2 rate measured (6.9 MB/s) is 2.4 ms, and nothing here moves more
 * than a frame or a sub-fetch at a time. */
#define G2DMA_WAIT_TICKS   25000U

#define G2DMA_PHYS(p)      (((unsigned int)(p)) & 0x1fffffe0U)

/* Start a transfer of `len` bytes (rounded up to 32) between RAM `ram` and G2
 * address `g2` (g2dma.c: out of line, the register sequence is long). The
 * caller has made the cache agree with RAM already. */
void g2dma_start(unsigned int ch, const void *ram, unsigned int g2,
		 unsigned int len, unsigned int dir);

static inline int g2dma_busy(unsigned int ch)
{
	return (G2DMA_ST(ch) & 1U) != 0U;
}

/* Channels with a transfer of the loader's started and not yet collected: the
 * only ones whose end bit the loader may clear, and the ones a hold leaves
 * running. A title may drive any of the four (g2dma_hold()). */
extern volatile unsigned int g2dma_mine;

/*
 * THE TITLE'S G2 DMA IS SUSPENDED WHILE THE LOADER USES THE BUS FROM UNDER IT,
 * as KOS's g2_lock() does around every G2 access. Measured 2026-09-30 on Sonic
 * Adventure 2: with the disc reads moved into the interrupt hook, the tick read
 * the BBA's ring while the title's sound driver was moving its banks to the
 * AICA, and the title waited forever after loading them -- on the console
 * only, flycast does not model the bus. Suspending every channel for the tick
 * fixed it. Nested; channels of the loader's own that are running are left
 * alone (a wait on them would never end), and g2dma_start() lifts the suspend
 * of a channel it takes. Bounded like every wait on G2.
 */
void g2dma_hold(void);
void g2dma_release(void);

/*
 * WHICH CHANNELS THE TITLE USES, so that the loader's own short transfers (the
 * RX DMA) never share one. Sharing is not safe even when the title's channel
 * is idle: a Katana title arms the end bits of all four channels on its IML4
 * (0x7f000), and its handler takes the end of a transfer of ours on a channel
 * it drives for the end of its own. Crazy Taxi 2 does not use channel 1 and
 * ignores ours; Sonic Adventure 2 does, and went to a black screen before its
 * menu for every build that received frames by DMA there (2026-10-01).
 *
 * A channel is the title's once seen busy or with its end bit up while
 * nothing of ours is on it, or with a RAM address (STAR) outside the loader's
 * .hiram -- the trace a finished transfer of the title's leaves, which the
 * first two tests miss (its transfers are short, its handler clears the bit
 * at once). g2dma_forget() clears the traces of an earlier loader at EXEC.
 * Channel 0 is always the title's (the AICA's). Counter: g_g2dma_foreign.
 */
extern volatile unsigned int g_g2dma_foreign;
void g2dma_forget(void);
/* A channel for a short transfer of the loader's: 3, then 2, then 1, of those
 * neither the title's nor busy; -1 if none (then the CPU does it). */
int g2dma_pick(void);

/* Wait for the channel to finish, at most G2DMA_WAIT_TICKS. 0 when done (its
 * end bit cleared), -1 if it is still running. */
static inline int g2dma_wait(unsigned int ch)
{
	unsigned int t0 = clk_now();

	while (g2dma_busy(ch))
	{
		if (clk_since(t0) > G2DMA_WAIT_TICKS)
		{
			return -1;
		}
	}
	SB_ISTNRM = G2DMA_IST_BIT(ch);
	g2dma_mine &= ~(1U << ch);
	return 0;
}

/*
 * THE LOADER'S OWN DMA: CD-DA's two ring writes, on channels 2 (left) and 3
 * (right); 0 is the title's sound driver and 1 the BBA's (KOS). Rule: no
 * loader access to the G2 bus by the CPU while one of them runs. Every place
 * that does one -- the adapter drivers, the G2 lock, the CD-DA staging buffer
 * the DMA reads -- calls g2dma_quiesce() first: a bounded wait, which aborts
 * a channel that will not finish (g_g2dma_timeouts) instead of hanging.
 */
#define G2DMA_CDDA_L 2U
#define G2DMA_CDDA_R 3U

extern unsigned int g_g2dma_timeouts;

#if WITH_CDDA
void g2dma_quiesce(void);
#else
#define g2dma_quiesce() ((void)0)
#endif

#if G2DMA_BENCH
void g2dma_bench(void);
#endif

#endif /* __G2DMA_H__ */
