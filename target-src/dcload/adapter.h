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

/* The network the DHCP lease described: netmask, router, DNS server, each as
 * the number a.b.c.d (like our_ip), 0 when unknown. Set by the instance that
 * took the lease and handed on with the address (rtl_handoff_save()); the host
 * reads it to tell a network title its settings (vnic.c, AGENTS.md 4.21). */
extern unsigned int g_net_cfg[3];
/* A HIGH image that is not resident -- the one doing the hops -- asks the
 * DHCP server for that network (DHCPINFORM, dhcp_inform()) when it holds an
 * address without it: the CD's dcload is older than the carry, whether it
 * left the adapter for a warm start or not (2.0.3 does not: a cold start
 * then takes its address from the host's packets, with no lease). */
#define NET_CFG_INFORM ((DCLOAD_LAYOUT_ID == 1) && !DCLOAD_RESIDENT)
#if NET_CFG_INFORM
/* 1 once the network is known (handed on with its check, or from a lease's
 * ACK) or was asked for; set_ip_dhcp() asks while it is 0. */
extern unsigned char g_net_cfg_known;
#endif

// The configured adapter, to be used in all other funcs.
extern adapter_t * bb;
extern adapter_t adapter_la;
extern adapter_t adapter_bba;
/* Asynchronous disc reads, the RX interrupt and the idle listen serve the LAN
 * Adapter too (its loop keeps the same contract as the BBA's: drain_iters, the
 * fine deadline, bounded waits). GA_LAN=0 (Makefile) returns it to synchronous
 * reads, the A/B for the console: its data port is one byte a G2 access, so a
 * chunk costs a few milliseconds of CPU in the hook's tick. */
#ifndef GA_LAN
#define GA_LAN 1
#endif
#if WITH_LAN_ADAPTER && GA_LAN
#define bb_async_capable() 1
#else
#define bb_async_capable() bb_is_bba()
#endif
/* The live driver is the BBA's: always false in a LAN Adapter only build. */
#if WITH_BBA
#define bb_is_bba() (bb == &adapter_bba)
#else
#define bb_is_bba() 0
#endif

// Set this variable to non-zero if you want the loop to exit.
extern volatile unsigned char escape_loop;
// If you want the loop to have a timeout, set this int to # of secs.
// Else, leave it as zero. If loop times out, it will be set to -1 and need resetting.
extern int timeout_loop;

/* When > 0, bb->loop() runs at most this many poll iterations and returns.
 * Used to flush the RX ring before starting a large burst -- see ReadSectors()
 * in cdfs_syscalls.c for why an un-drained ring is not a harmless condition. */
extern volatile int drain_iters;

/*
 * THE LOADER'S CLOCK: PERFORMANCE COUNTER 1, NOT A TIMER UNIT (2026-10-03).
 *
 * Every interval the loader measures -- the GD read deadline, the GD lock
 * watchdog, the CD-DA fetch and service-gap limits, the hook's periods, the
 * TX waits -- is read here. main() starts the counter in elapsed-time mode
 * counting CPU cycles (PMCR_Init(DCLOAD_PMCR, ...)) and nothing restarts it.
 * 200 MHz is exactly sixteen times Pck/4, so clk_since() returns Pck/4 ticks
 * (12500 a millisecond): every *_TICKS constant, and every counter the host
 * prints as milliseconds, keeps the unit it had on TMU2. The low 32 bits wrap
 * every 21.4 s, far above the longest interval measured (838 ms).
 *
 * It used to be TMU2, which a TITLE CAN TAKE. KOS reloads it every second
 * (2026-10-02). Under Sonic Adventure 2 an asynchronous disc read, whose wait
 * spans the title's frame, was judged 250 ms late one frame after it was
 * posted -- re-asked every 16.7 ms, the same LBA, for seconds, and the ADX
 * music starved -- while a synchronous read, inside one syscall, never was
 * (2026-10-03). The loader no longer programs or reads TMU2 at all.
 *
 * KOS clears this counter once, in its init (perf_cntr_timer_enable()), before
 * a title can read its disc; g_pmcr_backwards counts any later restart.
 *
 * WINDOWS CE TAKES IT (2026-10-08). A graphics module of Sega Rally 2's
 * 0WINCEOS.BIN (file offset 0xc9158, beside "VIEW_PERF") writes PMCR1 = 0xe004
 * and PMCR2 = 0xe001 -- both counters cleared and set to count cache events --
 * and writes them again each time it is called. The counter then counted
 * operand cache misses: every period of the hook's tick stopped elapsing (no
 * idle listen, so --diag and the host's keepalive heard nothing while the
 * title did not read), the tick no longer looked at a read on the wire, and
 * no read deadline could fire; g_irq_tick_sum stopped growing. clk_check()
 * (from the tick and from ReqCmd) sees the counter's mode change and moves
 * the clock to TMU2 for good: CE's kernel touches TMU0 and TMU1 only, and
 * the loader timed CE on TMU2 until 2026-10-03. A Katana or KOS title keeps
 * the counter (TMU2 is theirs, see above). The interval running across the
 * switch is measured wrong once.
 */
#ifdef BUS_RATIO_COUNTER
#error "clk_since() assumes the performance counter counts CPU cycles (200 MHz)"
#endif

/* A start mark for clk_since(): the counter in CPU cycles, or TMU2 (Pck/4,
 * counting down) negated and scaled to the same unit (adapter.c). */
unsigned int clk_now(void);

/* Pck/4 ticks since `start` (from clk_now()). */
unsigned int clk_since(unsigned int start);

/* Move the clock to TMU2 if a title reprogrammed performance counter 1. */
void clk_check(void);
/* PMCR1 as it was found taken, bit 31 set; 0 while the clock is the counter. */
extern unsigned int g_clk_tmu2;

/* When non-zero, bb->loop() gives up this many ticks (clk_since()) after
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
#if WITH_KOS
extern unsigned int g_gd_kos;
#else
#define g_gd_kos 0U	/* a variant without KOS paths (DCLOAD_OS, Makefile) */
#endif

static inline unsigned int bb_irq_hold(void)
{
	unsigned int sr;

	__asm__ volatile ("stc sr,%0" : "=r" (sr));
	if (((WITH_WINCE && (*(volatile unsigned int *)0xff000010U & 1U)) || g_gd_kos)
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
