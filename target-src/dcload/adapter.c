#include "adapter.h"
#include "rtl8139.h"
#if WITH_LAN_ADAPTER
#include "lan_adapter.h"
#endif

// Loop escape flag, used by all drivers.
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

// The currently configured driver.
adapter_t * bb;

#if !WITH_BBA
// What rtl8139.c defines, for a LAN Adapter only build: no warm start (the
// LAN Adapter always takes the cold path) and no RX DMA.
unsigned char g_warm_start;
unsigned int g_warm_ip;
volatile unsigned int g_rx_dma_tick;
// The adapter loop's limits (adapter.h); the LAN Adapter's loop does not
// read them, so its waits end on the seconds timeout alone.
volatile int drain_iters = 0;
volatile unsigned int fine_deadline_ticks = 0;
volatile unsigned int fine_deadline_start = 0;
unsigned int g_fine_timeouts = 0;
#endif

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
