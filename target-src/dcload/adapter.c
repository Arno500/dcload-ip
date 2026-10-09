#include "adapter.h"
#include "rtl8139.h"
#if WITH_LAN_ADAPTER
#include "lan_adapter.h"
#endif
#include "perfctr.h"

// Loop escape flag, used by all drivers.
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

// The currently configured driver.
adapter_t * bb;

#if !WITH_BBA
// What rtl8139.c defines, for a LAN Adapter only build: no BBA warm start
// and no RX DMA.
unsigned char g_warm_start;
unsigned int g_warm_ip;
volatile unsigned int g_rx_dma_tick;
#endif

// The loop contract both drivers keep (adapter.h): a bounded drain, the fine
// deadline, and the counters that say how a wait ended.
volatile int drain_iters = 0;
volatile unsigned int fine_deadline_ticks = 0;
volatile unsigned int fine_deadline_start = 0;
unsigned int g_fine_timeouts = 0;
// Times the performance counter read lower than it had before. Non-zero means
// the clock cannot be trusted for deadlines on this machine.
unsigned int g_pmcr_backwards = 0;
// Poll iterations spent with nothing received, at the last timeout.
unsigned int g_idle_polls_max = 0;

// The loader's clock (adapter.h): performance counter 1, or TMU2 once a title
// has taken the counter. TMU registers as longs from TOCR (0xffd80000).
#define CLK_PMCTR1L (*(volatile unsigned int *)0xff100008)
#define CLK_PMCR1 (*(volatile unsigned short *)PMCR1_CTRL_REG)
#define CLK_TMU ((volatile unsigned int *)0xffd80000)
#define CLK_TCNT2 (CLK_TMU[0x24 / 4])
unsigned int g_clk_tmu2 = 0;

unsigned int clk_now(void)
{
	if (g_clk_tmu2)
	{
		/* Down at Pck/4 from 0xffffffff: negated and << 4, it is the counter's
		 * unit, and wraps at the same 2^32 (every 21.4 s). */
		return ~CLK_TCNT2 << 4;
	}
	return CLK_PMCTR1L;
}

unsigned int clk_since(unsigned int start)
{
	return (clk_now() - start) >> 4;
}

void clk_check(void)
{
	/* The mode and the clock select read back; PMEN/PMST are not relied on. */
	unsigned int pmcr = CLK_PMCR1;
	volatile unsigned int *tmu = CLK_TMU;

	if (g_clk_tmu2 || (pmcr & 0x013fU) == PMCR_ELAPSED_TIME_MODE)
	{
		return;
	}
	((volatile unsigned short *)tmu)[0x28 / 2] = 0;	/* TCR2: Pck/4, no interrupt */
	tmu[0x20 / 4] = 0xffffffffU;			/* TCOR2 */
	tmu[0x24 / 4] = 0xffffffffU;			/* TCNT2 */
	((volatile unsigned char *)tmu)[4] |= 4U;	/* TSTR: start TMU2 */
	g_clk_tmu2 = pmcr | 0x80000000U;
}

// Packet receive buffer. HIRAM_BUF keeps its 1536 bytes out of the loader
// image, where they would otherwise sit directly in the path of a title's
// stack -- see hiram.h and AGENTS.md 4.6.
HIRAM_BUF __attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE]; // Here's a global array. Global packet receive buffer.
// Need to offset the packet by 2 for the command->data after headers to always be aligned to 8 bytes
// The performance gains are well worth the 2 wasted bytes.
__attribute__((aligned(2))) unsigned char * current_pkt = &(raw_current_pkt[2]);

int adapter_detect() {
	// Try the BBA first.
	if (0) {
#if WITH_BBA
	} else if (adapter_bba.detect() >= 0) {
		bb = &adapter_bba;
#endif
#if WITH_LAN_ADAPTER
	} else if (adapter_la.detect() >= 0) {
		bb = &adapter_la;
#endif
	} else {
		return -1;
	}

	// Initialize the chosen adapter.
	if (bb->init() < 0)
		return -1;

	escape_loop = 0;

	return 0;
}

void adapter_handoff_save(unsigned int ip)
{
#if WITH_BBA
	if (bb_is_bba())
	{
		rtl_handoff_save(ip);
	}
#else
	(void)ip;
#endif
}
