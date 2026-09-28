/*
 * The interrupt hook: installation and the tick (AGENTS.md 4.15).
 *
 * irq_hook.S has the code that runs on an interrupt and the template copied
 * into the title's vector table. This file decides WHEN that copy happens and
 * refuses every table it does not recognise.
 *
 * WHY IT IS RE-CHECKED ON EVERY GD SYSCALL. A first hook (removed 2026-08-07,
 * docs/loader-comparison.md 2.3) never ran once: it skipped itself while "our"
 * VBR was live, and Sonic Adventure never installs a VBR -- it writes its own
 * handlers into the one it inherits. So: patch the LIVE VBR (stc vbr), and
 * look again at every call, like isoldr's exception_vbr_ok(). A title that
 * re-installs its table is re-hooked (g_irq_rehooks).
 *
 * WHY ONLY TWO PATTERNS. The hook replaces the vector's first three
 * instructions, so irq_out must give back whatever they did:
 *   - Windows CE's `mov.l @(40,r7),r6 ; mov.l @(disp,pc),r0 ; mov r6,r1`,
 *     rebuilt exactly;
 *   - three nops -- the Katana library's entry: it copies six nops, then
 *     `mov.l r0,@-r15 ; mov.l @(disp,pc),r0 ; jmp @r0 ; mov.l r1,@-r15` to
 *     VBR+0x100/+0x400/+0x600 (Crazy Taxi, 2026-09-28; isoldr's katana_entry
 *     relies on the same nops) -- where nothing needs rebuilding.
 * Under them, the trampoline's 24 bytes must be free: zero, or nop padding
 * (our own exception.bin, which Katana titles adopt as their VBR). Anything
 * else is left alone and counted once per VBR (g_irq_refused,
 * g_irq_refused_vbr): the GD path then works as it did without a hook.
 *
 * The tick counts and times itself on every interrupt (phase 1, measured
 * 2026-09-27: ~600 a second on the console), and feeds CD-DA (phase 2).
 */

#include "irq.h"
#include "adapter.h"
#include "memfuncs.h"
#include "cdfs.h"
#include "cdda.h"
#include "rtl8139.h"

#if WITH_IRQ_HOOK

extern unsigned short irq_tramp[], irq_tramp_end[];
extern unsigned int irq_tramp_stack[], irq_tramp_entry[];
extern void irq_entry(void);
extern void irq_icache_flush(void);
extern char maple_dma_buffer[];

/* Where the template goes: it ends exactly at the interrupt vector. */
#define VBR_INT_OFF   0x600U
#define VBR_TRAMP_OFF 0x5e8U
#define TRAMP_FREE    ((VBR_INT_OFF - VBR_TRAMP_OFF) / 2)	/* words under the vector */

/* Windows CE's first three instructions at VBR+0x600, as 16-bit words:
 * mov.l @(40,r7),r6 ; mov.l @(disp=0x22,PC),r0 (= VBR+0x68c) ; mov r6,r1. */
#define CE_INT_0 0x567aU
#define CE_INT_1 0xd022U
#define CE_INT_2 0x6163U
#define SH_NOP   0x0009U

volatile unsigned int g_irq_hooked;		/* the live table carries the hook */
volatile unsigned int g_irq_nop_entry;		/* ... over three nops (Katana), not CE's entry */
volatile unsigned int g_irq_vbr;		/* the table it was last installed in */
volatile unsigned int g_irq_rehooks;		/* installations over a table that had lost it */
volatile unsigned int g_irq_refused;		/* tables not recognised, once per VBR */
volatile unsigned int g_irq_refused_vbr;
volatile unsigned int g_irq_entries;		/* every interrupt through the hook (irq_hook.S) */
volatile unsigned int g_irq_work;		/* non-zero: take the slow path and call irq_tick */
volatile unsigned int g_irq_ticks;		/* irq_tick calls */
volatile unsigned int g_irq_tick_max;		/* longest irq_tick, TMU2 ticks (Pck/4) */
volatile unsigned int g_irq_tick_sum;		/* all of them: the CPU the tick takes */
volatile unsigned int g_irq_evt_last;		/* INTEVT of the last tick */
volatile unsigned int g_irq_rx;			/* BBA RX interrupts taken (Katana) */
volatile unsigned int g_irq_swallow;		/* irq_hook.S: rte, the title never sees it */
volatile unsigned int g_irq_rx_evt;		/* INTEVT the BBA is routed to, 0 = none */
volatile unsigned int g_irq_iml[9];		/* the title's IML2/4/6 NRM,EXT,ERR at install */

/*
 * THE BBA'S RX INTERRUPT (2026-09-28, docs/wince-investigation.md 9o-9p).
 * Under a Katana title the tick only ran on the title's own interrupts --
 * 190 to 790 a second on Crazy Taxi, bunched around the frame -- so a chunk
 * back from the host after ~2.5 ms waited up to 14 ms more to be collected.
 * The chip raises its line already (GAPS 0x1414 = 1, RT_INTRMASK = the RX
 * bits); Holly only needs its EXT bit 3 (KOS's ASIC_EVT_EXP_PCI) routed to a
 * level, and only while a chunk is on the wire. The level: the first of IML6,
 * IML4, IML2 the title leaves empty -- else IML6, shared (Crazy Taxi uses
 * IML4, 9p). The tick acknowledges the chip and returns from the interrupt
 * itself (rte) when nothing of the title's is pending on that level;
 * otherwise the title's handler runs, and finds our bit already clear.
 */
#define SB_IST(n)  (((volatile unsigned int *)0xa05f6900U)[n])	/* NRM, EXT, ERR */
#define SB_IML2    ((volatile unsigned int *)0xa05f6910U)	/* IML4, IML6 follow, 0x10 apart;
								 * INTEVT 0x3a0, 0x360, 0x320 */
#define EXT_BBA    8U
static volatile unsigned int *rx_iml;

void irq_rx_arm(unsigned int on)
{
	if (rx_iml && g_irq_hooked)
	{
		rx_iml[1] = (rx_iml[1] & ~EXT_BBA) | (on ? EXT_BBA : 0U);
	}
}

#define INTEVT (*(volatile unsigned int *)0xff000028U)

/*
 * The template's two literals are filled in the loader's own copy (it is
 * RAM, like cdfs_redir.s' state) before the first installation, so "hooked"
 * is the template word for word and nothing else. dc-integrity.py sees them
 * differ from the ELF, as it does cdfs_redir.s' state words.
 */
void irq_hook_check(void)
{
	unsigned int vbr, sr, i, nops;
	unsigned int n = (unsigned int)(irq_tramp_end - irq_tramp);
	volatile unsigned short *t;

	__asm__ volatile ("stc vbr,%0" : "=r" (vbr));
	t = (volatile unsigned short *)(vbr + VBR_TRAMP_OFF);

	for (i = 0; i < n && t[i] == irq_tramp[i]; i++)
	{
	}
	if (i == n)
	{
		return;
	}

	for (i = 0; i < TRAMP_FREE && (!t[i] || t[i] == SH_NOP); i++)
	{
	}
	nops = t[TRAMP_FREE] == SH_NOP && t[TRAMP_FREE + 1] == SH_NOP
		&& t[TRAMP_FREE + 2] == SH_NOP;
	if ((vbr >> 29) != 4U || i < TRAMP_FREE	/* P1 only: no TLB under BL */
		|| (!nops && (t[TRAMP_FREE] != CE_INT_0
			      || t[TRAMP_FREE + 1] != CE_INT_1
			      || t[TRAMP_FREE + 2] != CE_INT_2)))
	{
		g_irq_hooked = 0;
		if (vbr != g_irq_refused_vbr)
		{
			g_irq_refused_vbr = vbr;
			g_irq_refused++;
		}
		return;
	}

	/* The Maple page's first KB: the part of the MAPL receive buffer no title
	 * ever makes us use, below gd_on_loader_stack()'s stack at the page top.
	 * They must not be one stack: see 9b of docs/wince-investigation.md. */
	irq_tramp_stack[0] = (unsigned int)maple_dma_buffer + 0x400U;
	irq_tramp_entry[0] = (unsigned int)irq_entry;

	/* Masked: an interrupt must not find half a patch. The trampoline and its
	 * literals first, the vector last, each written back, then the icache
	 * invalidated so the next fetch sees the new words. */
	__asm__ volatile ("stc sr,%0" : "=r" (sr));
	__asm__ volatile ("ldc %0,sr" : : "r" (sr | 0xf0U) : "memory");

	for (i = 0; i < n; i++)
	{
		if (i == TRAMP_FREE)
		{
			CacheBlockWriteBack((unsigned char *)((unsigned int)t & ~31U), 1);
		}
		t[i] = irq_tramp[i];
	}
	CacheBlockWriteBack((unsigned char *)((unsigned int)t & ~31U), 2);
	irq_icache_flush();

	if (vbr == g_irq_vbr)
	{
		g_irq_rehooks++;
	}
	g_irq_vbr = vbr;
	g_irq_nop_entry = nops;
	rx_iml = 0;
	g_irq_rx_evt = 0;
	if (nops)
	{
		/* IML2 (IRL 13, INTEVT 0x3a0), IML4 (IRL 11, 0x360), IML6 (IRL 9,
		 * 0x320) -- KOS asic.c: "691x -> irq 13 ... 693x -> irq 9". The
		 * highest free. 9p had IML2 and IML6 swapped, and froze Crazy Taxi
		 * (9q): its own IML2 interrupts were taken for ours and swallowed. */
		for (i = 0; i < 9; i++)
		{
			g_irq_iml[i] = SB_IML2[i + i / 3];
		}
		for (i = 3; i-- > 0 && !rx_iml;)
		{
			if (!(g_irq_iml[3 * i] | g_irq_iml[3 * i + 1] | g_irq_iml[3 * i + 2]))
			{
				rx_iml = SB_IML2 + 4 * i;
			}
		}
		if (!rx_iml)
		{
			rx_iml = SB_IML2 + 8;	/* none: share IML6 */
		}
		g_irq_rx_evt = 0x3a0U - (unsigned int)(rx_iml - SB_IML2) * 0x10U;
	}
	g_irq_work = 1;
	g_irq_hooked = 1;

	__asm__ volatile ("ldc %0,sr" : : "r" (sr) : "memory");
}

/* How often the tick feeds CD-DA: 5 ms of TMU2 (Pck/4). A sub-fetch is 53 ms
 * of audio, so this catches up ten times faster than the audio drains. */
#define IRQ_CDDA_PERIOD 62500U
/* How often it looks at a disc read on the wire: 0.5 ms. A Katana title may
 * take thousands of interrupts a second, and each look drains the RX ring;
 * a 4-sector chunk takes ~2 ms to come back. */
#define IRQ_READ_PERIOD 6250U

void irq_tick(void)
{
	static unsigned int cdda_mark, read_mark;
	unsigned int t0 = TMU2_COUNT;
	unsigned int dt;

	g_irq_ticks++;
	g_irq_evt_last = INTEVT;
	if (g_irq_evt_last == g_irq_rx_evt && (SB_IST(1) & EXT_BBA))
	{
		/* Swallowed only when the chip was pending and nothing of the
		 * title's is: an entry we did not cause always reaches the title,
		 * or a source of its own would re-enter here forever (9q). */
		rtl_irq_ack();
		g_irq_rx++;
		read_mark = t0 + IRQ_READ_PERIOD;	/* look at the read now */
		g_irq_swallow = !((SB_IST(0) & rx_iml[0])
				  | (SB_IST(1) & rx_iml[1] & ~EXT_BBA)
				  | (SB_IST(2) & rx_iml[2]));
	}

	if (gd_lock_state || cdda_busy)
	{
		/* The GD path owns the network -- or CD-DA does: a Katana title's
		 * GetDrvStat services it before taking the lock. */
	}
	else if ((unsigned int)(read_mark - t0) < IRQ_READ_PERIOD && gd_async_busy())
	{
		/* A read is on the wire and was looked at a moment ago. */
	}
	else if ((read_mark = t0, gd_async_tick()))
	{
		/* PHASE 3: a disc read is on the wire; it has the network until
		 * its verdict, and CD-DA declines meanwhile (g_gd_in_transfer). */
	}
#if WITH_CDDA
	else if ((unsigned int)(cdda_mark - t0) >= IRQ_CDDA_PERIOD)
	{
		cdda_mark = t0;
		cdda_service_tick();
	}
#endif

	dt = t0 - TMU2_COUNT;
	g_irq_tick_sum += dt;
	if (dt > g_irq_tick_max)
	{
		g_irq_tick_max = dt;
	}
}

#endif
