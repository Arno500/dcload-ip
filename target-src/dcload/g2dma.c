/*
 * g2dma.c -- waiting for the loader's own G2 DMA (g2dma.h). CD-DA only.
 */
#include "g2dma.h"

unsigned int g_g2dma_timeouts;		/* waits that gave up: the channel was aborted */

#if WITH_CDDA || G2DMA_BENCH || WITH_IRQ_HOOK

void g2dma_start(unsigned int ch, const void *ram, unsigned int g2,
		 unsigned int len, unsigned int dir)
{
	volatile unsigned int *r = &G2DMA_STAG(ch);

	G2DMA_G2APRO = G2DMA_G2APRO_ALL;
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
	}
	/* Their end bits, so that they never reach a title (write 1 to clear). */
	SB_ISTNRM = G2DMA_IST_BIT(G2DMA_CDDA_L) | G2DMA_IST_BIT(G2DMA_CDDA_R);
}

#endif
