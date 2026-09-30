/*
 * g2dma.c -- waiting for the loader's own G2 DMA (g2dma.h). CD-DA only.
 */
#include "g2dma.h"

unsigned int g_g2dma_timeouts;		/* waits that gave up: the channel was aborted */

#if WITH_CDDA || G2DMA_BENCH || WITH_IRQ_HOOK

volatile unsigned int g2dma_mine;
volatile unsigned int g_g2dma_foreign = 1U;	/* channel 0: the title's sound */
static unsigned int g2_hold_depth;
static unsigned int g2_held;		/* the channels this hold suspended */

void g2dma_hold(void)
{
	unsigned int ch, spin = 200000U;

	if (g2_hold_depth++)
	{
		return;
	}
	g2_held = 0;
	for (ch = 0; ch < 4; ch++)
	{
		if (!((g2dma_mine >> ch) & 1U) || !g2dma_busy(ch))
		{
			G2DMA_SUSP(ch) = 1;
			g2_held |= 1U << ch;
		}
	}
	/* The SH4, G2 and AICA FIFOs empty, as KOS waits (~2 ms at most). */
	while ((*(volatile unsigned int *)0xa05f688cU & 0x31U) && --spin)
	{
	}
}

extern char _hiram_start[], _hiram_end[];

static void g2dma_observe(void)
{
	unsigned int ch, a;
	unsigned int lo = (unsigned int)_hiram_start & 0x1fffffffU;
	unsigned int hi = ((unsigned int)_hiram_end & 0x1fffffffU) + 0x2000U;

	for (ch = 1; ch < 4; ch++)
	{
		if (((g_g2dma_foreign | g2dma_mine) >> ch) & 1U)
		{
			continue;
		}
		a = G2DMA_STAR(ch) & 0x1fffffe0U;
		if (g2dma_busy(ch) || (SB_ISTNRM & G2DMA_IST_BIT(ch))
		    || (a && (a < lo || a >= hi)))
		{
			g_g2dma_foreign |= 1U << ch;
		}
	}
}

int g2dma_pick(void)
{
	unsigned int ch;

	g2dma_observe();
	for (ch = 3; ch > 0; ch--)
	{
		if (!(((g_g2dma_foreign | g2dma_mine) >> ch) & 1U) && !g2dma_busy(ch))
		{
			return (int)ch;
		}
	}
	return -1;
}

void g2dma_forget(void)
{
	unsigned int ch;

	for (ch = 1; ch < 4; ch++)
	{
		if (!g2dma_busy(ch))
		{
			G2DMA_STAR(ch) = 0;
		}
	}
	g_g2dma_foreign = 1U;
}

void g2dma_release(void)
{
	unsigned int ch;

	if (!g2_hold_depth || --g2_hold_depth)
	{
		return;
	}
	for (ch = 0; ch < 4; ch++)
	{
		if ((g2_held >> ch) & 1U)
		{
			G2DMA_SUSP(ch) = 0;
		}
	}
}

void g2dma_start(unsigned int ch, const void *ram, unsigned int g2,
		 unsigned int len, unsigned int dir)
{
	volatile unsigned int *r = &G2DMA_STAG(ch);

	G2DMA_G2APRO = G2DMA_G2APRO_ALL;
	r[7] = 0;				/* SUSP: ours now, whatever a hold did */
	g2dma_mine |= 1U << ch;
	r[5] = 0;				/* EN */
	r[0] = g2 & 0x1fffffe0U;
	r[1] = G2DMA_PHYS(ram);
	r[2] = ((len + 31U) & ~31U) | 0x80000000U;
	r[3] = dir;
	r[4] = 4U;				/* TSEL: CPU trigger, suspend honoured */
	r[5] = 1;
	r[6] = 1;				/* ST */
}

#endif

#if WITH_CDDA

void g2dma_quiesce(void)
{
	unsigned int ch, spin;

	for (ch = G2DMA_CDDA_L; ch <= G2DMA_CDDA_R; ch++)
	{
		if (!((g2dma_mine >> ch) & 1U))
		{
			continue;	/* not ours: a title's transfer, or none */
		}
		/* Bounded on a count, like every wait on G2 (~2 ms). */
		for (spin = 200000U; g2dma_busy(ch); )
		{
			if (!--spin)
			{
				G2DMA_EN(ch) = 0;
				g_g2dma_timeouts++;
				break;
			}
		}
		/* Its end bit, so that it never reaches a title (write 1 to clear). */
		SB_ISTNRM = G2DMA_IST_BIT(ch);
		g2dma_mine &= ~(1U << ch);
	}
}

#endif
