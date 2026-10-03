/*
 * g2bench.c -- what G2 DMA does on THIS console, measured before it is used
 * (G2DMA_BENCH=1; docs/g2-dma-investigation.md, step 0).
 *
 * Runs once at boot, before the main loop, and answers the questions the rest
 * of the DMA work depends on, each against a CPU read of the same bytes:
 *
 *   RX    the BBA's SRAM read into RAM three ways -- the CPU through the GAPS
 *         window (what pktcpy() does), DMA channel 1 through the window, DMA
 *         channel 1 at the SRAM's own address (what KOS does);
 *   AICA  sound RAM written by the CPU (what cdda_push() does) and by DMA on
 *         each of the four channels: which of them reach the AICA at all;
 *   TORTURE  a DMA on each side in flight while the CPU writes and reads the
 *         AICA as a title's sound driver would. Whether that is safe decides
 *         whether a DMA may be left running under a title (G2DMA_CONCURRENT).
 *
 * Every figure is in g_bench[] (below) and on screen. Times are clk_since()
 * ticks (Pck/4, 12.5 per microsecond) per transfer: 1536 bytes for RX, 2368 for the
 * AICA. A positive control proves the checker counts a wrong word.
 *
 * It uses what is idle at boot: gd_stage (.hiram, a disc read's stage), the
 * BBA's SRAM between the RX ring's spill (0x4600) and the TX buffers
 * (0x6000), clear of the warm-start handoff at 0x5000, and sound RAM inside
 * the CD-DA rings' area, which nothing uses before a title plays music.
 */

#include "dcload.h"
#include "video.h"
#include "memfuncs.h"
#include "adapter.h"
#include "g2dma.h"

#if G2DMA_BENCH

/* Two transfer buffers: the Maple DMA page, 4 KB, aligned to 32 and idle at
 * boot (a MAPL command is what uses it; none has arrived yet). .hiram has no
 * room to spare. */
extern char maple_dma_buffer[];
#define gd_stage ((unsigned int *)maple_dma_buffer)

/*
 * g_bench[] -- read with dc-counters.py --raw, or off the screen, one line per
 * group of four:
 *  0.. 3  RX ticks: CPU, DMA window, DMA direct, DMA timeouts (window+direct)
 *  4.. 7  RX wrong words: CPU, DMA window, DMA direct; AICA reads that came
 *         back 0 and were read again (AGENTS.md 4.13 rule 8)
 *  8..11  AICA DMA ticks, channels 0..3
 * 12..15  AICA DMA wrong words, channels 0..3
 * 16..19  AICA CPU ticks, AICA CPU wrong words, channels that timed out
 *         (bit ch), Holly error status seen (OR)
 * 20..23  torture: rounds, rounds that began with a DMA still running,
 *         CPU wrong words (sound RAM + register), DMA wrong words
 * 24..27  torture: AICA channel used, RX source (1 window, 2 direct),
 *         timeouts, positive control (must be 1)
 * 28..31  end bits seen in Holly's normal status (>> 15), channels busy
 *         before the bench (bit ch), rounds with a DMA still running when the
 *         CPU was done, 'G2BN' when finished
 */
unsigned int g_bench[32];

#define GAPS_BENCH_OFF  0x5400U
#define GAPS_SRAM(off)  ((volatile unsigned int *)(0xa1840000U + (off)))
#define GAPS_WINPTR     (*(volatile unsigned int *)0xa100142cU)
#define GAPS_WIN_P1     0x81848000U
#define GAPS_WIN_G2     0x01848000U
#define GAPS_SRAM_G2    0x01840000U

#define AICA_A          0x148000U	/* DMA target */
#define AICA_B          0x14c000U	/* the "title's" CPU target */
#define AICA_RAM(off)   ((volatile unsigned int *)(0xa0800000U + (off)))
#define AICA_CH60_PITCH (*(volatile unsigned int *)(0xa0700000U + 0x80U * 60U + 24U))
#define G2_FIFO         (*(volatile unsigned int *)0xa05f688cU)

#define RX_LEN   1536U
#define AI_LEN   2368U
#define RX_RUNS  64U
#define AI_RUNS  16U
#define ROUNDS   256U

#define P2(p) ((volatile unsigned int *)(((unsigned int)(p) & 0x1fffffffU) | 0xa0000000U))

/* Never zero (bit 0 is set), so a zero read is always a failed read. */
static unsigned int pat(unsigned int i, unsigned int seed)
{
	return ((i + seed) * 0x9e3779b1U) | 0x00010001U;
}

static void fifo_wait(void)
{
	unsigned int spin = 200000U;

	while ((G2_FIFO & 0x31U) && --spin)
	{
	}
}

static unsigned int aica_rd(volatile unsigned int *p)
{
	unsigned int v = *p;
	unsigned int n = 2;

	while (!v && n--)
	{
		g_bench[7]++;
		v = *p;
	}
	return v;
}

static void ram_fill(void *p, unsigned int words, unsigned int seed)
{
	volatile unsigned int *q = P2(p);
	unsigned int i;

	for (i = 0; i < words; i++)
	{
		q[i] = seed ? pat(i, seed) : 0xdeadbeefU;
	}
}

static unsigned int ram_bad(const void *p, unsigned int words, unsigned int seed)
{
	volatile unsigned int *q = P2(p);
	unsigned int i, bad = 0;

	for (i = 0; i < words; i++)
	{
		bad += (q[i] != pat(i, seed));
	}
	return bad;
}

static void aica_fill(unsigned int off, unsigned int words, unsigned int seed)
{
	volatile unsigned int *q = AICA_RAM(off);
	unsigned int i;

	for (i = 0; i < words; i++)
	{
		if ((i & 7U) == 0U)
		{
			fifo_wait();
		}
		q[i] = seed ? pat(i, seed) : 0xdeadbeefU;
	}
}

static unsigned int aica_bad(unsigned int off, unsigned int words, unsigned int seed)
{
	volatile unsigned int *q = AICA_RAM(off);
	unsigned int i, bad = 0;

	fifo_wait();
	for (i = 0; i < words; i++)
	{
		bad += (aica_rd(q + i) != pat(i, seed));
	}
	return bad;
}

/* g2dma_wait(), noting the end bit before it is cleared. */
static int bench_wait(unsigned int ch)
{
	unsigned int t0 = clk_now();

	while (g2dma_busy(ch))
	{
		if (clk_since(t0) > G2DMA_WAIT_TICKS)
		{
			return -1;
		}
	}
	g_bench[28] |= (SB_ISTNRM >> 15) & 0xfU;
	SB_ISTNRM = G2DMA_IST_BIT(ch);
	return 0;
}

/* One RX transfer: 0 CPU, 1 DMA window, 2 DMA direct. Ticks, or ~0 on a
 * timeout. */
static unsigned int rx_once(unsigned int how, unsigned char *buf)
{
	unsigned int t0, t;

	ram_fill(buf, RX_LEN / 4U, 0);
	CacheBlockInvalidate(buf, RX_LEN / 32U);
	while (G2_FIFO & 0x20U)
	{
	}
	t0 = clk_now();
	if (how == 0U)
	{
		GAPS_WINPTR = 0x81840000U + GAPS_BENCH_OFF;
		memcpy_32bit(buf, (unsigned char *)GAPS_WIN_P1, RX_LEN / 4U);
		CacheBlockInvalidate((unsigned char *)GAPS_WIN_P1, RX_LEN / 32U);
		CacheBlockWriteBack(buf, RX_LEN / 32U);
	}
	else
	{
		if (how == 1U)
		{
			GAPS_WINPTR = 0x81840000U + GAPS_BENCH_OFF;
		}
		g2dma_start(1, buf, how == 1U ? GAPS_WIN_G2 : GAPS_SRAM_G2 + GAPS_BENCH_OFF,
			    RX_LEN, G2DMA_TO_RAM);
		if (bench_wait(1) < 0)
		{
			g_bench[3]++;
			return ~0U;
		}
	}
	t = clk_since(t0);
	g_bench[4 + how] += ram_bad(buf, RX_LEN / 4U, 1);
	return t;
}

static void bench_rx(void)
{
	unsigned char *buf = (unsigned char *)gd_stage;
	volatile unsigned int *s = GAPS_SRAM(GAPS_BENCH_OFF);
	unsigned int how, k, i, sum;

	for (i = 0; i < RX_LEN / 4U; i++)
	{
		if ((i & 7U) == 0U)
		{
			fifo_wait();
		}
		s[i] = pat(i, 1);
	}
	for (how = 0; how < 3U; how++)
	{
		for (k = 0, sum = 0; k < RX_RUNS; k++)
		{
			unsigned int t = rx_once(how, buf);

			if (t == ~0U)
			{
				break;
			}
			sum += t;
		}
		g_bench[how] = k ? sum / k : ~0U;
	}
}

static void bench_aica(void)
{
	unsigned char *src = (unsigned char *)gd_stage + 2048U;
	unsigned int ch, k, t0, sum;

	for (k = 0, sum = 0; k < AI_RUNS; k++)
	{
		aica_fill(AICA_A, AI_LEN / 4U, 0);
		t0 = clk_now();
		aica_fill(AICA_A, AI_LEN / 4U, 0x77U);
		sum += clk_since(t0);
		g_bench[17] += aica_bad(AICA_A, AI_LEN / 4U, 0x77U);
	}
	g_bench[16] = sum / AI_RUNS;

	for (ch = 0; ch < 4U; ch++)
	{
		ram_fill(src, AI_LEN / 4U, 0x100U + ch);
		for (k = 0, sum = 0; k < AI_RUNS; k++)
		{
			aica_fill(AICA_A, AI_LEN / 4U, 0);
			SB_ISTERR = SB_ISTERR;
			fifo_wait();
			t0 = clk_now();
			g2dma_start(ch, src, 0x00800000U + AICA_A, AI_LEN, G2DMA_TO_G2);
			if (bench_wait(ch) < 0)
			{
				g_bench[18] |= 1U << ch;
				G2DMA_EN(ch) = 0;
				break;
			}
			sum += clk_since(t0);
			g_bench[19] |= SB_ISTERR;
			g_bench[12 + ch] += aica_bad(AICA_A, AI_LEN / 4U, 0x100U + ch);
		}
		g_bench[8 + ch] = k ? sum / k : ~0U;
	}
}

/* The first channel that moved every word, 2 and 3 first: 0 is the title's. */
static unsigned int bench_pick(void)
{
	static const unsigned char order[4] = { 2, 3, 0, 1 };
	unsigned int i;

	for (i = 0; i < 4U; i++)
	{
		unsigned int ch = order[i];

		if (!(g_bench[18] & (1U << ch)) && !g_bench[12 + ch] && g_bench[8 + ch] != ~0U)
		{
			return ch;
		}
	}
	return 4U;
}

static void bench_torture(void)
{
	unsigned char *rx = (unsigned char *)gd_stage;
	unsigned char *src = (unsigned char *)gd_stage + 2048U;
	unsigned int ach = bench_pick();
	unsigned int how = g_bench[5] ? 2U : 1U;
	unsigned int r, i;

	g_bench[24] = ach;
	g_bench[25] = how;
	if (ach > 3U)
	{
		return;
	}
	for (r = 0; r < ROUNDS; r++)
	{
		volatile unsigned int *b = AICA_RAM(AICA_B);
		unsigned int seed = 0x1000U + r;

		ram_fill(rx, RX_LEN / 4U, 0);
		CacheBlockInvalidate(rx, RX_LEN / 32U);
		ram_fill(src, AI_LEN / 4U, seed);
		fifo_wait();

		if (how == 1U)
		{
			GAPS_WINPTR = 0x81840000U + GAPS_BENCH_OFF;
		}
		g2dma_start(1, rx, how == 1U ? GAPS_WIN_G2 : GAPS_SRAM_G2 + GAPS_BENCH_OFF,
			    RX_LEN, G2DMA_TO_RAM);
		g2dma_start(ach, src, 0x00800000U + AICA_A, AI_LEN, G2DMA_TO_G2);

		/* The title: sound RAM and a channel register, written then read. */
		g_bench[21] += (g2dma_busy(1) || g2dma_busy(ach));
		for (i = 0; i < 64U; i++)
		{
			if ((i & 7U) == 0U)
			{
				fifo_wait();
			}
			b[i] = pat(i, seed + 7U);
		}
		AICA_CH60_PITCH = seed & 0xffffU;
		fifo_wait();
		for (i = 0; i < 64U; i++)
		{
			g_bench[22] += (aica_rd(b + i) != pat(i, seed + 7U));
		}
		g_bench[22] += ((AICA_CH60_PITCH & 0xffffU) != (seed & 0xffffU));
		g_bench[30] += (g2dma_busy(1) || g2dma_busy(ach));

		g_bench[26] += (bench_wait(1) < 0);
		g_bench[26] += (bench_wait(ach) < 0);
		g_bench[23] += ram_bad(rx, RX_LEN / 4U, 1);
		g_bench[23] += aica_bad(AICA_A, AI_LEN / 4U, seed);
	}
	g_bench[20] = r;

	/* Positive control: one wrong word must be counted once. */
	P2(rx)[5] ^= 0x10U;
	g_bench[27] = ram_bad(rx, RX_LEN / 4U, 1);
}

static void bench_show(unsigned int y, const char *label, unsigned int first)
{
	char num[11];
	unsigned int i;

	clear_lines(y, 24, global_bg_color);
	draw_string(30, y, label, STR_COLOR);
	for (i = 0; i < 4U; i++)
	{
		uint_to_string_dec(g_bench[first + i], num);
		draw_string(150 + 120 * i, y, num, STR_COLOR);
	}
}

void g2dma_bench(void)
{
	static const char *const label[8] = {
		"RX ticks", "RX bad", "AICA dma", "AICA bad",
		"AICA cpu", "Torture", "Tort. 2", "IST/busy",
	};
	unsigned int ch, i;

	for (ch = 0; ch < 4U; ch++)
	{
		g_bench[29] |= g2dma_busy(ch) << ch;
	}
	if (bb == &adapter_bba)
	{
		bench_rx();
	}
	bench_aica();
	if (bb == &adapter_bba)
	{
		bench_torture();
	}
	g_bench[31] = 0x4e423247U;	/* 'G2BN' */
	for (i = 0; i < 8U; i++)
	{
		bench_show(174 + 24 * i, label[i], 4 * i);
	}
}

#endif
