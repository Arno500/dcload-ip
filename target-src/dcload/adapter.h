#ifndef __ADAPTER_H__
#define __ADAPTER_H__

#include "hiram.h"

// Build the HIT-0300 LAN Adapter driver alongside the BBA one. Set from the
// Makefile; the default here is only for a build that does not pass it.
// Dropping it saves about 2 KB of image -- see the Makefile's size section.
#ifndef WITH_LAN_ADAPTER
#define WITH_LAN_ADAPTER 1
#endif

// Raw receive buffer array size
// 1514 bytes is not a multiple of 8.
// Ethernet header (14) + ip header (20) + udp header (8) + command struct (12) = 54 bytes before command->data
// So to align the actual command->data to 8 bytes, need to align the array to 8 bytes and offset the start
// by 2 bytes. 1516 - 2 = 1514, and 54 bytes later (56 bytes from the start of the array) is aligned to 8 bytes since 56 = 8 * 7.
// Also: ip header is aligned to 8 bytes, udp/icmp header is aligned to 4 bytes, and command struct (or other payload header) is aligned to 4 bytes
// Use 1518 because the max data copied over G2 is 1516 by pkt_to_mem, then with the 2-byte shift is 1518.
// But SH4_mem_to_pkt() reads 4 bytes beyond the end of a size, so (1514 + 3)/4 becomes 1516/4, which will read 1520 bytes.
// So we end up with 1520 to account for that. Plus 1520 is the nearest multiple of 8 greater than 1514, too.
// And then you throw caching in, with does things in 32-byte blocks, so we need to extend to the nearest multiple of 32 >1514, or 1536,
// otherwise cache operations will spill over onto adjacent data, which can easily corrupt things.
#define RAW_RX_PKT_BUF_SIZE 1536

// Receive buffer size
#define RX_PKT_BUF_SIZE 1514

// Defines a "network adapter". There will be one of these for each of the
// available drivers.
typedef struct {
	// Driver name
	const char	* name;

	// Mac address
	unsigned char	mac[6];

	// 2 padding bytes to keep function pointers aligned to 4 bytes
	unsigned char pad[2];

	// Check to see if we have one
	int	(*detect)();

	// Initialize the adapter
	int	(*init)();

	// Start network I/O
	void	(*start)();

	// Stop network I/O
	void	(*stop)();

	// Poll for I/O
	void	(*loop)(int is_main_loop);

	// Transmit a packet on the adapter
	int	(*tx)(unsigned char * pkt, int len);
} adapter_t;

// Detect which adapter we are using and init our structs.
int adapter_detect();

/*
 * Hand the current network state to whatever is about to be started, so that a
 * chainloaded dcload can adopt the adapter instead of re-initialising it.
 * BBA only; a no-op on the LAN Adapter, which has nowhere to put it.
 * See the warm-start comment at the top of rtl8139.c.
 */
void adapter_handoff_save(unsigned int ip);

/* Non-zero when this instance adopted an already-running adapter, and the
 * address it inherited from the previous instance (0 if none). */
extern unsigned char g_warm_start;
extern unsigned int g_warm_ip;

// The configured adapter, to be used in all other funcs.
extern adapter_t * bb;
extern adapter_t adapter_la;
extern adapter_t adapter_bba;

// Set this variable to non-zero if you want the loop to exit.
extern volatile unsigned char escape_loop;
// If you want the loop to have a timeout, set this int to # of secs.
// Else, leave it as zero. If loop times out, it will be set to -1 and need resetting.
extern int timeout_loop;

/* When > 0, bb->loop() runs at most this many poll iterations and returns.
 * Used to flush the RX ring before starting a large burst -- see ReadSectors()
 * in cdfs_syscalls.c for why an un-drained ring is not a harmless condition. */
extern volatile int drain_iters;

/* SH4 TMU channel 2's down-counter, at Pck/4 = 12.5 ticks per microsecond,
 * free running from 0xffffffff. The loader's millisecond deadline clock: the
 * CD-DA fetches, the GD read wait and the GD lock watchdog all measure on it.
 *
 * `timeout_loop` counts whole seconds on the performance counter, which is too
 * coarse (a 2 s timeout fires at 3 s, measured on hardware) and reads 0 under
 * an emulator without PMCR support.
 *
 * Only moves once something has started TMU2: cdda.c does before its first
 * fetch, and setup_machine() does at EXEC when ISOLDR_SETUP_MACHINE=1. In the
 * default build nothing else starts it.
 *
 * NOT ALWAYS OURS, AND NOT ALWAYS FROM 0xffffffff (2026-10-02). KOS takes TMU2
 * for its millisecond clock: Pck/4 too, but reloaded from TCOR2 = 1 s
 * (timer_ms_enable()). `start - TMU2_COUNT` then goes "negative" at every
 * reload, and a deadline compared that way fired at once, once a second, in
 * the middle of whatever wait was running. Measure with tmu2_since(). */
#define TMU2_COUNT (*(volatile unsigned int *)0xffd80024)
#define TMU2_TCOR (*(volatile unsigned int *)0xffd80020)

/* Ticks since `start` was read from TMU2_COUNT, across one reload of whatever
 * period TCOR2 holds; with ours (0xffffffff) the plain unsigned subtraction.
 * Right for intervals shorter than the period: 1 s under KOS, so anything
 * measured on TMU2 must be checked at least that often (every deadline here is
 * 838 ms or less, and is polled). */
unsigned int tmu2_since(unsigned int start);

/* When non-zero, bb->loop() gives up this many TMU2 ticks after
 * `fine_deadline_start` was latched, exactly as the seconds deadline does:
 * timeout_loop = -1, escape_loop = 1. Set both, call, then clear. */
extern volatile unsigned int fine_deadline_ticks;
extern volatile unsigned int fine_deadline_start;
extern unsigned int g_fine_timeouts;

/*
 * NO THREAD SWITCH INSIDE A WAIT (2026-09-27).
 *
 * A Katana title's interrupt handlers run on top of us and return. Windows
 * CE's do not: its timer interrupt enters the scheduler, which runs another
 * thread -- for a whole quantum -- while this loop is half way through a
 * wait. Caught under flycast on Sega Rally 2: g_gd_in_transfer 1 with the CPU
 * in another thread's user code, and 49 reads in a row failed, each with an
 * RX overflow and a deadline that ran out while the loader was not running at
 * all (docs/wince-investigation.md 7o). So while the title runs with the MMU
 * on, the adapter loop runs with IMASK 15. Exceptions still reach the title's
 * handlers; nothing here takes one. A wait is ~2 ms, 250 ms at worst.
 *
 * KOS IS PREEMPTIVE TOO (g_gd_kos, 2026-10-02). Its timer interrupt can
 * switch threads in the middle of a disc read's wait, and since the host
 * points KOS's console at this loader (the dcload magic), the next thread's
 * printf would enter dcload's write syscall over the same pkt_buf and take
 * the read's ReturnValue for its own. KOS masks its own dcload syscalls
 * (fs_dcload.c, plain_dclsc), so masking the GD side closes the pair.
 */
extern unsigned int g_gd_kos;

static inline unsigned int bb_irq_hold(void)
{
	unsigned int sr;

	__asm__ volatile ("stc sr,%0" : "=r" (sr));
	if (((*(volatile unsigned int *)0xff000010U & 1U) || g_gd_kos)
	    && (~sr & 0xf0U))
	{
		__asm__ volatile ("ldc %0,sr" : : "r" (sr | 0xf0U) : "memory");
		return sr | 1U << 31;	/* SR bit 31 is reserved: "held" */
	}
	return 0;
}

static inline void bb_irq_restore(unsigned int held)
{
	if (held)
	{
		/* Put back IMASK only; the rest of SR is whatever it is now. */
		unsigned int sr;

		__asm__ volatile ("stc sr,%0" : "=r" (sr));
		sr = (sr & ~0xf0U) | (held & 0xf0U);
		__asm__ volatile ("ldc %0,sr" : : "r" (sr) : "memory");
	}
}

/* Diagnostics for the clock-free deadline in rtl_bb_loop. */
extern unsigned int g_pmcr_backwards;
extern unsigned int g_idle_polls_max;
extern unsigned int g_rx_frames, g_rx_wraps, g_rx_hdr_defer, g_rx_copying;
extern unsigned int g_rx_overflow, g_rx_reinit, g_rx_last_capr, g_rx_last_cbr, g_rx_polls;
extern unsigned int g_rx_linkchange, g_rx_link_giveup, g_rx_underrun_ack;
extern unsigned int g_rx_status_drop, g_rx_last_bad_status, g_rx_missed;
extern unsigned int g_rx_resync;
extern int loop_secs_elapsed;

// All adapter drivers should use this shared buffer to receive.
extern __attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
extern __attribute__((aligned(2))) unsigned char * current_pkt;

/*
// This is useful code to use the perf counters to time stuff (cycle count)
// Using the DCLOAD PMCR in CPU Cycle mode, 1 count = 1 cycle = roughly 5ns (really 1/199.5MHz)

#define LOOP_TIMING

#ifdef LOOP_TIMING
#include "perfctr.h"
#include "video.h"
static unsigned int first_array[2] = {0};
static unsigned int second_array[2] = {0};
static char uint_string_array[9] = {0};
#endif

#ifdef LOOP_TIMING
    PMCR_Read(DCLOAD_PMCR, first_array);
#endif

#ifdef LOOP_TIMING
		PMCR_Read(DCLOAD_PMCR, second_array);
		unsigned int loop_difference = (unsigned int)(*(unsigned long long int*)second_array - *(unsigned long long int*)first_array);

		clear_lines(222, 24, global_bg_color);
		uint_to_string(loop_difference, (unsigned char*)uint_string_array);
		draw_string(30, 222, uint_string_array, STR_COLOR);
#endif
*/

#endif
